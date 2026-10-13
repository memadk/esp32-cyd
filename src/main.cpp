// Sonos controller for the kitchen on a Sunton ESP32-2432S024C.
//   core 1 (Arduino loop): LVGL UI, display, touch, backlight
//   core 0 (sonosTask):    Wi-Fi, OTA, Sonos SOAP, UPnP event listener, album-art download/decode
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <WiFiServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "lgfx_cyd_2432s024c.hpp"
#include "secrets.h"
#include "app.h"
#include "sonos.h"
#include "ui.h"
#include "config.h"
#include "rlog.h"
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <esp_app_desc.h>

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef TOUCH_FLIP_X
#define TOUCH_FLIP_X 0
#endif
#ifndef TOUCH_FLIP_Y
#define TOUCH_FLIP_Y 0
#endif

static constexpr uint16_t EVENT_PORT   = 3400;              // UPnP NOTIFY listener
static constexpr uint32_t POLL_MS      = 20000;             // safety-net poll; events drive normal updates
static constexpr uint32_t TOPOLOGY_MS  = 60000;             // safety-net topology refresh

static LGFX_CYD_2432S024C lcd;
static LGFX_Sprite artSprite(&lcd);
static uint16_t *artBuf = nullptr;
static SonosClient sonos;
static Preferences prefs;
static WiFiServer eventServer(EVENT_PORT);

// ---------------- shared state (guarded by g_mutex) ----------------
struct Shared {
    String status;      bool statusDirty = false;
    SonosState state;   bool stateDirty  = false;
    std::vector<SonosFavorite> favs;  bool favsDirty  = false;
    std::vector<ZoneGroupInfo> groups; bool groupsDirty = false;
    std::vector<QueueItem> queue; int queueTotal = 0; int queueCurrent = 0; bool queueDirty = false;
    String room;        bool roomDirty  = false;
    String pendingRoom;                                  // requested from the web page
    bool artDirty = false; bool artValid = false;
    bool ready = false; bool wifiOk = false;
} g;
static SemaphoreHandle_t g_mutex;
static QueueHandle_t g_cmdQueue;
static volatile bool g_ota = false;
struct Cmd { AppCmd type; int value; };
static void ensureSubscriptions(bool force);   // defined in the GENA section

static void setStatus(const String &s) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g.status = s; g.statusDirty = true;
    xSemaphoreGive(g_mutex);
    log_i("[status] %s", s.c_str());
}

void app_send(AppCmd cmd, int value) {
    Cmd c{cmd, value};
    xQueueSend(g_cmdQueue, &c, 0);
}

// ---------------- LVGL <-> LovyanGFX glue ----------------
static constexpr int SCREEN_W = 240, SCREEN_H = 320, BUF_LINES = 20;
static constexpr size_t BUF_BYTES = SCREEN_W * BUF_LINES * sizeof(lv_color16_t);
static lv_color16_t *draw_buf1, *draw_buf2;
static volatile bool screenAsleep = false;
static bool swallowTouch = false;
static SonosTransport uiTransport = SonosTransport::Unknown;

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels(reinterpret_cast<lgfx::rgb565_t *>(px_map), w * h);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}

// Touch position in screen coordinates. LovyanGFX rotates touch coordinates together with the panel
// (lcd.setRotation), so the fixed panel-vs-touch-glass mirroring applies unchanged for every rotation. The CST820
// on the kitchen board reports taps near the top edge ~35 px too low, so a per-axis linear calibration follows.
static bool readTouch(int *sx, int *sy) {             // mirrored, not yet calibrated
    uint16_t x, y;
    if (!lcd.getTouch(&x, &y)) return false;
    *sx = TOUCH_FLIP_X ? SCREEN_W - 1 - x : x;
    *sy = TOUCH_FLIP_Y ? SCREEN_H - 1 - y : y;
    return true;
}
static void calibrateTouch(int *sx, int *sy) {
    int ix = *sx, iy = *sy;
    if (cfg.tcRot == cfg.rotation) { ix = ix * cfg.tcAx / 1000 + cfg.tcBx; iy = iy * cfg.tcAy / 1000 + cfg.tcBy; }
    *sx = ix < 0 ? 0 : ix >= SCREEN_W ? SCREEN_W - 1 : ix;
    *sy = iy < 0 ? 0 : iy >= SCREEN_H ? SCREEN_H - 1 : iy;
}

static void lv_touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    int tx, ty;
    bool pressed = readTouch(&tx, &ty);
    if (screenAsleep) {
        if (pressed) { screenAsleep = false; swallowTouch = true; lv_display_trigger_activity(nullptr); }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (swallowTouch) {
        if (!pressed) swallowTouch = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (pressed) {
        calibrateTouch(&tx, &ty);
        data->state = LV_INDEV_STATE_PRESSED; data->point.x = tx; data->point.y = ty;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

// ---------------- album art (Sonos task) ----------------
// Presents an HTTP response body as a plain Stream: handles Transfer-Encoding: chunked and Content-Length,
// so the JPEG/PNG decoder can consume the image straight from the socket without buffering it in RAM.
class HttpBodyStream : public Stream {
public:
    // Whole-body deadline: a trickling download must not hold the (watchdog-supervised) network task for > 60 s.
    HttpBodyStream(WiFiClient *c, bool chunked, int contentLength, uint32_t deadlineMs = 30000)
        : _c(c), _chunked(chunked), _remaining(chunked ? 0 : contentLength), _needHeader(chunked), _deadline(millis() + deadlineMs) {}
    bool timedOut() const { return _timedOut; }

    int available() override {
        if (_done) return 0;
        if (_chunked && _needHeader && !readChunkHeader(false)) return 0;
        int a = _c->available();
        if (_remaining >= 0 && a > _remaining) a = _remaining;
        return a;
    }
    int read() override { uint8_t b; return readBytes(reinterpret_cast<char *>(&b), 1) == 1 ? b : -1; }
    int peek() override { return (available() > 0) ? _c->peek() : -1; }
    size_t write(uint8_t) override { return 0; }

    size_t readBytes(char *buf, size_t len) override {
        size_t n = 0;
        uint32_t t0 = millis();
        while (n < len && !_done && millis() - t0 < 4000) {
            if ((int32_t)(millis() - _deadline) > 0) { _timedOut = true; _done = true; break; }
            if (_chunked && _needHeader) { if (!readChunkHeader(true)) { if (_done) break; continue; } }
            int avail = _c->available();
            if (avail <= 0) { if (!_c->connected()) { _done = true; break; } delay(1); continue; }
            int want = len - n;
            if (_remaining >= 0 && want > _remaining) want = _remaining;
            if (want > avail) want = avail;
            int got = _c->read(reinterpret_cast<uint8_t *>(buf) + n, want);
            if (got <= 0) { delay(1); continue; }
            n += got;
            t0 = millis();
            esp_task_wdt_reset();                      // progress: keep the task watchdog quiet while data trickles in
            if (_remaining > 0) {
                _remaining -= got;
                if (_remaining == 0) { if (_chunked) { skipCrlf(); _needHeader = true; } else _done = true; }
            }
        }
        return n;
    }
    size_t readBytes(uint8_t *buf, size_t len) { return readBytes(reinterpret_cast<char *>(buf), len); }

private:
    bool readChunkHeader(bool blocking) {              // "<hex>\r\n"; 0 = end of body
        uint32_t t0 = millis();
        String line;
        while (millis() - t0 < 4000) {
            int ch = _c->available() > 0 ? _c->read() : -1;
            if (ch < 0) { if (!blocking) return false; if (!_c->connected()) { _done = true; return false; } delay(1); continue; }
            if (ch == '\n') {
                line.trim();
                if (line.isEmpty()) continue;           // CRLF left over from the previous chunk
                long sz = strtol(line.c_str(), nullptr, 16);
                if (sz <= 0) { _done = true; return false; }
                _remaining = sz; _needHeader = false;
                return true;
            }
            if (ch != '\r') line += (char)ch;
        }
        _done = true;
        return false;
    }
    void skipCrlf() {
        uint32_t t0 = millis();
        int skipped = 0;
        while (skipped < 2 && millis() - t0 < 1000) {
            if (_c->available() > 0) { int ch = _c->read(); if (ch == '\r' || ch == '\n') skipped++; else break; }
            else delay(1);
        }
    }

    WiFiClient *_c;
    bool _chunked;
    int _remaining;        // bytes left in the current chunk / body; -1 = until close
    bool _needHeader;
    bool _done = false;
    uint32_t _deadline;
    bool _timedOut = false;
};

static bool fetchArt(const String &url) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(8000);
    const char *hdrs[] = {"Content-Type", "Transfer-Encoding"};
    http.collectHeaders(hdrs, 2);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); log_w("art GET %d", code); return false; }
    String ct = http.header("Content-Type");
    bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
    int size = http.getSize();
    HttpBodyStream body(http.getStreamPtr(), chunked, chunked ? 0 : size);

    artSprite.fillSprite(TFT_BLACK);
    bool png = ct.indexOf("png") >= 0;
    // zoom 0 = fit inside ART_SIZE x ART_SIZE, centred; the decoder streams straight from the socket
    bool ok = png ? artSprite.drawPng(&body, 0, 0, ART_SIZE, ART_SIZE, 0, 0, 0.0f, 0.0f, lgfx::middle_center)
                  : artSprite.drawJpg(&body, 0, 0, ART_SIZE, ART_SIZE, 0, 0, 0.0f, 0.0f, lgfx::middle_center);
    http.end();
    if (body.timedOut()) { ok = false; log_w("art: for langsomt, opgivet efter 30 s"); }
    if (ok) for (int i = 0; i < ART_SIZE * ART_SIZE; i++) artBuf[i] = __builtin_bswap16(artBuf[i]);   // sprite is big-endian
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g.artValid = ok; g.artDirty = true;
    xSemaphoreGive(g_mutex);
    log_i("art %s (%s%s), free heap %lu, largest %lu", ok ? "ok" : "DECODE FAILED", png ? "png" : "jpg",
          chunked ? ", chunked" : "", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    return ok;
}

// ---------------- configuration (NVS "cfg") ----------------
AppConfig cfg;
volatile bool cfgRotationDirty = false;
static Preferences cfgPrefs;

void config_load() {
    cfgPrefs.begin("cfg", false);
    auto geti = [](const char *k, int d) { return cfgPrefs.isKey(k) ? cfgPrefs.getInt(k, d) : d; };
    cfg.blBright = geti("blBright", cfg.blBright);
    cfg.blNormal = geti("blNormal", cfg.blNormal);
    cfg.blDark   = geti("blDark", cfg.blDark);
    cfg.blIdle   = geti("blIdle", cfg.blIdle);
    cfg.idleMin  = geti("idleMin", cfg.idleMin);
    cfg.sleepMin = geti("sleepMin", cfg.sleepMin);
    cfg.volStep  = geti("volStep", cfg.volStep);
    cfg.rotation = geti("rotation", cfg.rotation);
    cfg.otaPassword = cfgPrefs.isKey("otaPass") ? cfgPrefs.getString("otaPass", "") : String();
    if (cfgPrefs.isKey("logHost")) cfg.logHost = cfgPrefs.getString("logHost", cfg.logHost);
    cfg.logPort = geti("logPort", cfg.logPort);
    cfg.tcAx = geti("tcAx", cfg.tcAx); cfg.tcBx = geti("tcBx", cfg.tcBx);
    cfg.tcAy = geti("tcAy", cfg.tcAy); cfg.tcBy = geti("tcBy", cfg.tcBy);
    cfg.tcRot = geti("tcRot", cfg.tcRot);
}
void config_save() {
    cfgPrefs.putInt("blBright", cfg.blBright); cfgPrefs.putInt("blNormal", cfg.blNormal);
    cfgPrefs.putInt("blDark", cfg.blDark);     cfgPrefs.putInt("blIdle", cfg.blIdle);
    cfgPrefs.putInt("idleMin", cfg.idleMin);   cfgPrefs.putInt("sleepMin", cfg.sleepMin);
    cfgPrefs.putInt("volStep", cfg.volStep);   cfgPrefs.putInt("rotation", cfg.rotation);
    cfgPrefs.putString("otaPass", cfg.otaPassword);
    cfgPrefs.putString("logHost", cfg.logHost); cfgPrefs.putInt("logPort", cfg.logPort);
    cfgPrefs.putInt("tcAx", cfg.tcAx); cfgPrefs.putInt("tcBx", cfg.tcBx);
    cfgPrefs.putInt("tcAy", cfg.tcAy); cfgPrefs.putInt("tcBy", cfg.tcBy); cfgPrefs.putInt("tcRot", cfg.tcRot);
}

static WebServer cfgWeb(80);

static String htmlEscape(const String &s) { return xmlEscape(s); }

static void cfgField(String &h, const char *label, const char *name, int value, int lo, int hi, const char *hint = "") {
    h += "<label>"; h += label; h += "</label><input type=number name="; h += name; h += " value=";
    h += String(value); h += " min="; h += String(lo); h += " max="; h += String(hi); h += ">";
    if (*hint) { h += "<small>"; h += hint; h += "</small>"; }
}

// Sends `h` as HTTP chunks of at most ~600 bytes and clears it. Large single writes fail (EAGAIN) when the
// fragmented heap cannot give lwIP a big enough send buffer, so the page is streamed piecewise.
static void cfgFlush(String &h, bool final = false) {
    size_t pos = 0;
    while (pos < h.length()) {
        size_t n = std::min((size_t)600, h.length() - pos);
        cfgWeb.sendContent(h.substring(pos, pos + n));
        pos += n;
        delay(1);
    }
    h = "";
    if (final) cfgWeb.sendContent("");
}

static void cfgSendPage(const String &room, const std::vector<ZoneGroupInfo> &groups) {
    cfgWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
    cfgWeb.send(200, "text/html; charset=utf-8", "");
    String h;
    h.reserve(900);
    h += F("<!doctype html><html lang=da><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Sonos-Remote</title><style>body{font-family:-apple-system,system-ui,sans-serif;background:#101820;color:#f2f4f7;margin:0;padding:22px;max-width:560px}"
           "h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;color:#8a94a6;margin:26px 0 8px;border-bottom:1px solid #2a3646;padding-bottom:6px}"
           "label{display:block;margin:12px 0 4px;color:#8a94a6}small{color:#5d6a7d;display:block;margin-top:3px}");
    cfgFlush(h);
    h += F("select,input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border-radius:10px;border:1px solid #2a3646;background:#1b2634;color:#f2f4f7}"
           "button{width:100%;font-size:16px;padding:12px;border-radius:10px;border:0;background:#3b82f6;color:#fff;margin-top:18px;font-weight:600}"
           ".warn{background:#3a2a2a}.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}.stat{color:#8a94a6;font-size:14px;line-height:1.6}</style></head><body>"
           "<h1>Sonos-Remote</h1><div class=stat>");
    h += "Rum: <b>" + htmlEscape(room) + "</b> &middot; IP " + WiFi.localIP().toString() + " &middot; ";
    h += "Fri hukommelse " + String(ESP.getFreeHeap() / 1024) + " KB &middot; Oppetid " + String(millis() / 60000) + " min<br>Build ";
    h += __DATE__; h += " "; h += __TIME__;
    cfgFlush(h);
    h += F("</div><form method=post action=/save><h2>Rum</h2><label>Standardrum</label><select name=room>");
    bool listed = false;
    for (const auto &g : groups) for (const auto &n : g.zoneNames) {
        h += "<option"; if (n == room) { h += " selected"; listed = true; } h += ">"; h += htmlEscape(n); h += "</option>";
    }
    if (!listed) { h += "<option selected>"; h += htmlEscape(room); h += "</option>"; }
    h += F("</select><h2>Skærm</h2><div class=grid>");
    cfgFlush(h);
    cfgField(h, "Lys i lyst rum", "blBright", cfg.blBright, 5, 255);
    cfgField(h, "Lys i normalt rum", "blNormal", cfg.blNormal, 5, 255);
    cfgField(h, "Lys i mørkt rum", "blDark", cfg.blDark, 5, 255);
    cfgField(h, "Lys når inaktiv", "blIdle", cfg.blIdle, 0, 255);
    cfgFlush(h);
    cfgField(h, "Dæmp efter (min)", "idleMin", cfg.idleMin, 0, 120, "0 = aldrig");
    cfgField(h, "Sluk skærm efter (min)", "sleepMin", cfg.sleepMin, 0, 600, "kun når intet spiller, 0 = aldrig");
    h += F("</div><label>Vending</label><select name=rotation>");
    h += String("<option value=0") + (cfg.rotation == 0 ? " selected" : "") + ">USB-stik opad</option>";
    h += String("<option value=2") + (cfg.rotation == 2 ? " selected" : "") + ">USB-stik nedad (stander)</option>";
    h += F("</select><h2>Lyd</h2>");
    cfgField(h, "Lydstyrke pr. tryk", "volStep", cfg.volStep, 1, 10);
    cfgFlush(h);
    h += F("<h2>Fjernlog</h2><label>Logserver (vært eller IP)</label><input name=logHost value='");
    h += htmlEscape(cfg.logHost);
    h += F("'><small>Tom = slukket. Modtag med tools/logserver.py på Mac'en.</small>");
    cfgField(h, "UDP-port", "logPort", cfg.logPort, 1, 65535);
    cfgFlush(h);
    h += F("<h2>Opdatering</h2><label>OTA-kodeord</label><input name=otaPass type=password autocomplete=new-password placeholder='uændret'>"
           "<small>Tomt = uændret. Skriv et enkelt minus (-) for at fjerne kodeordet. Gælder efter genstart.</small>"
           "<button type=submit>Gem</button></form>"
           "<h2>Touch</h2>");
    h += cfg.tcRot == cfg.rotation ? F("<div class=stat>Kalibreret.</div>") : F("<div class=stat>Ikke kalibreret.</div>");
    h += F("<form method=post action=/calibrate><button type=submit>Kalibrér touch (tryk på 5 kryds på skærmen)</button></form>"
           "<form method=post action=/calibrate><input type=hidden name=reset value=1><button class=warn type=submit>Nulstil kalibrering</button></form>"
           "<form method=post action=/wifi><button class=warn type=submit>Skift Wi-Fi (genstarter i opsætning)</button></form>"
           "<form method=post action=/reboot><button class=warn type=submit>Genstart</button></form></body></html>");
    cfgFlush(h, true);
}

// Firmware update: hand the updater every big buffer it does not need. On the kitchen board the album-art sprite
// alone is ART_SIZE^2 * 2 bytes (~39 KB); with it and the Sonos buffers gone the heap looks like the info board's
// (~95 KB), which takes OTA without trouble. Six OTA attempts failed at 0 % before this: lwIP could not even buffer
// the incoming TCP data (same starvation as the evening "Not enough memory to allocate buffer" episodes).
// Both paths (ArduinoOTA and HTTP /update) run on the Sonos task, so no art decode is in flight here; the UI task
// on core 1 hides the image via artDirty before the buffer is freed. A failed update ends in a restart: the board is
// half torn down by then and a reboot is the cleanest way back (an earlier version froze at "Opdaterer firmware...").
static void otaPrepare(const char *status, bool stopWeb) {
    g_ota = true;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g.ready = false; g.status = status; g.statusDirty = true;
    g.artValid = false; g.artDirty = true;                 // UI hides img_art on its next tick
    xSemaphoreGive(g_mutex);
    delay(250);                                            // let core 1 finish a frame that may still read artBuf
    if (artBuf) { heap_caps_free(artBuf); artBuf = nullptr; }
    if (stopWeb) cfgWeb.stop();
    eventServer.end();
    sonos.releaseBuffers();
    log_i("update: buffers released, free heap %lu, largest %lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
}
static void otaFailed(const String &what) {
    log_e("%s, genstarter", what.c_str());
    setStatus(what + ", genstarter...");
    delay(1500);
    ESP.restart();
}

static void setupConfigWeb() {
    cfgWeb.on("/", HTTP_GET, []() {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        String room = g.room; std::vector<ZoneGroupInfo> groups = g.groups;
        xSemaphoreGive(g_mutex);
        cfgSendPage(room, groups);
    });
    cfgWeb.on("/save", HTTP_POST, []() {
        auto geti = [](const char *k, int lo, int hi, int cur) { if (!cfgWeb.hasArg(k)) return cur; int v = cfgWeb.arg(k).toInt(); return v < lo ? lo : v > hi ? hi : v; };
        cfg.blBright = geti("blBright", 5, 255, cfg.blBright);
        cfg.blNormal = geti("blNormal", 5, 255, cfg.blNormal);
        cfg.blDark   = geti("blDark", 5, 255, cfg.blDark);
        cfg.blIdle   = geti("blIdle", 0, 255, cfg.blIdle);
        cfg.idleMin  = geti("idleMin", 0, 120, cfg.idleMin);
        cfg.sleepMin = geti("sleepMin", 0, 600, cfg.sleepMin);
        cfg.volStep  = geti("volStep", 1, 10, cfg.volStep);
        int rot = geti("rotation", 0, 2, cfg.rotation);
        if (rot != cfg.rotation) { cfg.rotation = rot == 2 ? 2 : 0; cfgRotationDirty = true; }
        if (cfgWeb.hasArg("logHost")) { String lh = cfgWeb.arg("logHost"); lh.trim(); cfg.logHost = lh; }
        cfg.logPort = geti("logPort", 1, 65535, cfg.logPort);
        rlog_set_target(cfg.logHost, cfg.logPort);
        String op = cfgWeb.arg("otaPass");
        if (op == "-") cfg.otaPassword = ""; else if (op.length()) cfg.otaPassword = op;
        config_save();
        String room = cfgWeb.arg("room"); room.trim();
        if (room.length()) {
            xSemaphoreTake(g_mutex, portMAX_DELAY); bool changed = room != g.room; g.pendingRoom = room; xSemaphoreGive(g_mutex);
            if (changed) app_send(AppCmd::SetHomeRoom);
        }
        cfgWeb.sendHeader("Location", "/", true);
        cfgWeb.send(303, "text/plain", "");
    });
    cfgWeb.on("/wifi", HTTP_POST, []() {
        cfgWeb.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='font-family:sans-serif;padding:24px'>Genstarter i Wi-Fi-opsætning. Forbind til \"Sonos-Remote\".</body>");
        delay(300);
        app_send(AppCmd::WifiSetup);
    });
    cfgWeb.on("/calibrate", HTTP_POST, []() {
        if (cfgWeb.hasArg("reset")) { cfg.tcRot = -1; config_save(); log_i("touch calibration cleared"); }
        else touchCalRequest = true;
        cfgWeb.sendHeader("Location", "/", true);
        cfgWeb.send(303, "text/plain", "");
    });
    cfgWeb.on("/reboot", HTTP_POST, []() {
        cfgWeb.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='font-family:sans-serif;padding:24px'>Genstarter...</body>");
        delay(300);
        ESP.restart();
    });

    // Firmware upload over HTTP: tolerant of poor Wi-Fi links (no per-chunk timeout like espota).
    //   curl -F firmware=@.pio/build/<env>/firmware.bin http://<host>/update       (tools/httpupdate.sh)
    cfgWeb.on("/update", HTTP_POST, []() {
        bool ok = Update.isFinished() && !Update.hasError();
        cfgWeb.send(200, "text/plain; charset=utf-8", ok ? "OK - genstarter" : String("FEJL: ") + Update.errorString() + " - genstarter");
        delay(500);
        if (ok) ESP.restart();
        Update.abort();
        otaFailed(String("HTTP-opdatering fejlede: ") + Update.errorString());
    }, []() {
        HTTPUpload &up = cfgWeb.upload();
        if (up.status == UPLOAD_FILE_START) {
            if (cfg.otaPassword.length() && !cfgWeb.authenticate("admin", cfg.otaPassword.c_str())) { cfgWeb.requestAuthentication(); return; }
            otaPrepare("Opdaterer firmware (HTTP)...", false);       // the web server itself carries the upload
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) log_e("update begin: %s", Update.errorString());
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (!Update.hasError() && Update.write(up.buf, up.currentSize) != up.currentSize) log_e("update write: %s", Update.errorString());
            esp_task_wdt_reset();
            static uint32_t last = 0;
            if (millis() - last > 500) { last = millis(); setStatus("Opdaterer firmware... " + String(Update.progress() / 1024) + " KB"); }
        } else if (up.status == UPLOAD_FILE_END) {
            if (Update.end(true)) log_i("update ok: %u bytes", up.totalSize); else log_e("update end: %s", Update.errorString());
        } else if (up.status == UPLOAD_FILE_ABORTED) {
            Update.abort();
            otaFailed("HTTP-opdatering afbrudt");
        }
    });
    cfgWeb.begin();
}

// ---------------- OTA ----------------
static void setupOta() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setTimeout(10000);   // default 1 s of silence = "OTA fejl 3"; the kitchen link has multi-second stalls
    if (cfg.otaPassword.length()) ArduinoOTA.setPassword(cfg.otaPassword.c_str());
    else if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { otaPrepare("Opdaterer firmware...", true); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        esp_task_wdt_reset();
        static uint32_t last = 0;
        if (millis() - last > 400) { last = millis(); setStatus("Opdaterer firmware... " + String(done * 100 / total) + " %"); }
    });
    ArduinoOTA.onEnd([]() { setStatus("Genstarter..."); });
    ArduinoOTA.onError([](ota_error_t err) { otaFailed("OTA fejl " + String((int)err)); });
    ArduinoOTA.begin();
    Serial.printf("[ota] ready: %s.local (%s)\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// ---------------- UPnP event subscriptions (GENA) ----------------
struct Subscription {
    const char *eventPath;
    const char *callbackPath;
    bool onCoordinator;      // else on the room's own player
    IPAddress ip;
    String sid;
    uint32_t renewAt = 0;
    bool active = false;
};
static Subscription subs[] = {
    {"/MediaRenderer/AVTransport/Event",           "/evt/avt", true},
    {"/MediaRenderer/GroupRenderingControl/Event", "/evt/grc", true},
    {"/ZoneGroupTopology/Event",                   "/evt/zgt", false},
};
static volatile bool eventPollDue = false, topologyDue = false;

static void ensureSubscriptions(bool force = false) {
    String base = "http://" + WiFi.localIP().toString() + ":" + String(EVENT_PORT);
    for (auto &s : subs) {
        IPAddress want = s.onCoordinator ? sonos.coordinator() : sonos.roomPlayer();
        if ((uint32_t)want == 0) continue;
        if (s.active && (s.ip != want || force)) {
            sonos.unsubscribe(s.ip, s.eventPath, s.sid);
            s.active = false;
        }
        uint32_t timeout = 0;
        if (!s.active) {
            s.ip = want;
            if (sonos.subscribe(s.ip, s.eventPath, base + s.callbackPath, s.sid, timeout)) {
                s.active = true;
                s.renewAt = millis() + timeout * 500;      // renew at half the granted lifetime
                log_i("[gena] subscribed %s on %s (%lu s)", s.callbackPath, s.ip.toString().c_str(), (unsigned long)timeout);
            }
        } else if ((int32_t)(millis() - s.renewAt) >= 0) {
            if (sonos.renewSubscription(s.ip, s.eventPath, s.sid, timeout)) s.renewAt = millis() + timeout * 500;
            else { s.active = false; log_w("[gena] renew failed %s, resubscribing", s.callbackPath); }
        }
    }
}

static void dropSubscriptions() {
    for (auto &s : subs) { if (s.active) sonos.unsubscribe(s.ip, s.eventPath, s.sid); s.active = false; }
}

// Handle one incoming NOTIFY. Bodies are only parsed for the small GroupRenderingControl events;
// everything else just triggers a targeted re-read over SOAP.
static void handleEventClient(WiFiClient &c) {
    c.setTimeout(1500);
    String reqLine = c.readStringUntil('\n');
    int sp1 = reqLine.indexOf(' '), sp2 = reqLine.indexOf(' ', sp1 + 1);
    String method = sp1 > 0 ? reqLine.substring(0, sp1) : "";
    String path = (sp1 > 0 && sp2 > sp1) ? reqLine.substring(sp1 + 1, sp2) : "";
    int contentLength = 0;
    for (int i = 0; i < 40; i++) {
        String h = c.readStringUntil('\n');
        h.trim();
        if (h.isEmpty()) break;
        String u = h; u.toUpperCase();
        if (u.startsWith("CONTENT-LENGTH:")) contentLength = h.substring(15).toInt();
    }
    bool wantBody = path == "/evt/grc" && contentLength > 0 && contentLength < 6000;
    String body;
    if (wantBody) body.reserve(contentLength + 1);
    int remaining = contentLength;
    uint32_t t0 = millis();
    uint8_t buf[256];
    while (remaining > 0 && millis() - t0 < 2000) {
        int avail = c.available();
        if (avail <= 0) { delay(2); continue; }
        int n = c.readBytes(buf, std::min({avail, remaining, (int)sizeof(buf)}));
        if (n <= 0) break;
        if (wantBody) body.concat(reinterpret_cast<const char *>(buf), n);
        remaining -= n;
    }
    c.print("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    c.flush();
    c.stop();

    if (method != "NOTIFY") return;
    log_i("[gena] NOTIFY %s (%d bytes)", path.c_str(), contentLength);
    if (path == "/evt/grc") {
        String vol = xmlTag(body, "GroupVolume"), mute = xmlTag(body, "GroupMute");
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (vol.length()) g.state.volume = vol.toInt();
        if (mute.length()) g.state.muted = mute == "1";
        g.stateDirty = true;
        xSemaphoreGive(g_mutex);
    } else if (path == "/evt/avt") {
        eventPollDue = true;
    } else if (path == "/evt/zgt") {
        topologyDue = true;
    }
}

static void pollEventServer() {
    for (int i = 0; i < 3; i++) {
        WiFiClient c = eventServer.accept();
        if (!c) break;
        handleEventClient(c);
    }
}

// ---------------- Wi-Fi: credentials + captive-portal setup ----------------
static Preferences wifiPrefs;

static String portalPage(const std::vector<String> &ssids) {
    String h;
    h.reserve(2000 + ssids.size() * 60);
    h += F("<!doctype html><html lang=da><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Sonos-Remote</title><style>body{font-family:-apple-system,system-ui,sans-serif;background:#101820;color:#f2f4f7;"
           "margin:0;padding:24px}h1{font-size:22px}label{display:block;margin:16px 0 6px;color:#8a94a6}select,input,button{width:100%;"
           "box-sizing:border-box;font-size:17px;padding:12px;border-radius:10px;border:1px solid #2a3646;background:#1b2634;color:#f2f4f7}"
           "button{background:#3b82f6;border:0;margin-top:22px;font-weight:600}</style></head><body><h1>Sonos-Remote Wi-Fi</h1>"
           "<p>Vælg det Wi-Fi netværk (2,4 GHz) som fjernbetjeningen skal bruge.</p><form method=post action=/save>"
           "<label>Netværk</label><select name=ssid id=sel onchange=\"document.getElementById('man').value=this.value\">");
    for (const String &n : ssids) { h += "<option>"; h += n; h += "</option>"; }
    h += F("</select><label>Eller skriv navnet</label><input id=man name=ssid_manual autocomplete=off>"
           "<label>Kodeord</label><input name=pass type=password autocomplete=off>"
           "<button type=submit>Gem og genstart</button></form></body></html>");
    return h;
}

static void runWifiPortal() {
    WiFi.mode(WIFI_AP_STA);
    std::vector<String> ssids;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && ssids.size() < 15; i++) {
        String name = WiFi.SSID(i);
        if (name.isEmpty()) continue;
        bool dup = false;
        for (const String &e : ssids) if (e == name) dup = true;
        if (!dup) ssids.push_back(name);
    }
    WiFi.softAP("Sonos-Remote");
    IPAddress apIp = WiFi.softAPIP();
    DNSServer dns;
    dns.start(53, "*", apIp);
    WebServer web(80);
    volatile bool saved = false;
    web.on("/", HTTP_GET, [&]() { web.send(200, "text/html; charset=utf-8", portalPage(ssids)); });
    web.on("/save", HTTP_POST, [&]() {
        String ssid = web.arg("ssid_manual");
        if (ssid.isEmpty()) ssid = web.arg("ssid");
        ssid.trim();
        if (ssid.isEmpty()) { web.send(400, "text/plain; charset=utf-8", "Netværksnavn mangler"); return; }
        wifiPrefs.putString("ssid", ssid);
        wifiPrefs.putString("pass", web.arg("pass"));
        web.send(200, "text/html; charset=utf-8",
                 "<!doctype html><meta charset=utf-8><body style='font-family:sans-serif;background:#101820;color:#f2f4f7;padding:24px'>"
                 "<h1>Gemt</h1><p>Fjernbetjeningen genstarter og forbinder til <b>" + ssid + "</b>.</p></body>");
        saved = true;
    });
    web.onNotFound([&]() {                       // captive-portal detection from phones lands here
        web.sendHeader("Location", "http://" + apIp.toString() + "/", true);
        web.send(302, "text/plain", "");
    });
    web.begin();
    setStatus("Wi-Fi-opsætning\n\nForbind telefonen til Wi-Fi\n\"Sonos-Remote\"\n\nog åbn\nhttp://" + apIp.toString());
    Serial.printf("[wifi] portal at http://%s\n", apIp.toString().c_str());
    uint32_t t0 = millis();
    while (!saved && millis() - t0 < 15 * 60 * 1000) {
        dns.processNextRequest();
        web.handleClient();
        esp_task_wdt_reset();
        delay(5);
    }
    delay(1500);
    ESP.restart();
}

static bool connectWifi(const String &ssid, const String &pass) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname(OTA_HOSTNAME);
    WiFi.begin(ssid.c_str(), pass.c_str());
    setStatus("Forbinder til " + ssid + "...");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(200); esp_task_wdt_reset(); }
    bool ok = WiFi.status() == WL_CONNECTED;
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.wifiOk = ok; xSemaphoreGive(g_mutex);
    if (ok) Serial.printf("Wi-Fi ok, IP %s, free heap %lu (largest block %lu)\n", WiFi.localIP().toString().c_str(),
                          (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    return ok;
}

// Credentials: NVS (set via the portal) first, then the compile-time secrets.h fallback.
static void startWifiOrPortal() {
    wifiPrefs.begin("wifi", false);
    if (wifiPrefs.isKey("force") && wifiPrefs.getBool("force", false)) {
        wifiPrefs.remove("force");
        runWifiPortal();                                     // never returns
    }
    String ssid = wifiPrefs.isKey("ssid") ? wifiPrefs.getString("ssid", "") : String();
    String pass = wifiPrefs.isKey("pass") ? wifiPrefs.getString("pass", "") : String();
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    if (ssid.isEmpty() && strcmp(WIFI_SSID, "your-2.4GHz-network") != 0) { ssid = WIFI_SSID; pass = WIFI_PASSWORD; }
#endif
    if (ssid.isEmpty()) runWifiPortal();
    // With credentials known, keep trying forever (a wall-mounted board must never park itself in setup mode
    // because the link was weak for a minute). Setup mode is reachable from the web page instead.
    for (int attempt = 1;; attempt++) {
        if (connectWifi(ssid, pass)) return;
        int waitS = attempt < 5 ? 5 : attempt < 20 ? 20 : 60;
        setStatus("Wi-Fi fejlede (" + String(attempt) + "), prøver igen om " + String(waitS) + " s...");
        WiFi.disconnect(true);
        for (int i = 0; i < waitS * 4; i++) { delay(250); esp_task_wdt_reset(); }
        if (attempt % 30 == 0) { log_e("Wi-Fi unreachable for a long time, restarting"); delay(200); ESP.restart(); }
    }
}

// ---------------- Sonos task ----------------
static bool findSonos(const String &room) {
    setStatus("Leder efter " + room + "...");
    if (sonos.discover(room)) {
        prefs.putString("ip", sonos.coordinator().toString());
        return true;
    }
    String cached = prefs.isKey("ip") ? prefs.getString("ip", "") : String();
    IPAddress ip;
    if (cached.length() && ip.fromString(cached)) {
        sonos.setCoordinator(ip);
        SonosState s;
        if (sonos.refresh(s)) { log_i("using cached coordinator %s", cached.c_str()); return true; }
    }
    return false;
}

static void publishGroups() {
    std::vector<ZoneGroupInfo> groups;
    if (sonos.getGroups(groups)) {
        xSemaphoreTake(g_mutex, portMAX_DELAY); g.groups = groups; g.groupsDirty = true; xSemaphoreGive(g_mutex);
    }
}

static void sonosTask(void *) {
    log_i("task watchdog: sonos task %s", esp_err_to_name(esp_task_wdt_add(nullptr)));
    prefs.begin("sonos", false);
    String room = prefs.isKey("room") ? prefs.getString("room", SONOS_ROOM) : String(SONOS_ROOM);
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.room = room; g.roomDirty = true; xSemaphoreGive(g_mutex);

    startWifiOrPortal();
    setupOta();
    setupConfigWeb();
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
    while (!findSonos(room)) { setStatus(room + " ikke fundet, prøver igen..."); esp_task_wdt_reset(); delay(5000); esp_task_wdt_reset(); }

    std::vector<SonosFavorite> favs;
    if (sonos.browseFavorites(favs)) {
        xSemaphoreTake(g_mutex, portMAX_DELAY); g.favs = favs; g.favsDirty = true; xSemaphoreGive(g_mutex);
    }
    eventServer.begin();
    eventServer.setNoDelay(true);
    publishGroups();                                  // page dots + web room list from the start
    ensureSubscriptions();
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = true; xSemaphoreGive(g_mutex);

    uint32_t lastPoll = 0, lastTopology = millis(), lastSubCheck = 0, eventDebounce = 0, lastSuccess = millis(), lastReconnect = 0;
    int failures = 0;
    bool forcePoll = true;
    String lastArtUri, pendingArtUrl;
    uint32_t artRetryAt = 0; int artAttempts = 0;
    SonosState last;

    for (;;) {
        esp_task_wdt_reset();
        ArduinoOTA.handle();
        if (g_ota) { delay(10); continue; }
        rlog_poll();
        cfgWeb.handleClient();
        pollEventServer();

        Cmd c;
        while (xQueueReceive(g_cmdQueue, &c, 0) == pdTRUE) {
            bool ok = true;
            switch (c.type) {
                case AppCmd::Play:        ok = sonos.play(); break;
                case AppCmd::Pause:       ok = sonos.pause(); break;
                case AppCmd::Next:        ok = sonos.next(); break;
                case AppCmd::Prev:        ok = sonos.previous(); break;
                case AppCmd::SetVolume: {
                    Cmd more;                                   // coalesce rapid +/- taps into one call
                    while (xQueuePeek(g_cmdQueue, &more, 0) == pdTRUE && more.type == AppCmd::SetVolume) { xQueueReceive(g_cmdQueue, &more, 0); c = more; }
                    ok = sonos.setVolume(c.value);
                    break;
                }
                case AppCmd::ToggleMute:  ok = sonos.setMute(!last.muted); break;
                case AppCmd::PlayFavorite: ok = sonos.playFavoriteAt(c.value); break;
                case AppCmd::RefreshFavorites:
                    if (sonos.browseFavorites(favs)) {
                        xSemaphoreTake(g_mutex, portMAX_DELAY); g.favs = favs; g.favsDirty = true; xSemaphoreGive(g_mutex);
                    }
                    break;
                case AppCmd::RefreshGroups: publishGroups(); break;
                case AppCmd::RefreshQueue: {
                    std::vector<QueueItem> items; int total = 0;
                    int cur = last.track.trackNo;
                    int start = cur > 4 ? cur - 4 : 0;          // window around the current track
                    bool got = sonos.browseQueue(start, 30, items, total);
                    xSemaphoreTake(g_mutex, portMAX_DELAY);
                    g.queue = items; g.queueTotal = got ? total : -1; g.queueCurrent = cur; g.queueDirty = true;
                    xSemaphoreGive(g_mutex);
                    ok = got;
                    break;
                }
                case AppCmd::PlayQueueTrack: ok = sonos.playQueueTrack(c.value); break;
                case AppCmd::WifiSetup:
                    dropSubscriptions();
                    wifiPrefs.putBool("force", true);
                    setStatus("Genstarter til Wi-Fi-opsætning...");
                    delay(800);
                    ESP.restart();
                    break;
                case AppCmd::JoinGroup:
                case AppCmd::LeaveGroup:
                case AppCmd::SwipeRoom:
                case AppCmd::ControlRoom: {
                    xSemaphoreTake(g_mutex, portMAX_DELAY);
                    ZoneGroupInfo target = (c.value >= 0 && c.value < (int)g.groups.size()) ? g.groups[c.value] : ZoneGroupInfo{};
                    xSemaphoreGive(g_mutex);
                    if (target.zoneNames.empty()) { ok = false; break; }
                    if (c.type == AppCmd::JoinGroup)       ok = sonos.joinGroup(target.coordinatorUuid);
                    else if (c.type == AppCmd::LeaveGroup) ok = sonos.leaveGroup();
                    else {
                        String wanted = target.zoneNames[0];
                        ok = sonos.setRoom(wanted);
                        if (!ok) { delay(1000); ok = sonos.setRoom(wanted); }   // topology can lag a regroup
                        if (ok) { room = wanted; if (c.type == AppCmd::ControlRoom) prefs.putString("room", room); }
                        else log_w("could not switch to room '%s'", wanted.c_str());
                        if (!ok) { xSemaphoreTake(g_mutex, portMAX_DELAY); g.roomDirty = true; g.groupsDirty = true; xSemaphoreGive(g_mutex); }
                    }
                    if (ok) { delay(800); topologyDue = true; }
                    break;
                }
                case AppCmd::SetHomeRoom: {
                    xSemaphoreTake(g_mutex, portMAX_DELAY); String wanted = g.pendingRoom; xSemaphoreGive(g_mutex);
                    ok = wanted.length() && sonos.setRoom(wanted);
                    if (ok) { room = wanted; prefs.putString("room", room); topologyDue = true; }
                    break;
                }
                case AppCmd::Rediscover: findSonos(room); break;
            }
            if (!ok) log_w("command %d failed", (int)c.type);
            forcePoll = true;
            delay(150);
        }

        if (topologyDue || millis() - lastTopology > TOPOLOGY_MS) {
            topologyDue = false; lastTopology = millis();
            IPAddress before = sonos.coordinator();
            if (sonos.refreshTopology()) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (g.room != sonos.room()) { g.room = sonos.room(); g.roomDirty = true; }   // UI always mirrors the client
                xSemaphoreGive(g_mutex);
                if (sonos.coordinator() != before) {
                    log_i("coordinator changed to %s", sonos.coordinator().toString().c_str());
                    prefs.putString("ip", sonos.coordinator().toString());
                    lastArtUri = "";
                }
                ensureSubscriptions();
                forcePoll = true;
                publishGroups();
            }
        }

        if (eventPollDue) {                         // debounce bursts of NOTIFYs
            if (!eventDebounce) eventDebounce = millis();
            if (millis() - eventDebounce > 120) { eventPollDue = false; eventDebounce = 0; forcePoll = true; }
        }

        if (forcePoll || millis() - lastPoll > POLL_MS) {
            forcePoll = false; lastPoll = millis();
            SonosState s;
            if (sonos.refresh(s)) {
                failures = 0;
                lastSuccess = millis();
                last = s;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g.state = s; g.stateDirty = true;
                g.wifiOk = WiFi.status() == WL_CONNECTED;
                xSemaphoreGive(g_mutex);
                String uri = s.track.artUri;
                if (uri != lastArtUri) {
                    lastArtUri = uri;
                    pendingArtUrl = sonos.artUrl(s.track);
                    artAttempts = 0; artRetryAt = millis();
                    if (pendingArtUrl.isEmpty()) {
                        xSemaphoreTake(g_mutex, portMAX_DELAY); g.artValid = false; g.artDirty = true; xSemaphoreGive(g_mutex);
                    }
                }
            } else if (++failures >= 3) {
                failures = 0;
                xSemaphoreTake(g_mutex, portMAX_DELAY); g.state.online = false; g.stateDirty = true; xSemaphoreGive(g_mutex);
                if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(3000); }
                else if (sonos.discover(room)) ensureSubscriptions(true);
            }
        }

        if (pendingArtUrl.length() && (int32_t)(millis() - artRetryAt) >= 0) {
            pollEventServer();                          // drain queued NOTIFYs first: frees their buffers
            if (fetchArt(pendingArtUrl) || ++artAttempts >= 4) {
                if (artAttempts >= 4) { xSemaphoreTake(g_mutex, portMAX_DELAY); g.artValid = false; g.artDirty = true; xSemaphoreGive(g_mutex); }
                pendingArtUrl = "";
            } else {
                artRetryAt = millis() + 1500 * artAttempts;   // back off 1.5 s, 3 s, 4.5 s
            }
        }

        if (millis() - lastSubCheck > 5000) {
            lastSubCheck = millis();
            ensureSubscriptions();
            // heap watchdog: if memory stays critically low the network stack starves and the board goes deaf;
            // a clean reboot is better than a zombie remote
            static uint32_t lowSince = 0;
            uint32_t freeHeap = ESP.getFreeHeap();
            if (freeHeap < 18000) { if (!lowSince) lowSince = millis(); }
            else lowSince = 0;
            if (lowSince && millis() - lowSince > 30000) { log_e("heap critically low (%lu) for 30 s, restarting", (unsigned long)freeHeap); delay(100); ESP.restart(); }
            static uint32_t lastReport = 0;
            if (millis() - lastReport > 60000) {
                lastReport = millis();
                log_i("heap %lu free, largest %lu, stack unused %u, wifi rssi %d dBm", (unsigned long)freeHeap,
                      (unsigned long)ESP.getMaxAllocHeap(), (unsigned)uxTaskGetStackHighWaterMark(nullptr), WiFi.RSSI());
            }
            // stuck on a distant mesh node? ESP32 never roams by itself: force a fresh association
            static uint32_t weakSince = 0;
            int rssi = WiFi.RSSI();
            if (WiFi.status() == WL_CONNECTED && rssi < -80) { if (!weakSince) weakSince = millis(); } else weakSince = 0;
            if (weakSince && millis() - weakSince > 2 * 60000u && millis() - lastReconnect > 5 * 60000u) {
                lastReconnect = millis(); weakSince = 0;
                log_w("weak signal (%d dBm) for 2 min, reconnecting to find a better access point", rssi);
                WiFi.disconnect(); delay(500); WiFi.reconnect();
            }
            // network watchdog, based on the time since the last successful Sonos call
            uint32_t silent = millis() - lastSuccess;
            if (silent > 5 * 60000u) { log_e("no successful Sonos call for 5 min, restarting"); delay(200); ESP.restart(); }
            else if (silent > 2 * 60000u && millis() - lastReconnect > 60000u) {
                lastReconnect = millis();
                log_w("no successful Sonos call for 2 min (rssi %d), reconnecting Wi-Fi", WiFi.RSSI());
                WiFi.disconnect(); delay(500); WiFi.reconnect();
            }
        }
        delay(20);
    }
}

// ---------------- UI side ----------------
static void ui_sync_timer(lv_timer_t *) {
    if (xSemaphoreTake(g_mutex, 0) != pdTRUE) return;
    if (g.roomDirty)   { g.roomDirty = false; ui_set_room(g.room.c_str()); }
    if (g.statusDirty) { g.statusDirty = false; if (!g.ready) ui_set_status(g.status.c_str()); }
    if (g.ready) ui_show_now_playing();
    if (g.stateDirty)  { g.stateDirty = false; uiTransport = g.state.transport; ui_update_state(g.state); ui_set_wifi_ok(g.wifiOk); }
    if (g.favsDirty)   { g.favsDirty = false; ui_update_favorites(g.favs); }
    if (g.groupsDirty) { g.groupsDirty = false; ui_update_groups(g.groups); }
    if (g.queueDirty)  { g.queueDirty = false; ui_update_queue(g.queue, g.queueTotal, g.queueCurrent); }
    if (g.artDirty)    { g.artDirty = false; ui_set_art(g.artValid); }
    xSemaphoreGive(g_mutex);
    if (cfgRotationDirty) { cfgRotationDirty = false; lcd.setRotation(cfg.rotation); ui_apply_rotation(); }
}

static void backlightTask() {
    static uint32_t last = 0; static int level = 200;
    if (millis() - last < 250) return;
    last = millis();
    uint32_t inactive = lv_display_get_inactive_time(nullptr);
    bool playing = uiTransport == SonosTransport::Playing || uiTransport == SonosTransport::Transitioning;
    uint32_t sleepMs = (uint32_t)cfg.sleepMin * 60000u, idleMs = (uint32_t)cfg.idleMin * 60000u;
    if (!screenAsleep && !playing && sleepMs && inactive > sleepMs) { screenAsleep = true; level = 0; lcd.setBrightness(0); }
    if (screenAsleep) return;
    int ldr = analogRead(PIN_LDR);
    int target = ldr < 150 ? cfg.blBright : ldr < 800 ? cfg.blNormal : cfg.blDark;   // LDR divider: 0 daylight .. ~1700 dark
    if (idleMs && inactive > idleMs) target = min(target, cfg.blIdle);
    level += (target - level) / 3;
    if (level < 3) level = 3;
    lcd.setBrightness(level);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Sonos kitchen controller ===");
    Serial.printf("build %s %s\n", __DATE__, __TIME__);
    rlog_begin(OTA_HOSTNAME);

    // Why did we (re)start? Sent to the log server as soon as the network is up.
    static const char *reasons[] = {"unknown", "power-on", "external", "software", "panic", "int-wdt", "task-wdt", "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr-glitch", "cpu-lockup"};
    int rr = (int)esp_reset_reason();
    const esp_app_desc_t *app = esp_app_get_description();
    char runSha[9] = {0};
    for (int i = 0; i < 4; i++) sprintf(runSha + 2 * i, "%02x", app->app_elf_sha256[i]);
    String boot = String("BOOT build ") + __DATE__ + " " + __TIME__ + " elf=" + runSha + ", reset: " + reasons[(rr >= 0 && rr < 16) ? rr : 0];
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t sum;
        if (esp_core_dump_get_summary(&sum) == ESP_OK) {
            char sha[9] = {0};
            memcpy(sha, sum.app_elf_sha256, 8);                       // stored as a hex string
            boot += String(" | CRASH task=") + sum.exc_task + " cause=0x" + String(sum.ex_info.exc_cause, HEX)
                  + " vaddr=0x" + String(sum.ex_info.exc_vaddr, HEX) + " pc=0x" + String(sum.exc_pc, HEX) + " elf=" + sha + " bt=";
            for (uint32_t i = 0; i < sum.exc_bt_info.depth && i < 12; i++) { boot += "0x"; boot += String(sum.exc_bt_info.bt[i], HEX); boot += " "; }
            if (sum.exc_bt_info.corrupted) boot += "(corrupted)";
        } else boot += " | CRASH dump present";
        esp_core_dump_image_erase();
    }
    Serial.println(boot);
    rlog_set_boot_report(boot);

    // Hardware task watchdog: UI loop and Sonos task must both check in every 60 s or the chip reboots.
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 60000; wdt.idle_core_mask = 0; wdt.trigger_panic = true;
    esp_err_t wr = esp_task_wdt_reconfigure(&wdt);
    if (wr != ESP_OK) wr = esp_task_wdt_init(&wdt);
    esp_err_t wa = esp_task_wdt_add(nullptr);
    Serial.printf("task watchdog: config %s, loop task %s\n", esp_err_to_name(wr), esp_err_to_name(wa));

    pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
    digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
    analogReadResolution(12);

    config_load();
    rlog_set_target(cfg.logHost, cfg.logPort);
    {   // which touch variant is this? (CST820 capacitive answers at 0x15; the resistive XPT2046 is on SPI)
        Wire.begin(33, 32, 100000);
        Wire.beginTransmission(0x15);
        bool cap = Wire.endTransmission() == 0;
        Wire.end();
        Serial.printf("touch probe: %s\n", cap ? "CST8xx capacitive controller found at 0x15 (C variant)" : "no I2C touch at 0x15 (resistive R variant?)");
    }
    lcd.init();
    lcd.setRotation(cfg.rotation);
    lcd.setBrightness(200);
    lcd.fillScreen(TFT_BLACK);

    draw_buf1 = static_cast<lv_color16_t *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    draw_buf2 = nullptr;                    // single buffer: saves 11.5 KB of heap for the network stack
    artBuf = static_cast<uint16_t *>(heap_caps_malloc(ART_SIZE * ART_SIZE * 2, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    if (artBuf) artSprite.setBuffer(artBuf, ART_SIZE, ART_SIZE, 16);
    if (!draw_buf1 || !artBuf) {
        Serial.println("FATAL: out of memory for buffers");
        while (true) delay(1000);
    }

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(disp, lv_flush_cb);
    lv_display_set_buffers(disp, draw_buf1, nullptr, BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lv_touch_read_cb);

    g_mutex = xSemaphoreCreateMutex();
    g_cmdQueue = xQueueCreate(8, sizeof(Cmd));

    ui_init(SONOS_ROOM, artBuf);
    ui_set_status("Starter...");
    lv_timer_create(ui_sync_timer, 100, nullptr);

    xTaskCreatePinnedToCore(sonosTask, "sonos", 16384, nullptr, 1, nullptr, 0);
    Serial.printf("free heap after init: %lu\n", (unsigned long)ESP.getFreeHeap());
}

// ---------------- touch calibration (started from the web page) ----------------
// Draws five crosshairs straight to the panel (LVGL is not ticked meanwhile), averages the uncalibrated touch
// position of each tap and fits screen = a * raw + b per axis by least squares. 20 s without a tap aborts.
volatile bool touchCalRequest = false;

static void calDrawTarget(int x, int y, int n, int total) {
    lcd.fillScreen(lcd.color565(0x10, 0x18, 0x20));
    uint16_t c = lcd.color565(0x3b, 0x82, 0xf6);
    lcd.drawFastHLine(x - 14, y, 29, c); lcd.drawFastVLine(x, y - 14, 29, c);
    lcd.drawCircle(x, y, 7, c);
    lcd.setTextColor(lcd.color565(0xf2, 0xf4, 0xf7));
    lcd.setTextDatum(lgfx::middle_center);
    lcd.drawString("Tryk midt i krydset", SCREEN_W / 2, SCREEN_H / 2 - 24);
    lcd.drawString(String(n) + " / " + String(total), SCREEN_W / 2, SCREEN_H / 2 - 8);
}

static bool calFit(const int *raw, const int *target, int n, int *a1000, int *b) {
    double sr = 0, st = 0, srr = 0, srt = 0;
    for (int i = 0; i < n; i++) { sr += raw[i]; st += target[i]; srr += (double)raw[i] * raw[i]; srt += (double)raw[i] * target[i]; }
    double den = n * srr - sr * sr;
    if (den == 0) return false;
    double a = (n * srt - sr * st) / den, bb = (st - a * sr) / n;
    if (a < 0.7 || a > 1.4) return false;                  // nonsense taps: keep the old calibration
    *a1000 = (int)lround(a * 1000); *b = (int)lround(bb);
    return true;
}

static void runTouchCalibration() {
    static const int TX[] = {30, 210, 120, 30, 210}, TY[] = {24, 24, 160, 296, 296};
    const int N = 5;
    int rx[N], ry[N];
    log_i("touch calibration started (rotation %d)", cfg.rotation);
    bool ok = true;
    for (int i = 0; i < N && ok; i++) {
        calDrawTarget(TX[i], TY[i], i + 1, N);
        int x, y;
        uint32_t t0 = millis();
        while (readTouch(&x, &y)) { delay(10); esp_task_wdt_reset(); }         // wait for release
        while (!readTouch(&x, &y)) {                                           // wait for the tap
            delay(10); esp_task_wdt_reset();
            if (millis() - t0 > 20000) { ok = false; break; }
        }
        if (!ok) break;
        long sx = 0, sy = 0; int cnt = 0;
        while (readTouch(&x, &y)) { sx += x; sy += y; cnt++; delay(10); esp_task_wdt_reset(); }
        if (cnt == 0) { i--; continue; }
        rx[i] = sx / cnt; ry[i] = sy / cnt;
        log_i("touch calibration target %d,%d -> raw %d,%d (%d samples)", TX[i], TY[i], rx[i], ry[i], cnt);
    }
    int ax, bx, ay, by;
    const char *msg;
    if (!ok) msg = "Afbrudt";
    else if (calFit(rx, TX, N, &ax, &bx) && calFit(ry, TY, N, &ay, &by)) {
        cfg.tcAx = ax; cfg.tcBx = bx; cfg.tcAy = ay; cfg.tcBy = by; cfg.tcRot = cfg.rotation;
        config_save();
        log_i("touch calibration saved: x = raw*%d/1000%+d, y = raw*%d/1000%+d", ax, bx, ay, by);
        msg = "Kalibrering gemt";
    } else { log_w("touch calibration rejected (taps too far off)"); msg = "Mislykkedes - proev igen"; }
    lcd.fillScreen(lcd.color565(0x10, 0x18, 0x20));
    lcd.drawString(msg, SCREEN_W / 2, SCREEN_H / 2);
    delay(1200);
    swallowTouch = true;                                   // the last tap must not reach LVGL
    lv_obj_invalidate(lv_screen_active());
    lv_display_trigger_activity(nullptr);
}

void loop() {
    esp_task_wdt_reset();
    if (touchCalRequest) { touchCalRequest = false; runTouchCalibration(); }
    lv_timer_handler();
    backlightTask();
    delay(5);
}

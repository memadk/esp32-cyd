// Info board on a Sunton ESP32-2432S024C: electricity prices (DK1), weather, bus departures, Home Assistant, Meater probe.
//   core 1 (Arduino loop): LVGL UI, display, touch, backlight
//   core 0 (netTask):      Wi-Fi, OTA, web config, remote log, data fetching over HTTPS
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "lgfx_cyd_2432s024c.hpp"
#include "rlog.h"
#include "info_config.h"
#include "data.h"
#include "info_ui.h"

#if __has_include("../../include/secrets.h")
#include "../../include/secrets.h"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef TOUCH_FLIP_X
#define TOUCH_FLIP_X 0
#endif
#ifndef TOUCH_FLIP_Y
#define TOUCH_FLIP_Y 0
#endif

static LGFX_CYD_2432S024C lcd;
InfoConfig cfg;
volatile bool cfgRotationDirty = false;
static Preferences cfgPrefs, wifiPrefs;
static WebServer cfgWeb(80);
static SemaphoreHandle_t g_mutex;
static volatile bool g_ota = false;

// ---------------- shared state (guarded by g_mutex) ----------------
struct Shared {
    String status; bool statusDirty = false;
    PriceData prices; bool pricesDirty = false;
    WeatherData weather; bool weatherDirty = false;
    DepartureData deps; bool depsDirty = false;
    HaData ha; bool haDirty = false;
    MeaterData meater; bool meaterDirty = false;
    bool ready = false, wifiOk = false;
    bool refetchDeps = false;      // config changed
} g;

static void setStatus(const String &s) {
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.status = s; g.statusDirty = true; xSemaphoreGive(g_mutex);
    log_i("[status] %s", s.c_str());
}

// ---------------- config ----------------
void config_load() {
    cfgPrefs.begin("cfg", false);
    auto geti = [](const char *k, int d) { return cfgPrefs.isKey(k) ? cfgPrefs.getInt(k, d) : d; };
    auto gets = [](const char *k, const String &d) { return cfgPrefs.isKey(k) ? cfgPrefs.getString(k, d) : d; };
    auto getf = [](const char *k, float d) { return cfgPrefs.isKey(k) ? cfgPrefs.getFloat(k, d) : d; };
    cfg.blBright = geti("blBright", cfg.blBright); cfg.blNormal = geti("blNormal", cfg.blNormal);
    cfg.blDark = geti("blDark", cfg.blDark); cfg.blIdle = geti("blIdle", cfg.blIdle);
    cfg.idleMin = geti("idleMin", cfg.idleMin); cfg.rotation = geti("rotation", cfg.rotation);
    cfg.nightStart = geti("nightStart", cfg.nightStart); cfg.nightEnd = geti("nightEnd", cfg.nightEnd);
    cfg.darkOff = geti("darkOff", cfg.darkOff ? 1 : 0) != 0; cfg.darkMin = geti("darkMin", cfg.darkMin); cfg.wakeSec = geti("wakeSec", cfg.wakeSec);
    cfg.priceArea = gets("priceArea", cfg.priceArea); cfg.surchargeOere = geti("surcharge", cfg.surchargeOere);
    cfg.vat = geti("vat", cfg.vat ? 1 : 0) != 0;
    cfg.lat = getf("lat", cfg.lat); cfg.lon = getf("lon", cfg.lon); cfg.place = gets("place", cfg.place);
    cfg.rpKey = gets("rpKey", cfg.rpKey); cfg.stopQuery = gets("stop", cfg.stopQuery);
    cfg.line = gets("line", cfg.line); cfg.direction = gets("direction", cfg.direction);
    cfg.otaPassword = gets("otaPass", cfg.otaPassword);
    cfg.logHost = gets("logHost", cfg.logHost); cfg.logPort = geti("logPort", cfg.logPort);
    cfg.haUrl = gets("haUrl", cfg.haUrl); cfg.haToken = gets("haToken", cfg.haToken); cfg.haEntities = gets("haEnt", cfg.haEntities);
    cfg.meater = gets("meater", cfg.meater); cfg.meaterAlert = geti("meaterAl", cfg.meaterAlert ? 1 : 0) != 0;
}
void config_save() {
    cfgPrefs.putInt("blBright", cfg.blBright); cfgPrefs.putInt("blNormal", cfg.blNormal);
    cfgPrefs.putInt("blDark", cfg.blDark); cfgPrefs.putInt("blIdle", cfg.blIdle);
    cfgPrefs.putInt("idleMin", cfg.idleMin); cfgPrefs.putInt("rotation", cfg.rotation);
    cfgPrefs.putInt("nightStart", cfg.nightStart); cfgPrefs.putInt("nightEnd", cfg.nightEnd);
    cfgPrefs.putInt("darkOff", cfg.darkOff ? 1 : 0); cfgPrefs.putInt("darkMin", cfg.darkMin); cfgPrefs.putInt("wakeSec", cfg.wakeSec);
    cfgPrefs.putString("priceArea", cfg.priceArea); cfgPrefs.putInt("surcharge", cfg.surchargeOere);
    cfgPrefs.putInt("vat", cfg.vat ? 1 : 0);
    cfgPrefs.putFloat("lat", cfg.lat); cfgPrefs.putFloat("lon", cfg.lon); cfgPrefs.putString("place", cfg.place);
    cfgPrefs.putString("rpKey", cfg.rpKey); cfgPrefs.putString("stop", cfg.stopQuery);
    cfgPrefs.putString("line", cfg.line); cfgPrefs.putString("direction", cfg.direction);
    cfgPrefs.putString("otaPass", cfg.otaPassword);
    cfgPrefs.putString("logHost", cfg.logHost); cfgPrefs.putInt("logPort", cfg.logPort);
    cfgPrefs.putString("haUrl", cfg.haUrl); cfgPrefs.putString("haToken", cfg.haToken); cfgPrefs.putString("haEnt", cfg.haEntities);
    cfgPrefs.putString("meater", cfg.meater); cfgPrefs.putInt("meaterAl", cfg.meaterAlert ? 1 : 0);
}

// ---------------- LVGL <-> LovyanGFX glue ----------------
static int SCREEN_W = 240, SCREEN_H = 320;                 // set from cfg.rotation at boot (1/3 = landscape 320x240)
static constexpr int BUF_LINES = 24;
static constexpr size_t BUF_BYTES = 320 * BUF_LINES * sizeof(lv_color16_t);
static lv_color16_t *draw_buf1;

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
    lcd.startWrite(); lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels(reinterpret_cast<lgfx::rgb565_t *>(px_map), w * h);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}
static volatile bool screenOff = false;
static int g_ldr = 0, g_level = 200;            // for the minute log line
static volatile uint32_t wakeUntil = 0;         // millis until which a touch keeps the screen on at night
static bool swallowTouch = false;
static uint32_t ledAlertSince = 0;              // Meater "take it out" alert: red LED blinking since

static void lv_touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    uint16_t x, y;
    bool pressed = lcd.getTouch(&x, &y);
    if (screenOff) {                              // first touch only wakes the screen
        if (pressed) { wakeUntil = millis() + (uint32_t)cfg.wakeSec * 1000u; swallowTouch = true; lv_display_trigger_activity(nullptr); }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (swallowTouch) { if (!pressed) swallowTouch = false; data->state = LV_INDEV_STATE_RELEASED; return; }
    if (pressed) {
        wakeUntil = millis() + (uint32_t)cfg.wakeSec * 1000u;
        if (TOUCH_FLIP_X) x = SCREEN_W - 1 - x;
        if (TOUCH_FLIP_Y) y = SCREEN_H - 1 - y;
        data->state = LV_INDEV_STATE_PRESSED; data->point.x = x; data->point.y = y;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

// ---------------- Wi-Fi: NVS credentials + captive portal ----------------
static String portalPage(const std::vector<String> &ssids) {
    String h;
    h.reserve(1800 + ssids.size() * 60);
    h += F("<!doctype html><html lang=da><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Infotavle</title><style>body{font-family:-apple-system,system-ui,sans-serif;background:#101820;color:#f2f4f7;margin:0;padding:24px}"
           "h1{font-size:22px}label{display:block;margin:16px 0 6px;color:#8a94a6}select,input,button{width:100%;box-sizing:border-box;font-size:17px;padding:12px;"
           "border-radius:10px;border:1px solid #2a3646;background:#1b2634;color:#f2f4f7}button{background:#3b82f6;border:0;margin-top:22px;font-weight:600}</style></head>"
           "<body><h1>Infotavle Wi-Fi</h1><form method=post action=/save><label>Netværk</label><select name=ssid>");
    for (const String &n : ssids) { h += "<option>"; h += n; h += "</option>"; }
    h += F("</select><label>Eller skriv navnet</label><input name=ssid_manual autocomplete=off><label>Kodeord</label><input name=pass type=password>"
           "<button type=submit>Gem og genstart</button></form></body></html>");
    return h;
}
static void runWifiPortal() {
    WiFi.mode(WIFI_AP_STA);
    std::vector<String> ssids;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && ssids.size() < 15; i++) { String nm = WiFi.SSID(i); bool dup = nm.isEmpty(); for (auto &e : ssids) if (e == nm) dup = true; if (!dup) ssids.push_back(nm); }
    WiFi.softAP("Infotavle");
    IPAddress apIp = WiFi.softAPIP();
    DNSServer dns; dns.start(53, "*", apIp);
    WebServer web(80);
    volatile bool saved = false;
    web.on("/", HTTP_GET, [&]() { web.send(200, "text/html; charset=utf-8", portalPage(ssids)); });
    web.on("/save", HTTP_POST, [&]() {
        String ssid = web.arg("ssid_manual"); if (ssid.isEmpty()) ssid = web.arg("ssid"); ssid.trim();
        if (ssid.isEmpty()) { web.send(400, "text/plain", "ssid"); return; }
        wifiPrefs.putString("ssid", ssid); wifiPrefs.putString("pass", web.arg("pass"));
        web.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='font-family:sans-serif;padding:24px'>Gemt, genstarter.</body>");
        saved = true;
    });
    web.onNotFound([&]() { web.sendHeader("Location", "http://" + apIp.toString() + "/", true); web.send(302, "text/plain", ""); });
    web.begin();
    setStatus("Wi-Fi-opsætning\n\nForbind telefonen til Wi-Fi\n\"Infotavle\"\n\nog åbn\nhttp://" + apIp.toString());
    uint32_t t0 = millis();
    while (!saved && millis() - t0 < 15 * 60 * 1000) { dns.processNextRequest(); web.handleClient(); esp_task_wdt_reset(); delay(5); }
    delay(1500); ESP.restart();
}
static bool connectWifi(const String &ssid, const String &pass) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setHostname(OTA_HOSTNAME);
    WiFi.begin(ssid.c_str(), pass.c_str());
    setStatus("Forbinder til " + ssid + "...");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(200); esp_task_wdt_reset(); }
    bool ok = WiFi.status() == WL_CONNECTED;
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.wifiOk = ok; xSemaphoreGive(g_mutex);
    if (ok) log_i("Wi-Fi ok, IP %s, free heap %lu", WiFi.localIP().toString().c_str(), (unsigned long)ESP.getFreeHeap());
    return ok;
}
static void startWifiOrPortal() {
    wifiPrefs.begin("wifi", false);
    if (wifiPrefs.isKey("force") && wifiPrefs.getBool("force", false)) { wifiPrefs.remove("force"); runWifiPortal(); }
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

// ---------------- OTA ----------------
static void setupOta() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setTimeout(10000);   // default 1 s of silence = "OTA fejl 3"; the kitchen link has multi-second stalls
    if (cfg.otaPassword.length()) ArduinoOTA.setPassword(cfg.otaPassword.c_str());
    else if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { g_ota = true; cfgWeb.stop(); xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = false; g.status = "Opdaterer firmware..."; g.statusDirty = true; xSemaphoreGive(g_mutex); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { esp_task_wdt_reset(); static uint32_t last = 0; if (millis() - last > 400) { last = millis(); setStatus("Opdaterer firmware... " + String(done * 100 / total) + " %"); } });
    ArduinoOTA.onEnd([]() { setStatus("Genstarter..."); });
    ArduinoOTA.onError([](ota_error_t err) { g_ota = false; setStatus("OTA fejl " + String((int)err)); xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = true; xSemaphoreGive(g_mutex); });
    ArduinoOTA.begin();
    log_i("[ota] ready: %s.local", OTA_HOSTNAME);
}

// ---------------- web configuration ----------------
static String esc(const String &s) { String o; for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '"') o += "&quot;"; else o += c; } return o; }
static void flush(String &h, bool fin = false) {
    size_t pos = 0;
    while (pos < h.length()) { size_t n = std::min((size_t)600, h.length() - pos); cfgWeb.sendContent(h.substring(pos, pos + n)); pos += n; delay(1); }
    h = ""; if (fin) cfgWeb.sendContent("");
}
static void field(String &h, const char *label, const char *name, const String &value, const char *type = "text", const char *hint = "") {
    h += "<label>"; h += label; h += "</label><input name="; h += name; h += " type="; h += type; h += " value='"; h += esc(value); h += "'>";
    if (*hint) { h += "<small>"; h += hint; h += "</small>"; }
}
static void sendPage() {
    cfgWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
    cfgWeb.send(200, "text/html; charset=utf-8", "");
    String h; h.reserve(900);
    h += F("<!doctype html><html lang=da><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Infotavle</title>"
           "<style>body{font-family:-apple-system,system-ui,sans-serif;background:#101820;color:#f2f4f7;margin:0;padding:22px;max-width:560px}h1{font-size:22px;margin:0 0 4px}"
           "h2{font-size:16px;color:#8a94a6;margin:26px 0 8px;border-bottom:1px solid #2a3646;padding-bottom:6px}label{display:block;margin:12px 0 4px;color:#8a94a6}"
           "small{color:#5d6a7d;display:block;margin-top:3px}select,input,textarea{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border-radius:10px;border:1px solid #2a3646;background:#1b2634;color:#f2f4f7}");
    flush(h);
    h += F("button{width:100%;font-size:16px;padding:12px;border-radius:10px;border:0;background:#3b82f6;color:#fff;margin-top:18px;font-weight:600}.warn{background:#3a2a2a}"
           ".grid{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}.stat{color:#8a94a6;font-size:14px;line-height:1.6}</style></head><body><h1>Infotavle</h1><div class=stat>");
    h += "IP " + WiFi.localIP().toString() + " &middot; Fri hukommelse " + String(ESP.getFreeHeap() / 1024) + " KB &middot; Oppetid " + String(millis() / 60000) + " min<br>Build ";
    h += __DATE__; h += " "; h += __TIME__;
    h += F("</div><form method=post action=/save><h2>Elpris</h2><div class=grid>");
    field(h, "Prisområde", "priceArea", cfg.priceArea, "text", "DK1 = vest for Storebælt, DK2 = øst");
    field(h, "Tillæg (øre/kWh, ekskl. moms)", "surcharge", String(cfg.surchargeOere), "number", "nettarif + elafgift, lægges til spotprisen");
    h += F("</div><label>Vis priser</label><select name=vat>");
    h += String("<option value=1") + (cfg.vat ? " selected" : "") + ">inkl. moms</option><option value=0" + (!cfg.vat ? " selected" : "") + ">ekskl. moms</option></select>";
    flush(h);
    h += F("<h2>Vejr</h2><div class=grid>");
    field(h, "Breddegrad", "lat", String(cfg.lat, 4), "text"); field(h, "Længdegrad", "lon", String(cfg.lon, 4), "text");
    h += F("</div>"); field(h, "Stednavn", "place", cfg.place);
    flush(h);
    h += F("<h2>Bus</h2>");
    field(h, "Rejseplanen API-nøgle", "rpKey", "", "password", cfg.rpKey.length() ? "En nøgle er gemt, den officielle API bruges. Tom = uændret, minus (-) = slet." : "Valgfri. Uden nøgle bruges Rejseplanens ældre afgangstavle, som ikke kræver nøgle.");
    field(h, "Stoppested", "stop", cfg.stopQuery); 
    h += F("<div class=grid>"); field(h, "Linje", "line", cfg.line); field(h, "Retning (indeholder)", "direction", cfg.direction); h += F("</div>");
    flush(h);
    h += F("<h2>Hjem (Home Assistant)</h2>");
    field(h, "Adresse", "haUrl", cfg.haUrl, "text", "fx http://192.168.68.96:8123");
    field(h, "Adgangstoken", "haToken", "", "password", cfg.haToken.length() ? "Et token er gemt. Tom = uændret, minus (-) = slet." : "Lav et langlivet token i Home Assistant: profil, Sikkerhed, Langlivede adgangstokens.");
    h += F("<label>Enheder (én per linje: entity_id|Navn)</label><textarea name=haEnt rows=6 placeholder='sensor.stue_temperatur|Stue&#10;binary_sensor.hoveddoer|Hoveddør'>");
    h += esc(cfg.haEntities); h += F("</textarea><small>Op til 8 linjer. Navn er valgfrit, ellers bruges enhedens navn fra Home Assistant. Flere enheder med + imellem tæller hvor mange der er åbne/tændte, fx binary_sensor.a+binary_sensor.b|Åbne vinduer.</small>");
    flush(h);
    h += F("<h2>Stegning (Meater via Home Assistant)</h2>");
    field(h, "Meater-enhed", "meater", cfg.meater, "text", "Præfiks eller et af probens entity_id'er, fx sensor.kokken_meater_pro. Tom = siden er slået fra.");
    h += F("<label>Når kødet skal tages ud</label><select name=meaterAl>");
    h += String("<option value=1") + (cfg.meaterAlert ? " selected" : "") + ">tænd skærmen og blink rødt</option><option value=0" + (!cfg.meaterAlert ? " selected" : "") + ">intet</option></select>";
    flush(h);
    h += F("<h2>Skærm</h2><div class=grid>");
    field(h, "Lys i lyst rum", "blBright", String(cfg.blBright), "number"); field(h, "Lys i normalt rum", "blNormal", String(cfg.blNormal), "number");
    field(h, "Lys i mørkt rum", "blDark", String(cfg.blDark), "number"); field(h, "Lys når inaktiv", "blIdle", String(cfg.blIdle), "number");
    field(h, "Dæmp efter (min)", "idleMin", String(cfg.idleMin), "number", "0 = aldrig");
    h += F("</div><h2>Sluk skærm</h2><div class=grid>");
    field(h, "Nat fra (time)", "nightStart", String(cfg.nightStart), "number", "0-23");
    field(h, "Nat til (time)", "nightEnd", String(cfg.nightEnd), "number", "samme som 'fra' = ingen natplan");
    field(h, "Sluk når mørkt i (min)", "darkMin", String(cfg.darkMin), "number");
    field(h, "Vågn ved tryk (sek)", "wakeSec", String(cfg.wakeSec), "number");
    h += F("</div><label>Sluk i mørke</label><select name=darkOff>");
    h += String("<option value=1") + (cfg.darkOff ? " selected" : "") + ">ja, når lyset i rummet er slukket</option><option value=0" + (!cfg.darkOff ? " selected" : "") + ">nej</option></select>";
    h += F("<label>Vending (gælder efter genstart)</label><select name=rotation>");
    const char *rotNames[4] = {"Stående, USB-stik opad", "Liggende, USB-stik til højre", "Stående, USB-stik nedad", "Liggende, USB-stik til venstre"};
    for (int r = 0; r < 4; r++) { h += "<option value=" + String(r) + (cfg.rotation == r ? " selected" : "") + ">"; h += rotNames[r]; h += "</option>"; }
    h += F("</select>");
    flush(h);
    h += F("<h2>Fjernlog og opdatering</h2>");
    field(h, "Logserver", "logHost", cfg.logHost); field(h, "UDP-port", "logPort", String(cfg.logPort), "number");
    field(h, "OTA-kodeord", "otaPass", "", "password", "Tom = uændret, minus (-) = fjern.");
    h += F("<button type=submit>Gem</button></form><form method=post action=/wifi><button class=warn type=submit>Skift Wi-Fi (genstarter i opsætning)</button></form>"
           "<form method=post action=/reboot><button class=warn type=submit>Genstart</button></form></body></html>");
    flush(h, true);
}
static void setupConfigWeb() {
    cfgWeb.on("/", HTTP_GET, []() { sendPage(); });
    cfgWeb.on("/save", HTTP_POST, []() {
        auto geti = [](const char *k, int lo, int hi, int cur) { if (!cfgWeb.hasArg(k)) return cur; int v = cfgWeb.arg(k).toInt(); return v < lo ? lo : v > hi ? hi : v; };
        auto gets = [](const char *k, String cur) { if (!cfgWeb.hasArg(k)) return cur; String v = cfgWeb.arg(k); v.trim(); return v.length() ? v : cur; };
        cfg.priceArea = gets("priceArea", cfg.priceArea); cfg.surchargeOere = geti("surcharge", 0, 1000, cfg.surchargeOere);
        cfg.vat = geti("vat", 0, 1, cfg.vat) != 0;
        if (cfgWeb.hasArg("lat")) cfg.lat = cfgWeb.arg("lat").toFloat(); if (cfgWeb.hasArg("lon")) cfg.lon = cfgWeb.arg("lon").toFloat();
        cfg.place = gets("place", cfg.place);
        String key = cfgWeb.arg("rpKey"); key.trim(); if (key == "-") cfg.rpKey = ""; else if (key.length()) cfg.rpKey = key;
        cfg.stopQuery = gets("stop", cfg.stopQuery); cfg.line = gets("line", cfg.line); cfg.direction = gets("direction", cfg.direction);
        cfg.blBright = geti("blBright", 5, 255, cfg.blBright); cfg.blNormal = geti("blNormal", 5, 255, cfg.blNormal);
        cfg.blDark = geti("blDark", 5, 255, cfg.blDark); cfg.blIdle = geti("blIdle", 0, 255, cfg.blIdle); cfg.idleMin = geti("idleMin", 0, 600, cfg.idleMin);
        cfg.nightStart = geti("nightStart", 0, 23, cfg.nightStart); cfg.nightEnd = geti("nightEnd", 0, 23, cfg.nightEnd);
        cfg.darkOff = geti("darkOff", 0, 1, cfg.darkOff) != 0; cfg.darkMin = geti("darkMin", 1, 120, cfg.darkMin); cfg.wakeSec = geti("wakeSec", 5, 600, cfg.wakeSec);
        int rot = geti("rotation", 0, 3, cfg.rotation); bool rotChanged = rot != cfg.rotation; cfg.rotation = rot;
        if (cfgWeb.hasArg("logHost")) { String lh = cfgWeb.arg("logHost"); lh.trim(); cfg.logHost = lh; } cfg.logPort = geti("logPort", 1, 65535, cfg.logPort);
        rlog_set_target(cfg.logHost, cfg.logPort);
        String op = cfgWeb.arg("otaPass"); if (op == "-") cfg.otaPassword = ""; else if (op.length()) cfg.otaPassword = op;
        if (cfgWeb.hasArg("haUrl")) { String u = cfgWeb.arg("haUrl"); u.trim(); cfg.haUrl = u; }
        String ht = cfgWeb.arg("haToken"); ht.trim(); if (ht == "-") cfg.haToken = ""; else if (ht.length()) cfg.haToken = ht;
        if (cfgWeb.hasArg("meater")) { String m = cfgWeb.arg("meater"); m.trim(); cfg.meater = m; }
        cfg.meaterAlert = geti("meaterAl", 0, 1, cfg.meaterAlert) != 0;
        if (cfgWeb.hasArg("haEnt")) { String e = cfgWeb.arg("haEnt"); e.replace("\r", ""); e.trim(); cfg.haEntities = e; }
        config_save();
        xSemaphoreTake(g_mutex, portMAX_DELAY); g.refetchDeps = true; xSemaphoreGive(g_mutex);
        if (rotChanged) {                                   // the LVGL display is sized at boot: restart to apply
            cfgWeb.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><meta http-equiv=refresh content='12;url=/'><body style='font-family:sans-serif;padding:24px;background:#101820;color:#f2f4f7'>Gemt. Genstarter med ny vending...</body>");
            delay(400); ESP.restart();
        }
        cfgWeb.sendHeader("Location", "/", true); cfgWeb.send(303, "text/plain", "");
    });
    cfgWeb.on("/wifi", HTTP_POST, []() { cfgWeb.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='padding:24px'>Genstarter i Wi-Fi-opsætning. Forbind til \"Infotavle\".</body>"); delay(300); wifiPrefs.putBool("force", true); delay(300); ESP.restart(); });
    cfgWeb.on("/test/hang", HTTP_POST, []() {          // watchdog test: block the network task forever
        cfgWeb.send(200, "text/plain", "hanging the network task now; expect a task-wdt reboot within 60 s");
        delay(200);
        log_w("TEST: hanging the network task");
        for (;;) delay(1000);
    });
    cfgWeb.on("/deps", HTTP_GET, []() {                 // what the bus page currently holds (for checking against Rejseplanen)
        DepartureData d; xSemaphoreTake(g_mutex, portMAX_DELAY); d = g.deps; xSemaphoreGive(g_mutex);
        String o = "ok=" + String(d.ok ? 1 : 0) + " error=" + d.error + " stop=" + d.stopName + " fetchedAt=" + String((long)d.fetchedAt) + " now=" + String((long)time(nullptr)) + "\n";
        for (const Departure &x : d.list) {
            char pl[8], rt[8]; struct tm t; localtime_r(&x.planned, &t); strftime(pl, sizeof pl, "%H:%M", &t); localtime_r(&x.realtime, &t); strftime(rt, sizeof rt, "%H:%M", &t);
            o += x.line + " " + x.direction + " plan=" + pl + " rt=" + rt + " hasRt=" + (x.hasRealtime ? "1" : "0") + " cancelled=" + (x.cancelled ? "1" : "0") + "\n";
        }
        cfgWeb.send(200, "text/plain; charset=utf-8", o);
    });
    cfgWeb.on("/reboot", HTTP_POST, []() { cfgWeb.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='padding:24px'>Genstarter...</body>"); delay(300); ESP.restart(); });

    // Firmware upload over HTTP: tolerant of poor Wi-Fi links (no per-chunk timeout like espota).
    //   curl -F firmware=@.pio/build/<env>/firmware.bin http://<host>/update       (tools/httpupdate.sh)
    cfgWeb.on("/update", HTTP_POST, []() {
        bool ok = Update.isFinished() && !Update.hasError();
        cfgWeb.send(200, "text/plain; charset=utf-8", ok ? "OK - genstarter" : String("FEJL: ") + Update.errorString());
        delay(500);
        if (ok) ESP.restart();
        Update.abort();
        g_ota = false;
        xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = true; xSemaphoreGive(g_mutex);
    }, []() {
        HTTPUpload &up = cfgWeb.upload();
        if (up.status == UPLOAD_FILE_START) {
            if (cfg.otaPassword.length() && !cfgWeb.authenticate("admin", cfg.otaPassword.c_str())) { cfgWeb.requestAuthentication(); return; }
            g_ota = true;
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = false; g.status = "Opdaterer firmware (HTTP)..."; g.statusDirty = true; xSemaphoreGive(g_mutex);
            
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
            log_w("update aborted");
            g_ota = false;
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = true; xSemaphoreGive(g_mutex);
        }
    });
    cfgWeb.begin();
}

// ---------------- network / data task ----------------
static void netTask(void *) {
    log_i("task watchdog: net task %s", esp_err_to_name(esp_task_wdt_add(nullptr)));
    startWifiOrPortal();
    setupOta();
    setupConfigWeb();
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
    setStatus("Henter tid...");
    uint32_t t0 = millis();
    while (time(nullptr) < 1700000000 && millis() - t0 < 15000) { delay(200); esp_task_wdt_reset(); }
    xSemaphoreTake(g_mutex, portMAX_DELAY); g.ready = true; xSemaphoreGive(g_mutex);

    uint32_t nextPrices = 0, nextWeather = 0, nextDeps = 0, nextHa = 0, nextMeater = 0, lastReport = 0;
    String stopId, stopName;
    PriceData prices; WeatherData weather; DepartureData deps; HaData ha; MeaterData meater;
    for (;;) {
        esp_task_wdt_reset();
        ArduinoOTA.handle();
        if (g_ota) { delay(10); continue; }
        rlog_poll();
        cfgWeb.handleClient();
        uint32_t now = millis();
        bool refetch = false;
        xSemaphoreTake(g_mutex, portMAX_DELAY); refetch = g.refetchDeps; g.refetchDeps = false; xSemaphoreGive(g_mutex);
        if (refetch) { stopId = ""; nextDeps = now; nextPrices = now; nextWeather = now; nextHa = now; nextMeater = now; }

        if ((int32_t)(now - nextPrices) >= 0) {
            bool ok = fetchPrices(prices, cfg.priceArea.c_str());
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.prices = prices; g.pricesDirty = true; xSemaphoreGive(g_mutex);
            struct tm t; time_t tt = time(nullptr); localtime_r(&tt, &t);
            bool haveTomorrow = false;
            for (auto &s : prices.slots) if (s.t > tt + 12 * 3600) haveTomorrow = true;
            nextPrices = now + (!ok ? (prices.slots.empty() ? 20000 : 2 * 60000)      // nothing on screen yet: retry fast (DNS/TLS often fail right after boot)
                                    : (t.tm_hour >= 12 && !haveTomorrow) ? 15 * 60000 : 60 * 60000);
        }
        if ((int32_t)(now - nextWeather) >= 0) {
            bool ok = fetchWeather(weather, cfg.lat, cfg.lon);
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.weather = weather; g.weatherDirty = true; xSemaphoreGive(g_mutex);
            nextWeather = now + (ok ? 20 * 60000 : weather.fetchedAt == 0 ? 20000 : 3 * 60000);
        }
        if ((int32_t)(now - nextDeps) >= 0) {
            bool useKey = cfg.rpKey.length() > 0;              // official API when a key is stored, else the key-less legacy board
            if (stopId.isEmpty()) {
                String err;
                bool found = useKey ? resolveStop(cfg.rpKey, cfg.stopQuery, stopId, stopName, err) : resolveStopLegacy(cfg.stopQuery, stopId, stopName, err);
                if (!found) log_w("stop lookup%s: %s", useKey ? "" : " (legacy)", err.c_str());
                else log_i("stop '%s' -> %s '%s'", cfg.stopQuery.c_str(), stopId.c_str(), stopName.c_str());
            }
            bool ok = useKey ? fetchDepartures(deps, cfg.rpKey, stopId, cfg.line, cfg.direction) : fetchDeparturesLegacy(deps, stopId, cfg.line, cfg.direction);
            deps.stopName = stopName;
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.deps = deps; g.depsDirty = true; xSemaphoreGive(g_mutex);
            nextDeps = now + (ok ? 90000 : deps.fetchedAt == 0 ? 20000 : 3 * 60000);
        }
        if ((int32_t)(now - nextHa) >= 0) {
            static uint32_t haBackoff = 0;                                  // HA unreachable: 30 s, 60 s, 2 min, 4 min, then 5 min
            bool ok = fetchHomeAssistant(ha, cfg.haUrl, cfg.haToken, cfg.haEntities);
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.ha = ha; g.haDirty = true; xSemaphoreGive(g_mutex);
            haBackoff = ha.hostDown ? (haBackoff ? min<uint32_t>(haBackoff * 2, 5 * 60000) : 30000) : 0;
            nextHa = now + (cfg.haToken.isEmpty() ? 10 * 60000 : ha.hostDown ? haBackoff : ok ? 60000 : ha.fetchedAt == 0 ? 20000 : 3 * 60000);
        }
        if ((int32_t)(now - nextMeater) >= 0) {
            // HA polls Meater Cloud every 30 s: 15 s during a cook, 60 s while the probe is idle, 5 min when not set up / offline host
            bool ok = fetchMeater(meater, cfg.haUrl, cfg.haToken, cfg.meater);
            xSemaphoreTake(g_mutex, portMAX_DELAY); g.meater = meater; g.meaterDirty = true; xSemaphoreGive(g_mutex);
            nextMeater = now + (cfg.meater.isEmpty() || cfg.haToken.isEmpty() || meater.hostDown ? 5 * 60000 : !ok ? 3 * 60000 : meater.cooking ? 15000 : 60000);
        }
        // stuck on a distant mesh node? ESP32 never roams by itself: force a fresh association
        static uint32_t weakSince = 0, lastWeakReconnect = 0;
        int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
        if (rssi != 0 && rssi < -80) { if (!weakSince) weakSince = now; } else weakSince = 0;
        if (weakSince && now - weakSince > 2 * 60000u && now - lastWeakReconnect > 5 * 60000u) {
            lastWeakReconnect = now; weakSince = 0;
            log_w("weak signal (%d dBm) for 2 min, reconnecting to find a better access point", rssi);
            WiFi.disconnect(); delay(500); WiFi.reconnect();
        }
        // network watchdog: Wi-Fi gone for 2 min -> reconnect, 5 min -> restart
        static uint32_t wifiLostSince = 0, lastReconnect = 0;
        if (WiFi.status() == WL_CONNECTED) wifiLostSince = 0; else if (!wifiLostSince) wifiLostSince = now;
        if (wifiLostSince && now - wifiLostSince > 5 * 60000u) { log_e("Wi-Fi lost for 5 min, restarting"); delay(200); ESP.restart(); }
        else if (wifiLostSince && now - wifiLostSince > 2 * 60000u && now - lastReconnect > 60000u) { lastReconnect = now; log_w("Wi-Fi lost, reconnecting"); WiFi.disconnect(); delay(500); WiFi.reconnect(); }
        if (now - lastReport > 60000) {
            lastReport = now;
            log_i("heap %lu free, largest %lu, rssi %d, ldr %d, backlight %d, inactive %lus", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                  WiFi.RSSI(), g_ldr, g_level, (unsigned long)(lv_display_get_inactive_time(nullptr) / 1000));
        }
        delay(20);
    }
}

// ---------------- UI side ----------------
static void ui_sync_timer(lv_timer_t *) {
    if (xSemaphoreTake(g_mutex, 0) != pdTRUE) return;
    if (g.statusDirty) { g.statusDirty = false; if (!g.ready) ui_set_status(g.status.c_str()); }
    if (g.ready) { ui_show_pages(); ui_set_wifi_ok(g.wifiOk); }
    if (g.pricesDirty) { g.pricesDirty = false; ui_update_prices(g.prices); }
    if (g.weatherDirty) { g.weatherDirty = false; ui_update_weather(g.weather); }
    if (g.depsDirty) { g.depsDirty = false; ui_update_departures(g.deps); }
    if (g.haDirty) { g.haDirty = false; ui_update_home(g.ha); }
    bool alert = false;
    if (g.meaterDirty) { g.meaterDirty = false; alert = ui_update_meater(g.meater); }
    xSemaphoreGive(g_mutex);
    if (alert && cfg.meaterAlert) {                       // meat ready: light the screen even at night / in the dark, for 10 min
        wakeUntil = millis() + 10 * 60000u; lv_display_trigger_activity(nullptr);
        ledAlertSince = millis();
    }
    // red LED blinks until the screen is touched (or 15 min pass)
    if (ledAlertSince) {
        bool touched = lv_display_get_inactive_time(nullptr) < 300 && millis() - ledAlertSince > 1000;
        if (touched || millis() - ledAlertSince > 15 * 60000u) { ledAlertSince = 0; digitalWrite(PIN_LED_R, HIGH); }
        else digitalWrite(PIN_LED_R, (millis() / 400) % 2 ? LOW : HIGH);
    }

}
static void backlightTask() {
    static uint32_t last = 0, darkSince = 0; static int level = 200;
    if (millis() - last < 250) return;
    last = millis();
    uint32_t inactive = lv_display_get_inactive_time(nullptr);
    int ldr = analogRead(PIN_LDR);                        // low = bright, high = dark
    g_ldr = ldr;

    // night plan (local time) and dark room
    time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
    bool haveTime = t.tm_year + 1900 >= 2024;
    bool night = haveTime && cfg.nightStart != cfg.nightEnd &&
                 (cfg.nightStart < cfg.nightEnd ? (t.tm_hour >= cfg.nightStart && t.tm_hour < cfg.nightEnd)
                                                : (t.tm_hour >= cfg.nightStart || t.tm_hour < cfg.nightEnd));
    // divider on IO34 (2 x 1 MOhm + LDR to GND): ~0 in daylight, ~600 in evening room light, ~1700 with the lights off
    if (ldr > 1200) { if (!darkSince) darkSince = millis(); } else darkSince = 0;
    bool dark = cfg.darkOff && darkSince && millis() - darkSince > (uint32_t)cfg.darkMin * 60000u;
    bool awake = (int32_t)(millis() - wakeUntil) < 0;
    bool off = (night || dark) && !awake;

    int target;
    if (off) target = 0;
    else {
        target = ldr < 150 ? cfg.blBright : ldr < 800 ? cfg.blNormal : cfg.blDark;
        uint32_t idleMs = (uint32_t)cfg.idleMin * 60000u;
        if (idleMs && inactive > idleMs) target = min(target, cfg.blIdle);
    }
    level += (target - level) / 3;
    if (target > 0 && level < 3) level = 3;
    if (target == 0 && level < 3) level = 0;
    screenOff = off && level == 0;
    g_level = level;
    lcd.setBrightness(level);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Infotavle ===");
    rlog_begin(OTA_HOSTNAME);
    static const char *reasons[] = {"unknown", "power-on", "external", "software", "panic", "int-wdt", "task-wdt", "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr-glitch", "cpu-lockup"};
    int rr = (int)esp_reset_reason();
    const esp_app_desc_t *app = esp_app_get_description();
    char runSha[9] = {0}; for (int i = 0; i < 4; i++) sprintf(runSha + 2 * i, "%02x", app->app_elf_sha256[i]);
    String boot = String("BOOT infotavle build ") + __DATE__ + " " + __TIME__ + " elf=" + runSha + ", reset: " + reasons[(rr >= 0 && rr < 16) ? rr : 0];
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t sum;
        if (esp_core_dump_get_summary(&sum) == ESP_OK) {
            char sha[9] = {0}; memcpy(sha, sum.app_elf_sha256, 8);
            boot += String(" | CRASH task=") + sum.exc_task + " cause=0x" + String(sum.ex_info.exc_cause, HEX) + " vaddr=0x" + String(sum.ex_info.exc_vaddr, HEX) + " pc=0x" + String(sum.exc_pc, HEX) + " elf=" + sha + " bt=";
            for (uint32_t i = 0; i < sum.exc_bt_info.depth && i < 12; i++) { boot += "0x"; boot += String(sum.exc_bt_info.bt[i], HEX); boot += " "; }
        } else boot += " | CRASH dump present";
        esp_core_dump_image_erase();
    }
    Serial.println(boot);
    rlog_set_boot_report(boot);

    esp_task_wdt_config_t wdt = {}; wdt.timeout_ms = 60000; wdt.idle_core_mask = 0; wdt.trigger_panic = true;
    esp_err_t wr = esp_task_wdt_reconfigure(&wdt);
    if (wr != ESP_OK) wr = esp_task_wdt_init(&wdt);
    esp_err_t wa = esp_task_wdt_add(nullptr);
    Serial.printf("task watchdog: config %s, loop task %s\n", esp_err_to_name(wr), esp_err_to_name(wa));

    analogReadResolution(12);
    pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);          // RGB LED, active low: off
    digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
    config_load();
    rlog_set_target(cfg.logHost, cfg.logPort);
    cfg.rotation &= 3;
    if (cfg.rotation & 1) { SCREEN_W = 320; SCREEN_H = 240; }
    lcd.init(); lcd.setRotation(cfg.rotation); lcd.setBrightness(200); lcd.fillScreen(TFT_BLACK);

    draw_buf1 = static_cast<lv_color16_t *>(heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(disp, lv_flush_cb);
    lv_display_set_buffers(disp, draw_buf1, nullptr, BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lv_touch_read_cb);

    g_mutex = xSemaphoreCreateMutex();
    ui_init(SCREEN_W, SCREEN_H);
    ui_set_status("Starter...");
    lv_timer_create(ui_sync_timer, 100, nullptr);
    lv_timer_create([](lv_timer_t *) { ui_tick(); }, 1000, nullptr);
    xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
    Serial.printf("free heap after init: %lu\n", (unsigned long)ESP.getFreeHeap());
}

void loop() {
    esp_task_wdt_reset();
    lv_timer_handler();
    backlightTask();
    delay(5);
}

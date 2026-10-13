#include "data.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

static const char *DTAG = "data";

// ---------------------------------------------------------------- helpers
static time_t parseLocalIso(const char *s) {       // "2026-09-04T23:15:00" or "2026-09-04T23:15" (local time)
    struct tm t = {};
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) < 5) return 0;
    t.tm_year -= 1900; t.tm_mon -= 1; t.tm_isdst = -1;
    return mktime(&t);
}
static time_t parseLocalDate(const char *s) {      // "2026-09-05"
    struct tm t = {};
    if (sscanf(s, "%4d-%2d-%2d", &t.tm_year, &t.tm_mon, &t.tm_mday) < 3) return 0;
    t.tm_year -= 1900; t.tm_mon -= 1; t.tm_hour = 12; t.tm_isdst = -1;
    return mktime(&t);
}

// Undo HTTP chunked transfer encoding in place ("<hex>\r\n<data>\r\n" ... "0\r\n\r\n"). Returns false if not chunked.
static bool dechunkInPlace(String &body) {
    char *buf = body.begin(); size_t n = body.length(), r = 0, w = 0;
    while (r < n) {
        char *endp; unsigned long sz = strtoul(buf + r, &endp, 16);
        if (endp == buf + r) return false;                     // not a hex size: not chunked after all
        while (endp < buf + n && *endp != '\n') endp++;         // skip extensions up to CRLF
        r = (endp - buf) + 1;
        if (sz == 0) break;
        if (r + sz > n) sz = n - r;                             // truncated tail: keep what we have
        memmove(buf + w, buf + r, sz); w += sz; r += sz + 2;    // +2: CRLF after the chunk
    }
    body.remove(w);
    return true;
}

// GET; the body ends up in `body` (capped). One connection at a time: TLS needs ~45 KB of heap, plain HTTP ~5 KB.
// The body is read in our own loop with a hard deadline and watchdog resets: on a weak link a trickling download
// through http.getString() can otherwise starve the task watchdog (seen 2026-09-12 at -88 dBm).
static bool getBodyWith(NetworkClient &client, const String &url, String &body, size_t maxBytes, String &err, const char *accept, const String &bearer = String()) {
    HTTPClient http;
    http.setTimeout(12000);
    http.setReuse(false);
    http.addHeader("Accept", accept);
    if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
    if (!http.begin(client, url)) { err = "begin"; return false; }
    esp_task_wdt_reset();
    int code = http.GET();
    esp_task_wdt_reset();
    if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
    int size = http.getSize();                                  // -1 when chunked / unknown
    if (size > (int)maxBytes) { err = "svar for stort"; http.end(); return false; }
    body = String();
    if (!body.reserve(size > 0 ? size + 1 : 4096)) { err = "hukommelse"; http.end(); return false; }
    NetworkClient *st = http.getStreamPtr();
    uint32_t deadline = millis() + 30000, lastData = millis();
    char chunk[512];
    while (size < 0 || (int)body.length() < size) {
        int avail = st ? st->available() : 0;
        if (avail > 0) {
            int n = st->readBytes(chunk, avail > (int)sizeof chunk ? sizeof chunk : avail);
            if (n <= 0) break;
            if (body.length() + n > maxBytes) { err = "svar for stort"; body = String(); http.end(); return false; }
            body.concat(chunk, n); lastData = millis();
            esp_task_wdt_reset();
            if (size < 0 && body.endsWith("\r\n0\r\n\r\n")) break;   // terminating chunk seen: don't wait for the server to close
        } else {
            if (!st || !st->connected()) break;                 // server closed: body complete (or connection lost)
            if (millis() - lastData > 12000) { err = "timeout"; break; }
            delay(5);
        }
        if ((int32_t)(millis() - deadline) > 0) { err = "for langsomt"; break; }
    }
    http.end();
    if (err.isEmpty() && size > 0 && (int)body.length() < size) err = "afbrudt";
    if (err.length()) { body = String(); return false; }
    if (size < 0 && body.length()) dechunkInPlace(body);     // no-op when the server sent a plain, close-delimited body
    return body.length() > 0;
}
static bool httpsGetBody(const String &url, String &body, size_t maxBytes, String &err, const String &bearer = String()) {
    WiFiClientSecure client;
    client.setInsecure();                          // public data; no certificate bundle on the device
    client.setTimeout(12);
    return getBodyWith(client, url, body, maxBytes, err, "application/json", bearer);
}
static bool httpGetBody(const String &url, String &body, size_t maxBytes, String &err, const String &bearer = String()) {
    WiFiClient client;
    client.setTimeout(12);
    return getBodyWith(client, url, body, maxBytes, err, "*/*", bearer);
}

// Rejseplanen's legacy HAFAS endpoints speak Latin-1 and use numeric entities (&#216;) for a few characters.
static void appendUtf8(String &o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
static String latin1EntitiesToUtf8(const String &in) {
    String o; o.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        unsigned char c = in[i];
        if (c == '&') {
            int semi = in.indexOf(';', i);
            if (semi > 0 && semi - i <= 8) {
                String ent = in.substring(i + 1, semi);
                if (ent[0] == '#') { appendUtf8(o, (uint32_t)ent.substring(1).toInt()); i = semi; continue; }
                if (ent == "amp") { o += '&'; i = semi; continue; }
                if (ent == "quot") { o += '"'; i = semi; continue; }
                if (ent == "lt") { o += '<'; i = semi; continue; }
                if (ent == "gt") { o += '>'; i = semi; continue; }
                if (ent == "apos") { o += '\''; i = semi; continue; }
            }
        }
        appendUtf8(o, c);                          // ASCII, or a raw Latin-1 byte
    }
    return o;
}
static String utf8ToLatin1UrlEncoded(const String &in) {  // query strings for the legacy endpoints
    String o;
    for (size_t i = 0; i < in.length(); i++) {
        unsigned char c = in[i]; uint32_t cp = c;
        if (c >= 0xC0 && c < 0xE0 && i + 1 < in.length()) { cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F); i++; }
        else if (c >= 0xE0 && i + 2 < in.length()) { cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F); i += 2; }
        if (cp < 0x80 && isalnum((int)cp)) o += (char)cp;
        else { if (cp > 0xFF) cp = '?'; char h[4]; snprintf(h, 4, "%%%02X", (unsigned)cp); o += h; }
    }
    return o;
}
static String xmlAttr(const String &seg, const char *name) {  // value of name="..." inside one element
    String key = String(" ") + name + "=\"";
    int a = seg.indexOf(key); if (a < 0) return String();
    a += key.length();
    int b = seg.indexOf('"', a); if (b < 0) return String();
    return seg.substring(a, b);
}

// ---------------------------------------------------------------- prices
// Streaming parser for the EDS DayAheadPrices JSON. The records array is scanned as it arrives, so the whole body
// (12-35 KB) is never held in RAM: during a TLS session mbedTLS owns ~40 KB and the largest free block is often too
// small for it ("prices: hukommelse", 15x in the week to 2026-09-19). pumpBody() below de-chunks for us.
class PriceSink : public Stream {
public:
    std::vector<PriceSlot> slots;
    bool tooBig = false, tooSlow = false;
    explicit PriceSink(size_t cap, uint32_t deadlineMs) : _cap(cap), _deadline(deadlineMs) { _buf.reserve(1536); slots.reserve(200); }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *d, size_t n) override {
        if ((_total += n) > _cap) { tooBig = true; return 0; }                       // short write aborts pumpBody()
        if ((int32_t)(millis() - _deadline) > 0) { tooSlow = true; return 0; }
        _buf.concat((const char *)d, n);
        esp_task_wdt_reset();
        parse();
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
private:
    String _buf; size_t _total = 0, _cap; uint32_t _deadline;
    void parse() {
        while (true) {
            int a = _buf.indexOf("\"TimeDK\"");
            if (a < 0) {                                                           // keep a tail: the key may be split
                if (_buf.length() > 32) _buf.remove(0, _buf.length() - 16);
                return;
            }
            if (a > 0) _buf.remove(0, a);
            int colon = 8; while (colon < (int)_buf.length() && _buf[colon] == ' ') colon++;
            if (colon >= (int)_buf.length()) return;
            if (_buf[colon] != ':') { _buf.remove(0, 8); continue; }               // "sort":"TimeDK" in the header, not a key
            int q = _buf.indexOf('"', colon); if (q < 0) return;                  // opening quote of the value
            int qe = _buf.indexOf('"', q + 1); if (qe < 0) return;
            int k = _buf.indexOf("\"DayAheadPriceDKK\"", qe); if (k < 0) return;
            int c = _buf.indexOf(':', k); if (c < 0) return;
            int v = c + 1; while (v < (int)_buf.length() && _buf[v] == ' ') v++;
            int e = v; while (e < (int)_buf.length() && _buf[e] != ',' && _buf[e] != '}') e++;
            if (e >= (int)_buf.length()) return;                                  // value not complete yet
            time_t ts = parseLocalIso(_buf.c_str() + q + 1);
            if (ts && !_buf.startsWith("null", v)) {
                PriceSlot sl; sl.t = ts; sl.oere = (int16_t)lroundf(atof(_buf.c_str() + v) / 10.0f);   // DKK/MWh -> øre/kWh
                slots.push_back(sl);
            }
            _buf.remove(0, e);
        }
    }
};

// Pumps the response body into `sink` in 512-byte pieces read on the stack, de-chunking on the fly. Do NOT use
// HTTPClient::writeToStream here: it mallocs a 4 KB buffer, and during a TLS session (mbedTLS holds ~40 KB) the heap
// has no 4 KB block left -> "too less ram! need 4096" on every price fetch from 2026-09-20 00:00 until this was written.
static bool pumpBody(HTTPClient &http, Stream &sink, String &err, uint32_t deadlineMs) {
    int size = http.getSize();                                                    // -1 = chunked or close-delimited
    bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
    NetworkClient *st = http.getStreamPtr();
    if (!st) { err = "ingen stream"; return false; }
    enum { SIZE, DATA, DATA_CRLF, DONE } cs = chunked ? SIZE : DATA;
    size_t remaining = chunked ? 0 : (size > 0 ? (size_t)size : SIZE_MAX);        // SIZE_MAX: read until the server closes
    String sizeLine;
    char buf[512];
    uint32_t lastData = millis();
    while (cs != DONE) {
        int avail = st->available();
        if (avail <= 0) {
            if (!st->connected()) {
                if (!chunked && size < 0) break;                                  // close-delimited body: complete
                err = "afbrudt"; return false;
            }
            if (millis() - lastData > 12000) { err = "timeout"; return false; }
            if ((int32_t)(millis() - deadlineMs) > 0) { err = "for langsomt"; return false; }
            delay(5);
            continue;
        }
        int n = st->readBytes(buf, avail > (int)sizeof buf ? sizeof buf : avail);
        if (n <= 0) continue;
        lastData = millis();
        esp_task_wdt_reset();
        int i = 0;
        while (i < n && cs != DONE) {
            if (cs == SIZE) {                                                     // hex chunk size, optional ";ext", CRLF
                char c = buf[i++];
                if (c == '\n') { remaining = strtoul(sizeLine.c_str(), nullptr, 16); sizeLine = ""; cs = remaining ? DATA : DONE; }
                else if (c != '\r' && sizeLine.length() < 16) sizeLine += c;
            } else if (cs == DATA) {
                size_t take = (size_t)(n - i) < remaining ? (size_t)(n - i) : remaining;
                if (sink.write((const uint8_t *)buf + i, take) != take) { err = "afbrudt"; return false; }   // sink gave up
                i += take; remaining -= take;
                if (remaining == 0) cs = chunked ? DATA_CRLF : DONE;
            } else if (cs == DATA_CRLF) {
                if (buf[i++] == '\n') cs = SIZE;
            }
        }
    }
    return true;
}

// HTTPS GET whose body is pumped into `sink` (Stream::write) instead of a String.
static bool httpsGetToSink(const String &url, Stream &sink, String &err, uint32_t deadlineMs) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(12);
    HTTPClient http;
    http.setTimeout(12000);
    http.setReuse(false);
    const char *hdrs[] = {"Transfer-Encoding"};
    http.collectHeaders(hdrs, 1);
    http.addHeader("Accept", "application/json");
    if (!http.begin(client, url)) { err = "begin"; return false; }
    esp_task_wdt_reset();
    int code = http.GET();
    esp_task_wdt_reset();
    if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
    bool ok = pumpBody(http, sink, err, deadlineMs);
    http.end();
    return ok;
}

bool fetchPrices(PriceData &out, const char *priceArea) {
    time_t now = time(nullptr);
    struct tm t; localtime_r(&now, &t);
    char start[16], end[16];
    strftime(start, sizeof start, "%Y-%m-%d", &t);
    t.tm_mday += 2; t.tm_hour = 0; mktime(&t);
    strftime(end, sizeof end, "%Y-%m-%d", &t);
    String url = String("https://api.energidataservice.dk/dataset/DayAheadPrices?start=") + start + "&end=" + end +
                 "&filter=%7B%22PriceArea%22%3A%5B%22" + priceArea + "%22%5D%7D&columns=TimeDK%2CDayAheadPriceDKK&sort=TimeDK&limit=200";
    String err;
    uint32_t deadline = millis() + 30000;
    PriceSink sink(64 * 1024, deadline);
    bool ok = httpsGetToSink(url, sink, err, deadline);
    if (sink.tooBig) { ok = false; err = "svar for stort"; }
    if (sink.tooSlow) { ok = false; err = "for langsomt"; }
    if (!ok) { out.error = err; out.ok = !out.slots.empty(); log_w("[%s] prices: %s", DTAG, err.c_str()); return false; }
    std::vector<PriceSlot> &slots = sink.slots;
    if (slots.empty()) { out.error = "ingen data"; log_w("[%s] prices: no records", DTAG); return false; }
    out.slots = slots; out.fetchedAt = now; out.ok = true; out.error = "";
    log_i("[%s] prices: %d slots from %s", DTAG, slots.size(), start);
    return true;
}

// ---------------------------------------------------------------- weather
bool fetchWeather(WeatherData &out, float lat, float lon) {
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 3) + "&longitude=" + String(lon, 3) +
                 "&current=temperature_2m,weather_code,wind_speed_10m,precipitation"
                 "&hourly=temperature_2m,precipitation_probability&forecast_hours=24"
                 "&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_sum"
                 "&timezone=Europe%2FCopenhagen&forecast_days=4&wind_speed_unit=ms";
    String body, err;
    if (!httpsGetBody(url, body, 12 * 1024, err)) { out.error = err; log_w("[%s] weather: %s", DTAG, err.c_str()); return false; }
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    body = String();
    if (e) { out.error = "json"; log_w("[%s] weather json: %s", DTAG, e.c_str()); return false; }
    JsonObject cur = doc["current"];
    out.temp = cur["temperature_2m"] | 0.0f; out.wind = cur["wind_speed_10m"] | 0.0f;
    out.precip = cur["precipitation"] | 0.0f; out.code = cur["weather_code"] | 0;
    JsonArray ht = doc["hourly"]["time"], hT = doc["hourly"]["temperature_2m"], hP = doc["hourly"]["precipitation_probability"];
    out.hourlyCount = 0;
    if (ht.size()) out.hourlyStart = parseLocalIso(ht[0].as<const char *>());
    for (size_t i = 0; i < ht.size() && i < 24; i++) { out.hourlyTemp[i] = (int8_t)lroundf(hT[i] | 0.0f); out.hourlyPop[i] = (uint8_t)(hP[i] | 0); out.hourlyCount++; }
    JsonArray dt = doc["daily"]["time"];
    out.dayCount = 0;
    for (size_t i = 0; i < dt.size() && i < 4; i++) {
        WeatherDay &d = out.days[i];
        d.date = parseLocalDate(dt[i].as<const char *>());
        d.tmax = doc["daily"]["temperature_2m_max"][i] | 0.0f; d.tmin = doc["daily"]["temperature_2m_min"][i] | 0.0f;
        d.code = doc["daily"]["weather_code"][i] | 0; d.rain = doc["daily"]["precipitation_sum"][i] | 0.0f;
        out.dayCount++;
    }
    out.ok = true; out.error = ""; out.fetchedAt = time(nullptr);
    log_i("[%s] weather: %.1f C code %d, %d days", DTAG, out.temp, out.code, out.dayCount);
    return true;
}

// ---------------------------------------------------------------- departures (Rejseplanen HAFAS ReST)
static const char *RP_BASE = "https://www.rejseplanen.dk/api/";

bool resolveStop(const String &apiKey, const String &stopQuery, String &stopIdOut, String &stopNameOut, String &err) {
    if (apiKey.isEmpty()) { err = "API-nøgle mangler"; return false; }
    String q; for (size_t i = 0; i < stopQuery.length(); i++) { char c = stopQuery[i]; if (isalnum((unsigned char)c)) q += c; else { char h[4]; snprintf(h, 4, "%%%02X", (unsigned char)c); q += h; } }
    String url = String(RP_BASE) + "location.name?accessId=" + apiKey + "&input=" + q + "&format=json&type=S&maxNo=3";
    String body;
    if (!httpsGetBody(url, body, 16 * 1024, err)) return false;
    JsonDocument filter;
    filter["stopLocationOrCoordLocation"][0]["StopLocation"]["extId"] = true;
    filter["stopLocationOrCoordLocation"][0]["StopLocation"]["name"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();
    if (e) { err = "json"; return false; }
    JsonArray arr = doc["stopLocationOrCoordLocation"];
    for (JsonObject o : arr) {
        JsonObject sl = o["StopLocation"];
        if (!sl.isNull()) { stopIdOut = sl["extId"].as<String>(); stopNameOut = sl["name"].as<String>(); return stopIdOut.length() > 0; }
    }
    err = "stop ikke fundet";
    return false;
}

bool fetchDepartures(DepartureData &out, const String &apiKey, const String &stopId, const String &line, const String &directionContains) {
    if (apiKey.isEmpty()) { out.error = "API-nøgle mangler"; out.ok = false; return false; }
    if (stopId.isEmpty()) { out.error = "stop ikke fundet"; out.ok = false; return false; }
    String url = String(RP_BASE) + "departureBoard?accessId=" + apiKey + "&id=" + stopId + "&format=json&duration=180&maxJourneys=20";
    if (line.length()) url += "&lines=" + line;
    String body, err;
    if (!httpsGetBody(url, body, 40 * 1024, err)) { out.error = err; log_w("[%s] departures: %s", DTAG, err.c_str()); return false; }
    JsonDocument filter;
    JsonObject f = filter["Departure"][0];
    f["name"] = true; f["direction"] = true; f["time"] = true; f["date"] = true; f["rtTime"] = true; f["rtDate"] = true; f["cancelled"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();
    if (e) { out.error = "json"; log_w("[%s] departures json: %s", DTAG, e.c_str()); return false; }
    std::vector<Departure> list;
    for (JsonObject d : doc["Departure"].as<JsonArray>()) {
        String dir = d["direction"] | "";
        if (directionContains.length() && dir.indexOf(directionContains) < 0) continue;
        Departure dep;
        dep.line = d["name"] | ""; dep.line.replace("Bus ", "");
        dep.direction = dir;
        String date = d["date"] | "", tm_ = d["time"] | "";
        dep.planned = parseLocalIso((date + "T" + tm_).c_str());
        dep.hasRealtime = d["rtTime"].is<const char *>();
        if (dep.hasRealtime) { String rd = d["rtDate"] | date, rt = d["rtTime"] | ""; dep.realtime = parseLocalIso((rd + "T" + rt).c_str()); }
        else dep.realtime = dep.planned;
        dep.cancelled = d["cancelled"] | false;
        if (dep.planned) list.push_back(dep);
        if (list.size() >= 8) break;
    }
    out.list = list; out.ok = true; out.error = ""; out.fetchedAt = time(nullptr);
    log_i("[%s] departures: %d", DTAG, list.size());
    return true;
}

// ---------------------------------------------------------------- departures without an API key (legacy HAFAS station board)
// Rejseplanen's old web endpoints still answer without a key, over plain HTTP, and the vs_java3 board is a few
// hundred bytes of one <Journey .../> element per departure with real-time delay. Undocumented: may vanish.
static const char *RP_LEGACY = "http://www.rejseplanen.dk/bin/";

bool resolveStopLegacy(const String &stopQuery, String &stopIdOut, String &stopNameOut, String &err) {
    String url = String(RP_LEGACY) + "ajax-getstop.exe/mny?getstop=1&noSession=yes&REQ0JourneyStopsB=5&js=true&S=" + utf8ToLatin1UrlEncoded(stopQuery) + "?";
    String body;
    if (!httpGetBody(url, body, 12 * 1024, err)) return false;
    int a = body.indexOf('{'), b = body.lastIndexOf('}');
    if (a < 0 || b < a) { err = "svar"; return false; }
    body = body.substring(a, b + 1);
    JsonDocument filter;
    filter["suggestions"][0]["value"] = true; filter["suggestions"][0]["extId"] = true; filter["suggestions"][0]["type"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();
    if (e) { err = "json"; return false; }
    for (JsonObject o : doc["suggestions"].as<JsonArray>()) {
        if (String(o["type"] | "") != "1") continue;            // 1 = stop/station
        stopIdOut = o["extId"] | ""; stopNameOut = latin1EntitiesToUtf8(o["value"] | "");
        if (stopIdOut.length()) return true;
    }
    err = "stop ikke fundet";
    return false;
}

bool fetchDeparturesLegacy(DepartureData &out, const String &stopId, const String &line, const String &directionContains) {
    if (stopId.isEmpty()) { out.error = "stop ikke fundet"; out.ok = false; return false; }
    String url = String(RP_LEGACY) + "stboard.exe/mn?input=" + stopId + "&boardType=dep&time=now&start=yes&L=vs_java3&maxJourneys=24";
    String body, err;
    if (!httpGetBody(url, body, 16 * 1024, err)) { out.error = err; log_w("[%s] departures(legacy): %s", DTAG, err.c_str()); return false; }
    std::vector<Departure> list;
    int pos = 0;
    while ((pos = body.indexOf("<Journey ", pos)) >= 0) {
        int end = body.indexOf("/>", pos); if (end < 0) break;
        String seg = body.substring(pos, end); pos = end + 2;
        Departure dep;
        dep.line = latin1EntitiesToUtf8(xmlAttr(seg, "line"));
        if (dep.line.isEmpty()) { dep.line = latin1EntitiesToUtf8(xmlAttr(seg, "prod")); int h = dep.line.indexOf('#'); if (h >= 0) dep.line = dep.line.substring(0, h); dep.line.replace("Bybus ", ""); dep.line.replace("Bus ", ""); }
        if (line.length() && !dep.line.equalsIgnoreCase(line)) continue;
        dep.direction = latin1EntitiesToUtf8(xmlAttr(seg, "dir"));
        String target = latin1EntitiesToUtf8(xmlAttr(seg, "targetLoc"));
        if (dep.direction.isEmpty()) dep.direction = target;
        if (directionContains.length() && dep.direction.indexOf(directionContains) < 0 && target.indexOf(directionContains) < 0) continue;
        String d = xmlAttr(seg, "fpDate"), t = xmlAttr(seg, "fpTime");     // "12.09.26" "16:26"
        struct tm tm_ = {};
        if (sscanf(d.c_str(), "%2d.%2d.%2d", &tm_.tm_mday, &tm_.tm_mon, &tm_.tm_year) < 3 || sscanf(t.c_str(), "%2d:%2d", &tm_.tm_hour, &tm_.tm_min) < 2) continue;
        tm_.tm_year += 100; tm_.tm_mon -= 1; tm_.tm_isdst = -1;
        dep.planned = mktime(&tm_);
        // delay = confirmed delay, e_delay = current prognosis (what the Midttrafik/Rejseplanen apps show); prefer the prognosis
        String delay = xmlAttr(seg, "delay"), eDelay = xmlAttr(seg, "e_delay");
        auto numeric = [](const String &v) { return v.length() && v != "-" && (isdigit((unsigned char)v[0]) || v[0] == '-'); };
        dep.cancelled = delay == "cancel" || eDelay == "cancel";
        String use = numeric(eDelay) ? eDelay : delay;
        dep.hasRealtime = !dep.cancelled && numeric(use);
        dep.realtime = dep.hasRealtime ? dep.planned + use.toInt() * 60 : dep.planned;
        if (dep.planned) list.push_back(dep);
        if (list.size() >= 8) break;
    }
    body = String();
    out.list = list; out.ok = true; out.error = ""; out.fetchedAt = time(nullptr);
    log_i("[%s] departures(legacy): %d", DTAG, list.size());
    return true;
}

// ---------------------------------------------------------------- Home Assistant (REST /api/states/<entity>)
// Entity line syntax on the web page:  entity_id|Label      or, to count how many of several are "on"/open:
//   binary_sensor.a+binary_sensor.b+...|Åbne vinduer        -> "2" / "Ingen"
static String haStateText(const String &st, const String &domain, const String &devClass) {
    struct { const char *k, *v; } common[] = {{"home", "Hjemme"}, {"not_home", "Ude"}, {"open", "Åben"}, {"closed", "Lukket"}, {"opening", "Åbner"},
        {"closing", "Lukker"}, {"locked", "Låst"}, {"unlocked", "Ulåst"}, {"heat", "Varme"}, {"cool", "Køl"}, {"idle", "Hviler"}, {"heating", "Varmer"},
        {"playing", "Spiller"}, {"paused", "Pause"}, {"unavailable", "–"}, {"unknown", "?"}, {"charging", "Lader"}, {"discharging", "Aflader"},
        {"disarmed", "Frakoblet"}, {"armed_home", "Tilkoblet hjemme"}, {"armed_away", "Tilkoblet"}, {"armed_night", "Nat"}, {"arming", "Tilkobler"}, {"pending", "Afventer"}, {"triggered", "UDLØST"},
        {"docked", "I dock"}, {"cleaning", "Støvsuger"}, {"returning", "På vej hjem"}, {"mowing", "Slår græs"}, {"error", "Fejl"}, {"standby", "Standby"},
        {"clear-night", "Klart"}, {"cloudy", "Skyet"}, {"rainy", "Regn"}, {"sunny", "Sol"}, {"partlycloudy", "Let skyet"}, {"fog", "Tåge"}, {"snowy", "Sne"}};
    for (auto &e : common) if (st == e.k) return e.v;
    if (st == "on" || st == "off") {                    // binary sensors read differently depending on what they sense
        bool on = st == "on";
        if (domain == "binary_sensor") {
            if (devClass == "opening" || devClass == "door" || devClass == "window" || devClass == "garage_door") return on ? "Åben" : "Lukket";
            if (devClass == "lock") return on ? "Ulåst" : "Låst";
            if (devClass == "motion" || devClass == "occupancy" || devClass == "presence") return on ? "Bevægelse" : "Ingen";
            if (devClass == "plug") return on ? "Tilsluttet" : "Frakoblet";
            if (devClass == "battery_charging") return on ? "Lader" : "Lader ikke";
            if (devClass == "connectivity") return on ? "Forbundet" : "Afbrudt";
            if (devClass == "problem") return on ? "Problem" : "OK";
            if (devClass == "moisture") return on ? "Fugt!" : "Tør";
            if (devClass == "running") return on ? "Kører" : "Stoppet";
            if (devClass == "smoke" || devClass == "gas" || devClass == "carbon_monoxide") return on ? "ALARM" : "OK";
        }
        return on ? "Tændt" : "Slukket";
    }
    char *end; double v = strtod(st.c_str(), &end);
    if (end != st.c_str() && *end == 0) {              // numeric: at most one decimal, no trailing ",0"
        char b[24]; if (fabs(v - round(v)) < 0.05) snprintf(b, sizeof b, "%.0f", v); else snprintf(b, sizeof b, "%.1f", v);
        String r(b); r.replace('.', ','); return r;
    }
    return st;
}

// One entity: fills state/unit/label. Returns false on error (state then holds a short Danish reason).
static bool haGetEntity(const String &base, bool tls, const String &token, const String &id, String &state, String &unit, String &label, String &err) {
    String url = base + "/api/states/" + id, body;
    bool got = tls ? httpsGetBody(url, body, 8 * 1024, err, token) : httpGetBody(url, body, 8 * 1024, err, token);
    if (!got) { state = err == "HTTP 404" ? "findes ikke" : err == "HTTP 401" ? "token afvist" : err; return false; }
    JsonDocument filter; filter["state"] = true; filter["attributes"]["friendly_name"] = true; filter["attributes"]["unit_of_measurement"] = true; filter["attributes"]["device_class"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();
    if (e) { state = "json"; err = "json"; return false; }
    if (label.isEmpty()) label = doc["attributes"]["friendly_name"] | id;
    unit = doc["attributes"]["unit_of_measurement"] | "";
    String raw = doc["state"] | "?", devClass = doc["attributes"]["device_class"] | "";
    state = haStateText(raw, id.substring(0, id.indexOf('.')), devClass);
    return true;
}

// Host-level failure (connection refused / timeout / no route): HA is down or restarting. Every entity would fail the
// same way, so the cycle stops at the first one; the caller backs off. Seen 2026-09-14 and -19: HA restarts produced
// 18 connect attempts per second and ~235 error lines. HTTP 4xx/5xx are per-entity answers and do not count.
static bool haHostError(const String &err) {
    return err.startsWith("HTTP -") || err == "begin" || err == "timeout" || err == "for langsomt";
}

bool fetchHomeAssistant(HaData &out, const String &baseUrl, const String &token, const String &entitiesSpec) {
    out.hostDown = false;
    if (token.isEmpty()) { out.error = "Token mangler"; out.ok = false; return false; }
    if (baseUrl.isEmpty()) { out.error = "URL mangler"; out.ok = false; return false; }
    std::vector<HaItem> items;
    int pos = 0;
    while (pos < (int)entitiesSpec.length() && items.size() < 8) {   // "entity_id|Label" per line
        int nl = entitiesSpec.indexOf('\n', pos); if (nl < 0) nl = entitiesSpec.length();
        String line = entitiesSpec.substring(pos, nl); pos = nl + 1; line.trim(); if (line.isEmpty() || line[0] == '#') continue;
        HaItem it; int bar = line.indexOf('|');
        it.id = bar >= 0 ? line.substring(0, bar) : line; it.id.trim();
        it.label = bar >= 0 ? line.substring(bar + 1) : String(); it.label.trim();
        it.ok = false; items.push_back(it);
    }
    if (items.empty()) { out.error = "Ingen enheder valgt"; out.ok = false; out.items.clear(); return false; }
    String base = baseUrl; while (base.endsWith("/")) base.remove(base.length() - 1);
    bool tls = base.startsWith("https://");
    int okCount = 0; String firstErr;
    auto hostDown = [&](const String &err, int done) {
        out.hostDown = true; out.error = "Home Assistant svarer ikke"; out.ok = !out.items.empty();   // keep last values on screen
        log_w("[%s] home assistant: svarer ikke (%s) efter %d/%d enheder, resten springes over", DTAG, err.c_str(), done, (int)items.size());
    };
    int done = 0;
    for (HaItem &it : items) {
        String err;
        if (it.id.indexOf('+') >= 0) {                  // group: count members that are on/open/unlocked
            int onCount = 0, members = 0, failed = 0; int p = 0;
            while (p <= (int)it.id.length()) {
                int plus = it.id.indexOf('+', p); if (plus < 0) plus = it.id.length();
                String mid = it.id.substring(p, plus); mid.trim(); p = plus + 1; if (mid.isEmpty()) continue;
                String st, un, lb = "x";
                if (!haGetEntity(base, tls, token, mid, st, un, lb, err)) {
                    if (haHostError(err)) { hostDown(err, done); return false; }
                    failed++; if (firstErr.isEmpty()) firstErr = mid + ": " + st; continue;
                }
                members++;
                if (st == "Åben" || st == "Tændt" || st == "Ulåst" || st == "Bevægelse" || st == "Problem" || st == "Kører" || st == "Lader" || st == "Fugt!" || st == "ALARM" || st == "Åbner") onCount++;
            }
            done++;
            if (!members) { it.state = failed ? "findes ikke" : "tom"; continue; }
            it.state = onCount ? String(onCount) : "Ingen"; it.unit = ""; it.ok = true; okCount++;
            if (it.label.isEmpty()) it.label = "Aktive";
            continue;
        }
        if (haGetEntity(base, tls, token, it.id, it.state, it.unit, it.label, err)) { it.ok = true; okCount++; }
        else if (haHostError(err)) { hostDown(err, done); return false; }
        else if (firstErr.isEmpty()) firstErr = it.id + ": " + it.state;
        done++;
    }
    out.items = items; out.fetchedAt = time(nullptr);
    out.ok = okCount > 0; out.error = okCount == (int)items.size() ? "" : firstErr;
    log_i("[%s] home assistant: %d/%d ok%s%s", DTAG, okCount, (int)items.size(), firstErr.length() ? ", " : "", firstErr.c_str());
    return out.ok;
}

// ---------------------------------------------------------------- Meater (via Home Assistant)
// Entity ids come from HA's English translation keys: <prefix>_internal_temperature, _ambient_temperature, _cook_state,
// _target_temperature, _peak_temperature, _time_remaining, _time_elapsed, _cooking. The cook sensors are "unavailable"
// when no cook is set up; all of them are when the probe is not connected to Meater Cloud (phone app / Block out of range).
static const char *MEATER_SUFFIXES[] = {"_internal_temperature", "_ambient_temperature", "_cook_state", "_target_temperature",
    "_peak_temperature", "_time_remaining", "_time_elapsed", "_cooking", "_internal", "_ambient"};

String meaterPrefix(const String &configured) {
    String p = configured; p.trim();
    while (p.endsWith("_")) p.remove(p.length() - 1);
    for (const char *suf : MEATER_SUFFIXES) if (p.endsWith(suf)) { p.remove(p.length() - strlen(suf)); break; }
    if (p.length() && p.indexOf('.') < 0) p = "sensor." + p;
    return p;
}

// Raw state of one entity ("unavailable", "54.2", "2026-09-29T17:12:03.123+00:00").
static bool haGetRaw(const String &base, bool tls, const String &token, const String &id, String &raw, String &err) {
    String url = base + "/api/states/" + id, body;
    bool got = tls ? httpsGetBody(url, body, 4 * 1024, err, token) : httpGetBody(url, body, 4 * 1024, err, token);
    if (!got) return false;
    JsonDocument filter; filter["state"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();
    if (e) { err = "json"; return false; }
    raw = doc["state"] | "unknown";
    return true;
}
static bool haMissing(const String &raw) { return raw == "unavailable" || raw == "unknown" || raw.isEmpty(); }
static float haNum(const String &raw) { char *end; float v = strtof(raw.c_str(), &end); return end != raw.c_str() ? v : NAN; }

// ISO 8601 with offset -> UTC epoch, without relying on timegm/TZ.
static time_t isoToEpoch(const String &s) {
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    y -= mo <= 2; int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = y - era * 400;              // days from civil (H. Hinnant)
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    long t = days * 86400L + h * 3600L + mi * 60L + se;
    int tz = s.indexOf('T'); int off = -1;
    for (int i = tz + 1; i < (int)s.length(); i++) if (s[i] == '+' || s[i] == '-') { off = i; break; }
    if (off > 0) { int oh = 0, om = 0; sscanf(s.c_str() + off + 1, "%d:%d", &oh, &om); long o = oh * 3600L + om * 60L; t -= s[off] == '+' ? o : -o; }
    return (time_t)t;
}

bool fetchMeater(MeaterData &out, const String &baseUrl, const String &token, const String &prefixCfg) {
    out.hostDown = false;
    String prefix = meaterPrefix(prefixCfg);
    if (prefix.isEmpty()) { out = MeaterData(); out.error = "Meater ikke sat op"; return false; }
    if (token.isEmpty()) { out.ok = false; out.error = "Home Assistant-token mangler"; return false; }
    String base = baseUrl; while (base.endsWith("/")) base.remove(base.length() - 1);
    bool tls = base.startsWith("https://");
    MeaterData m;
    String raw, err;
    auto get = [&](const char *suffix) -> bool {
        raw = ""; err = "";
        if (haGetRaw(base, tls, token, prefix + suffix, raw, err)) return true;
        if (haHostError(err)) { out.hostDown = true; out.ok = false; out.error = "Home Assistant svarer ikke"; }
        else { out.ok = false; out.error = err == "HTTP 404" ? String("findes ikke: ") + prefix + suffix : err == "HTTP 401" ? "token afvist" : err; }
        log_w("[%s] meater: %s%s: %s", DTAG, prefix.c_str(), suffix, err.c_str());
        return false;
    };
    if (!get("_internal_temperature")) return false;
    m.ok = true; m.online = !haMissing(raw); m.internal = haNum(raw);
    if (m.online) {
        if (get("_ambient_temperature") && !haMissing(raw)) m.ambient = haNum(raw);
        if (out.hostDown) return false;
        if (get("_cook_state") && !haMissing(raw)) { m.cooking = true; m.state = raw; }
        if (m.cooking) {
            if (get("_target_temperature") && !haMissing(raw)) m.target = haNum(raw);
            if (get("_peak_temperature") && !haMissing(raw)) m.peak = haNum(raw);
            if (get("_time_remaining") && !haMissing(raw)) m.doneAt = isoToEpoch(raw);
            if (get("_time_elapsed") && !haMissing(raw)) m.startedAt = isoToEpoch(raw);
            if (get("_cooking") && !haMissing(raw)) m.name = raw;
        }
        if (out.hostDown) return false;
    }
    m.fetchedAt = time(nullptr);
    out = m;
    static int lastKey = -1;                              // log every fetch during a cook, otherwise only changes
    int key = m.online * 2 + m.cooking;
    if (m.cooking || key != lastKey)
        log_i("[%s] meater: online=%d cooking=%d state=%s int=%.1f amb=%.1f target=%.1f eta=%lds", DTAG, m.online, m.cooking, m.state.c_str(),
              m.internal, m.ambient, m.target, m.doneAt ? (long)(m.doneAt - m.fetchedAt) : -1L);
    lastKey = key;
    return true;
}

// ---------------------------------------------------------------- weather codes (WMO)
const char *weatherText(int c) {
    if (c == 0) return "Klart";
    if (c == 1) return "Mest klart";
    if (c == 2) return "Delvist skyet";
    if (c == 3) return "Overskyet";
    if (c == 45 || c == 48) return "Tåge";
    if (c >= 51 && c <= 57) return "Finregn";
    if (c >= 61 && c <= 67) return "Regn";
    if (c >= 71 && c <= 77) return "Sne";
    if (c >= 80 && c <= 82) return "Byger";
    if (c == 85 || c == 86) return "Snebyger";
    if (c >= 95) return "Torden";
    return "-";
}
const char *weatherSymbol(int c) {           // glyphs available in the built-in font set
    if (c <= 1) return "\xE2\x98\x80";          // ☀ (U+2600) - if missing in font, shows as blank; text carries the meaning
    return "";
}

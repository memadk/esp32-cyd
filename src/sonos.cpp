#include "sonos.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

static const char *STAG = "sonos";

// ---------------------------------------------------------------- XML / URL helpers
static void appendUtf8(String &out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

String xmlUnescape(const String &s) { return xmlUnescapeRange(s, 0, s.length()); }

String xmlUnescapeRange(const String &s, int from, int to) {
    String out;
    if (from < 0) from = 0;
    if (to > (int)s.length()) to = s.length();
    if (to <= from) return out;
    out.reserve(to - from);
    const int n = to;
    for (int i = from; i < n; i++) {
        char c = s[i];
        if (c != '&') { out += c; continue; }
        int semi = s.indexOf(';', i);
        if (semi < 0 || semi - i > 10) { out += c; continue; }
        String ent = s.substring(i + 1, semi);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.length() > 1 && ent[0] == '#') {
            uint32_t cp = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                           : strtoul(ent.c_str() + 1, nullptr, 10);
            appendUtf8(out, cp);
        } else { out += c; continue; }
        i = semi;
    }
    return out;
}

String xmlEscape(const String &s) {
    String out;
    out.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

String xmlTag(const String &xml, const char *tag) {
    String open = String("<") + tag;
    int a = xml.indexOf(open);
    if (a < 0) return "";
    int gt = xml.indexOf('>', a);
    if (gt < 0) return "";
    if (xml[gt - 1] == '/') return "";
    String close = String("</") + tag + ">";
    int b = xml.indexOf(close, gt + 1);
    if (b < 0) return "";
    return xml.substring(gt + 1, b);
}

static bool xmlTagRange(const String &xml, const char *tag, int &from, int &to) {
    String open = String("<") + tag;
    int a = xml.indexOf(open);
    if (a < 0) return false;
    int gt = xml.indexOf('>', a);
    if (gt < 0 || xml[gt - 1] == '/') return false;
    String close = String("</") + tag + ">";
    int b = xml.indexOf(close, gt + 1);
    if (b < 0) return false;
    from = gt + 1; to = b;
    return true;
}

String xmlAttr(const String &element, const char *attr) {
    String key = String(" ") + attr + "=\"";
    int a = element.indexOf(key);
    if (a < 0) return "";
    a += key.length();
    int b = element.indexOf('"', a);
    if (b < 0) return "";
    return element.substring(a, b);
}

String urlEncode(const String &s) {
    static const char *hex = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static IPAddress ipFromLocation(const String &loc) {     // http://192.168.x.y:1400/xml/...
    IPAddress ip;
    int s = loc.indexOf("//"), e = loc.indexOf(':', s + 2);
    if (s >= 0 && e > s) ip.fromString(loc.substring(s + 2, e));
    return ip;
}

static int parseHms(const String &t) {          // "0:03:21" -> 201; NOT_IMPLEMENTED -> -1
    if (t.isEmpty() || t.indexOf(':') < 0) return -1;
    int h = 0, m = 0, sec = 0;
    if (sscanf(t.c_str(), "%d:%d:%d", &h, &m, &sec) != 3) return -1;
    return h * 3600 + m * 60 + sec;
}

String ZoneGroupInfo::label() const {
    String out;
    for (size_t i = 0; i < zoneNames.size(); i++) { if (i) out += " + "; out += zoneNames[i]; }
    return out;
}

// ---------------------------------------------------------------- SOAP
void SonosClient::ensureBuffers() {
    static bool done = false;
    if (done) return;
    done = true;
    _rx.reserve(12 * 1024);      // topology responses are ~9.5 KB; reserve() is a no-op when already large enough
    _tmp.reserve(10 * 1024);
}

bool SonosClient::soap(IPAddress ip, const char *path, const char *service, const char *action, const String &body, String &response) {
    if ((uint32_t)ip == 0) return false;
    String envelope;
    envelope.reserve(400 + body.length());
    envelope += "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
    envelope += action;
    envelope += " xmlns:u=\"urn:schemas-upnp-org:service:";
    envelope += service;
    envelope += ":1\">";
    envelope += body;
    envelope += "</u:";
    envelope += action;
    envelope += "></s:Body></s:Envelope>";

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(4000);
    http.setReuse(false);
    if (!http.begin(client, "http://" + ip.toString() + ":1400" + path)) return false;
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPACTION", String("\"urn:schemas-upnp-org:service:") + service + ":1#" + action + "\"");
    esp_task_wdt_reset();                              // a stalled link makes each call take ~10 s; several in a row must not trip the watchdog
    int code = http.POST(envelope);
    esp_task_wdt_reset();
    response = "";
    if (code > 0) {
        int size = http.getSize();
        if (!response.reserve(size > 0 ? size + 1 : 4096)) {
            log_e("[%s] no RAM for %d byte response (free %lu)", STAG, size, (unsigned long)ESP.getFreeHeap());
            http.end();
            return false;
        }
        WiFiClient *stream = http.getStreamPtr();
        uint8_t buf[256];
        int remaining = size;
        uint32_t last = millis(), deadline = millis() + 15000;   // idle 3 s / total 15 s: never hold the watchdog-supervised task
        while ((remaining != 0) && millis() - last < 3000) {
            if ((int32_t)(millis() - deadline) > 0) { log_w("[%s] %s: svar for langsomt, opgivet", STAG, action); code = -100; break; }
            size_t avail = stream->available();
            if (!avail) { if (!http.connected()) break; delay(2); continue; }
            int n = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
            if (n <= 0) break;
            response.concat(reinterpret_cast<const char *>(buf), n);
            if (remaining > 0) remaining -= n;
            last = millis();
            esp_task_wdt_reset();
        }
    }
    http.end();
    if (code != 200) {
        log_w("[%s] %s -> HTTP %d %s", STAG, action, code, xmlTag(response, "errorCode").c_str());
        return false;
    }
    return true;
}

bool SonosClient::avTransport(const char *action, const String &extra) {
    String &r = _rx;
    return soap("/MediaRenderer/AVTransport/Control", "AVTransport", action, "<InstanceID>0</InstanceID>" + extra, r);
}

bool SonosClient::play()     { return avTransport("Play", "<Speed>1</Speed>"); }
bool SonosClient::pause()    { return avTransport("Pause"); }
bool SonosClient::next()     { return avTransport("Next"); }
bool SonosClient::previous() { return avTransport("Previous"); }

bool SonosClient::setVolume(int volume) {
    String &r = _rx;
    return soap("/MediaRenderer/GroupRenderingControl/Control", "GroupRenderingControl", "SetGroupVolume",
                "<InstanceID>0</InstanceID><DesiredVolume>" + String(constrain(volume, 0, 100)) + "</DesiredVolume>", r);
}

bool SonosClient::setMute(bool mute) {
    String &r = _rx;
    return soap("/MediaRenderer/GroupRenderingControl/Control", "GroupRenderingControl", "SetGroupMute",
                String("<InstanceID>0</InstanceID><DesiredMute>") + (mute ? "1" : "0") + "</DesiredMute>", r);
}

bool SonosClient::refresh(SonosState &out) {
    String &r = _rx;
    if (!soap("/MediaRenderer/AVTransport/Control", "AVTransport", "GetTransportInfo", "<InstanceID>0</InstanceID>", r)) {
        out.online = false;
        return false;
    }
    out.online = true;
    String ts = xmlTag(r, "CurrentTransportState");
    if (ts == "PLAYING") out.transport = SonosTransport::Playing;
    else if (ts == "PAUSED_PLAYBACK") out.transport = SonosTransport::Paused;
    else if (ts == "STOPPED") out.transport = SonosTransport::Stopped;
    else if (ts == "TRANSITIONING") out.transport = SonosTransport::Transitioning;
    else out.transport = SonosTransport::Unknown;

    if (soap("/MediaRenderer/GroupRenderingControl/Control", "GroupRenderingControl", "GetGroupVolume", "<InstanceID>0</InstanceID>", r))
        out.volume = xmlTag(r, "CurrentVolume").toInt();
    if (soap("/MediaRenderer/GroupRenderingControl/Control", "GroupRenderingControl", "GetGroupMute", "<InstanceID>0</InstanceID>", r))
        out.muted = xmlTag(r, "CurrentMute") == "1";
    out.groupZones = _groupZones;

    SonosTrack t;
    if (soap("/MediaRenderer/AVTransport/Control", "AVTransport", "GetPositionInfo", "<InstanceID>0</InstanceID>", r)) {
        t.trackUri     = xmlUnescape(xmlTag(r, "TrackURI"));
        t.trackNo      = xmlTag(r, "Track").toInt();
        t.durationSec  = parseHms(xmlTag(r, "TrackDuration"));
        t.positionSec  = parseHms(xmlTag(r, "RelTime"));
        out.positionAtMs = millis();
        if (t.durationSec <= 0) { t.durationSec = -1; t.positionSec = -1; }
        String meta = xmlUnescape(xmlTag(r, "TrackMetaData"));
        if (meta.length() > 20) {
            t.title         = xmlUnescape(xmlTag(meta, "dc:title"));
            t.artist        = xmlUnescape(xmlTag(meta, "dc:creator"));
            t.album         = xmlUnescape(xmlTag(meta, "upnp:album"));
            t.artUri        = xmlUnescape(xmlTag(meta, "upnp:albumArtURI"));
            t.streamContent = xmlUnescape(xmlTag(meta, "r:streamContent"));
            // Sonos uses ZPSTR_* placeholders in stream metadata while a radio stream starts; the app localises them
            if (t.streamContent == "ZPSTR_CONNECTING")     t.streamContent = "Forbinder...";
            else if (t.streamContent == "ZPSTR_BUFFERING") t.streamContent = "Indlæser...";
            else if (t.streamContent.startsWith("ZPSTR_")) t.streamContent = "Indlæser...";
            else if (t.streamContent.indexOf("x-sonosapi") >= 0 || t.streamContent.indexOf("://") >= 0) t.streamContent = "";
        }
    }
    if (soap("/MediaRenderer/AVTransport/Control", "AVTransport", "GetMediaInfo", "<InstanceID>0</InstanceID>", r)) {
        String currentUri = xmlUnescape(xmlTag(r, "CurrentURI"));
        t.fromQueue   = currentUri.startsWith("x-rincon-queue:");
        t.queueLength = xmlTag(r, "NrTracks").toInt();
        if (currentUri.startsWith("x-sonosapi-stream:") || currentUri.startsWith("x-rincon-mp3radio:") || t.trackUri.isEmpty())
            t.trackUri = currentUri;
        String meta = xmlUnescape(xmlTag(r, "CurrentURIMetaData"));
        if (meta.length() > 20) {
            t.station = xmlUnescape(xmlTag(meta, "dc:title"));
            if (t.artUri.isEmpty()) t.artUri = xmlUnescape(xmlTag(meta, "upnp:albumArtURI"));
        }
    }
    out.track = t;
    return true;
}

String SonosClient::artUrl(const SonosTrack &track) const {
    const String &uri = track.artUri;
    if (uri.isEmpty()) return "";
    if (uri.startsWith("http://")) return uri;
    if (uri.startsWith("https://")) {
        if (track.trackUri.isEmpty()) return "";
        return "http://" + _ip.toString() + ":1400/getaa?s=1&u=" + urlEncode(track.trackUri);
    }
    return "http://" + _ip.toString() + ":1400" + uri;
}

// ---------------------------------------------------------------- Favourites
bool SonosClient::browseFavorites(std::vector<SonosFavorite> &out) {
    String &r = _rx;
    if (!soap("/MediaServer/ContentDirectory/Control", "ContentDirectory", "Browse",
              "<ObjectID>FV:2</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>dc:title,r:description,res</Filter>"
              "<StartingIndex>0</StartingIndex><RequestedCount>40</RequestedCount><SortCriteria></SortCriteria>", r))
        return false;
    int from, to;
    if (!xmlTagRange(r, "Result", from, to)) return false;
    _tmp = xmlUnescapeRange(r, from, to);
    const String &didl = _tmp;
    out.clear();
    int pos = 0;
    while (true) {
        int a = didl.indexOf("<item ", pos);
        if (a < 0) break;
        int b = didl.indexOf("</item>", a);
        if (b < 0) break;
        String item = didl.substring(a, b + 7);
        pos = b + 7;
        SonosFavorite f;
        f.title       = xmlUnescape(xmlTag(item, "dc:title"));
        f.description = xmlUnescape(xmlTag(item, "r:description"));
        f.hasUri      = xmlTag(item, "res").length() > 0;
        out.push_back(f);
    }
    log_i("[%s] %d favourites", STAG, out.size());
    return true;
}

bool SonosClient::playFavoriteAt(int index) {
    String &r = _rx;
    if (index < 0 || !soap("/MediaServer/ContentDirectory/Control", "ContentDirectory", "Browse",
              "<ObjectID>FV:2</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>"
              "<StartingIndex>" + String(index) + "</StartingIndex><RequestedCount>1</RequestedCount><SortCriteria></SortCriteria>", r))
        return false;
    int from, to;
    if (!xmlTagRange(r, "Result", from, to)) return false;
    _tmp = xmlUnescapeRange(r, from, to);
    String uri  = xmlUnescape(xmlTag(_tmp, "res"));
    String meta = xmlUnescape(xmlTag(_tmp, "r:resMD"));
    _tmp = "";
    if (uri.isEmpty()) return false;
    if (!avTransport("SetAVTransportURI",
                     "<CurrentURI>" + xmlEscape(uri) + "</CurrentURI><CurrentURIMetaData>" + xmlEscape(meta) + "</CurrentURIMetaData>"))
        return false;
    return play();
}

// ---------------------------------------------------------------- Queue
bool SonosClient::browseQueue(int start, int count, std::vector<QueueItem> &out, int &total) {
    String &r = _rx;
    if (!soap("/MediaServer/ContentDirectory/Control", "ContentDirectory", "Browse",
              "<ObjectID>Q:0</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>dc:title,dc:creator</Filter>"
              "<StartingIndex>" + String(start) + "</StartingIndex><RequestedCount>" + String(count) + "</RequestedCount><SortCriteria></SortCriteria>", r))
        return false;
    total = xmlTag(r, "TotalMatches").toInt();
    String didl = xmlUnescape(xmlTag(r, "Result"));
    r = String();
    out.clear();
    int pos = 0, n = start;
    while (true) {
        int a = didl.indexOf("<item ", pos);
        if (a < 0) break;
        int b = didl.indexOf("</item>", a);
        if (b < 0) break;
        String item = didl.substring(a, b + 7);
        pos = b + 7;
        QueueItem q;
        q.number = ++n;
        q.title  = xmlUnescape(xmlTag(item, "dc:title"));
        q.artist = xmlUnescape(xmlTag(item, "dc:creator"));
        out.push_back(q);
    }
    log_i("[%s] queue: %d of %d items from %d", STAG, out.size(), total, start);
    return true;
}

bool SonosClient::playQueueTrack(int number) {
    String &r = _rx;
    bool onQueue = false;
    if (soap("/MediaRenderer/AVTransport/Control", "AVTransport", "GetMediaInfo", "<InstanceID>0</InstanceID>", r))
        onQueue = xmlUnescape(xmlTag(r, "CurrentURI")).startsWith("x-rincon-queue:");
    if (!onQueue && !avTransport("SetAVTransportURI",
                                 "<CurrentURI>x-rincon-queue:" + _coordinatorUuid + "#0</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"))
        return false;
    if (!avTransport("Seek", "<Unit>TRACK_NR</Unit><Target>" + String(number) + "</Target>")) return false;
    return play();
}

// ---------------------------------------------------------------- Grouping
bool SonosClient::joinGroup(const String &coordinatorUuid) {
    String &r = _rx;
    return soap(_roomPlayerIp, "/MediaRenderer/AVTransport/Control", "AVTransport", "SetAVTransportURI",
                "<InstanceID>0</InstanceID><CurrentURI>x-rincon:" + coordinatorUuid + "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>", r);
}

bool SonosClient::leaveGroup() {
    String &r = _rx;
    return soap(_roomPlayerIp, "/MediaRenderer/AVTransport/Control", "AVTransport", "BecomeCoordinatorOfStandaloneGroup",
                "<InstanceID>0</InstanceID>", r);
}

// ---------------------------------------------------------------- Discovery / topology
bool SonosClient::discover(const String &room, uint32_t timeoutMs) {
    ensureBuffers();
    static const char MSEARCH[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    WiFiUDP udp;
    if (!udp.begin(0)) return false;
    for (int i = 0; i < 2; i++) {
        udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
        udp.write((const uint8_t *)MSEARCH, sizeof(MSEARCH) - 1);
        udp.endPacket();
        delay(100);
    }
    // Collect every responder: an S1 and an S2 household on the same LAN each answer with their own topology,
    // so the first device to reply may not know our room at all.
    IPAddress found[8];
    int nFound = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs && nFound < 8) {
        int len = udp.parsePacket();
        if (len > 0) {
            char buf[1024];
            int n = udp.read(buf, sizeof(buf) - 1);
            buf[n > 0 ? n : 0] = 0;
            if (strstr(buf, "ZonePlayer")) {
                IPAddress ip = udp.remoteIP();
                bool dup = false;
                for (int i = 0; i < nFound; i++) if (found[i] == ip) dup = true;
                if (!dup) found[nFound++] = ip;
            }
        }
        if (nFound && millis() - t0 > 1500) break;       // enough responders collected
        delay(10);
    }
    udp.stop();
    if (nFound == 0) { log_w("[%s] no Sonos device answered SSDP", STAG); return false; }
    _room = room;
    std::vector<ZoneGroupInfo> groups;
    for (int i = 0; i < nFound; i++) {
        log_i("[%s] trying topology from %s", STAG, found[i].toString().c_str());
        if (fetchTopology(found[i], groups) && applyTopology(groups)) { _anyIp = found[i]; return true; }
    }
    return false;
}

bool SonosClient::setRoom(const String &room) {
    // Only commit the new room if the topology actually resolves it; otherwise keep the old one.
    String previous = _room;
    _room = room;
    if (refreshTopology()) return true;
    _room = previous;
    return false;
}

bool SonosClient::refreshTopology() {
    if (_room.isEmpty()) return false;
    std::vector<ZoneGroupInfo> groups;
    IPAddress src = (uint32_t)_roomPlayerIp ? _roomPlayerIp : ((uint32_t)_ip ? _ip : _anyIp);
    return fetchTopology(src, groups) && applyTopology(groups);
}

bool SonosClient::getGroups(std::vector<ZoneGroupInfo> &out) {
    IPAddress src = (uint32_t)_roomPlayerIp ? _roomPlayerIp : ((uint32_t)_ip ? _ip : _anyIp);
    return fetchTopology(src, out);
}

bool SonosClient::fetchTopology(IPAddress anyDevice, std::vector<ZoneGroupInfo> &groups) {
    String &r = _rx;
    if (!soap(anyDevice, "/ZoneGroupTopology/Control", "ZoneGroupTopology", "GetZoneGroupState", "", r)) return false;
    int from, to;
    if (!xmlTagRange(r, "ZoneGroupState", from, to)) return false;
    _tmp = xmlUnescapeRange(r, from, to);          // one copy instead of two
    const String &state = _tmp;
    groups.clear();
    int pos = 0;
    while (true) {
        int g = state.indexOf("<ZoneGroup ", pos);
        if (g < 0) break;
        int ge = state.indexOf("</ZoneGroup>", g);
        if (ge < 0) break;
        String group = state.substring(g, ge);
        pos = ge + 12;

        ZoneGroupInfo info;
        info.coordinatorUuid = xmlAttr(group.substring(0, group.indexOf('>')), "Coordinator");
        int m = 0;
        while (true) {
            int a = group.indexOf("<ZoneGroupMember ", m);
            if (a < 0) break;
            int b = group.indexOf('>', a);
            String member = group.substring(a, b);
            m = b + 1;
            ZoneMember zm;
            zm.uuid = xmlAttr(member, "UUID");
            zm.name = xmlUnescape(xmlAttr(member, "ZoneName"));
            zm.ip = ipFromLocation(xmlUnescape(xmlAttr(member, "Location")));
            zm.invisible = member.indexOf("Invisible=\"1\"") >= 0;
            if (zm.uuid == info.coordinatorUuid) info.coordinatorIp = zm.ip;
            if (zm.name == _room) info.containsRoom = true;
            info.members.push_back(zm);
        }
        // visible zone names, coordinator's room first
        for (const auto &zm : info.members) if (zm.uuid == info.coordinatorUuid && !zm.invisible) info.zoneNames.push_back(zm.name);
        for (const auto &zm : info.members) {
            if (zm.invisible) continue;
            bool dup = false;
            for (const auto &n : info.zoneNames) if (n == zm.name) dup = true;
            if (!dup) info.zoneNames.push_back(zm.name);
        }
        if (info.zoneNames.empty()) continue;      // e.g. a group of only invisible devices
        groups.push_back(info);
    }
    return !groups.empty();
}

bool SonosClient::applyTopology(const std::vector<ZoneGroupInfo> &groups) {
    for (const auto &g : groups) {
        if (!g.containsRoom) continue;
        IPAddress player;
        String uuid;
        for (const auto &zm : g.members) if (zm.name == _room && !zm.invisible) { player = zm.ip; uuid = zm.uuid; }
        if ((uint32_t)player == 0) for (const auto &zm : g.members) if (zm.name == _room) { player = zm.ip; uuid = zm.uuid; }
        _ip = g.coordinatorIp;
        _coordinatorUuid = g.coordinatorUuid;
        _roomPlayerIp = player;
        _roomUuid = uuid;
        _groupZones = g.zoneNames.size();
        log_i("[%s] room '%s' player %s, coordinator %s, %d zone(s) in group", STAG, _room.c_str(),
              _roomPlayerIp.toString().c_str(), _ip.toString().c_str(), _groupZones);
        return true;
    }
    log_w("[%s] room '%s' not found in zone topology", STAG, _room.c_str());
    return false;
}

// ---------------------------------------------------------------- GENA events
bool SonosClient::gena(const char *method, IPAddress ip, const char *eventPath, const String &callbackUrl,
                       String &sid, uint32_t &timeoutSec) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(4000);
    http.setReuse(false);
    if (!http.begin(client, "http://" + ip.toString() + ":1400" + eventPath)) return false;
    if (callbackUrl.length()) {                       // new subscription
        http.addHeader("CALLBACK", "<" + callbackUrl + ">");
        http.addHeader("NT", "upnp:event");
        http.addHeader("TIMEOUT", "Second-600");
    } else {
        http.addHeader("SID", sid);
        if (strcmp(method, "SUBSCRIBE") == 0) http.addHeader("TIMEOUT", "Second-600");
    }
    const char *hdrs[] = {"SID", "TIMEOUT"};
    http.collectHeaders(hdrs, 2);
    esp_task_wdt_reset();
    int code = http.sendRequest(method);
    esp_task_wdt_reset();
    if (code == 200) {
        if (callbackUrl.length()) sid = http.header("SID");
        String t = http.header("TIMEOUT");           // "Second-600"
        int dash = t.indexOf('-');
        timeoutSec = dash >= 0 ? (uint32_t)t.substring(dash + 1).toInt() : 300;
        if (timeoutSec == 0) timeoutSec = 300;
    } else {
        log_w("[%s] %s %s -> %d", STAG, method, eventPath, code);
    }
    http.end();
    return code == 200;
}

bool SonosClient::subscribe(IPAddress ip, const char *eventPath, const String &callbackUrl, String &sid, uint32_t &timeoutSec) {
    return gena("SUBSCRIBE", ip, eventPath, callbackUrl, sid, timeoutSec);
}
bool SonosClient::renewSubscription(IPAddress ip, const char *eventPath, const String &sid, uint32_t &timeoutSec) {
    String s = sid;
    return gena("SUBSCRIBE", ip, eventPath, "", s, timeoutSec);
}
bool SonosClient::unsubscribe(IPAddress ip, const char *eventPath, const String &sid) {
    String s = sid; uint32_t t;
    return gena("UNSUBSCRIBE", ip, eventPath, "", s, t);
}

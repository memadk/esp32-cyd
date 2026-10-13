// Minimal Sonos local-control client (UPnP/SOAP over plain HTTP on port 1400) + GENA event subscriptions.
#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

enum class SonosTransport : uint8_t { Unknown, Playing, Paused, Stopped, Transitioning };

struct SonosTrack {
    String title, artist, album, artUri, streamContent, station, trackUri;
    int durationSec = -1;     // -1 = unknown / stream
    int positionSec = -1;
    int trackNo = 0;          // 1-based position in the queue, 0 = n/a
    int queueLength = 0;
    bool fromQueue = false;   // playing from the Sonos queue (x-rincon-queue)
};

struct SonosState {
    bool online = false;
    SonosTransport transport = SonosTransport::Unknown;
    int volume = -1;          // group volume (== zone volume when the room is alone)
    bool muted = false;
    int groupZones = 1;       // visible zones in the group the room belongs to
    SonosTrack track;
    uint32_t positionAtMs = 0;   // millis() when positionSec was read
};

struct QueueItem {
    int number = 0;           // 1-based
    String title, artist;
};

struct SonosFavorite {
    String title, description;
    bool hasUri = false;                 // the (large) uri/metadata are fetched again when the item is played
    bool playable() const { return hasUri; }
};

struct ZoneMember {
    String uuid, name;
    IPAddress ip;
    bool invisible = false;   // stereo-pair partner, sub, surround
};

struct ZoneGroupInfo {
    String coordinatorUuid;
    IPAddress coordinatorIp;
    std::vector<ZoneMember> members;
    std::vector<String> zoneNames;    // visible, de-duplicated, coordinator's room first
    bool containsRoom = false;        // the controlled room is in this group
    String label() const;             // "Stue + Kontor"
};

class SonosClient {
public:
    // SSDP discovery, then resolve the group coordinator for `room` from the zone topology.
    bool discover(const String &room, uint32_t timeoutMs = 4000);
    bool setRoom(const String &room);                 // switch controlled room (re-resolves topology)
    const String &room() const { return _room; }
    bool refreshTopology();                           // follows the room into/out of groups
    bool getGroups(std::vector<ZoneGroupInfo> &out);  // fresh topology for the room picker

    IPAddress coordinator() const { return _ip; }
    IPAddress roomPlayer() const { return _roomPlayerIp; }   // the room's own visible player
    bool hasCoordinator() const { return (uint32_t)_ip != 0; }
    void setCoordinator(IPAddress ip) { _ip = ip; }   // cached-IP fallback when discovery fails
    int groupZones() const { return _groupZones; }

    bool refresh(SonosState &out);
    bool play();
    bool pause();
    bool next();
    bool previous();
    bool setVolume(int volume);    // group volume
    bool setMute(bool mute);       // group mute
    bool browseFavorites(std::vector<SonosFavorite> &out);
    bool playFavoriteAt(int index);      // index into the browseFavorites() list
    bool joinGroup(const String &coordinatorUuid);   // room joins that group
    bool leaveGroup();                               // room becomes standalone
    bool browseQueue(int start, int count, std::vector<QueueItem> &out, int &total);   // start is 0-based
    bool playQueueTrack(int number);                 // 1-based; switches the transport to the queue if needed

    String artUrl(const SonosTrack &track) const;

    // GENA events. eventPath e.g. "/MediaRenderer/AVTransport/Event"
    bool subscribe(IPAddress ip, const char *eventPath, const String &callbackUrl, String &sid, uint32_t &timeoutSec);
    bool renewSubscription(IPAddress ip, const char *eventPath, const String &sid, uint32_t &timeoutSec);
    bool unsubscribe(IPAddress ip, const char *eventPath, const String &sid);

private:
    bool soap(IPAddress ip, const char *path, const char *service, const char *action, const String &body, String &response);
    bool soap(const char *path, const char *service, const char *action, const String &body, String &response) {
        return soap(_ip, path, service, action, body, response);
    }
    bool avTransport(const char *action, const String &extra = "");
    bool fetchTopology(IPAddress anyDevice, std::vector<ZoneGroupInfo> &groups);
    bool applyTopology(const std::vector<ZoneGroupInfo> &groups);
    bool gena(const char *method, IPAddress ip, const char *eventPath, const String &callbackUrl,
              String &sid, uint32_t &timeoutSec);

    IPAddress _ip;             // group coordinator of the controlled room
    IPAddress _roomPlayerIp;   // the room's own player
    IPAddress _anyIp;          // any Sonos device, for topology queries
    String _room, _roomUuid, _coordinatorUuid;
    int _groupZones = 1;
    String _rx;        // persistent SOAP receive buffer (reserved once, avoids heap fragmentation)
    String _tmp;       // persistent scratch for unescaped payloads
    void ensureBuffers();
public:
    void releaseBuffers() { _rx = String(); _tmp = String(); }   // before OTA: give the updater room
};

// XML / URL helpers
String xmlUnescape(const String &s);
String xmlEscape(const String &s);
String xmlTag(const String &xml, const char *tag);
String xmlAttr(const String &element, const char *attr);
String urlEncode(const String &s);
String xmlUnescapeRange(const String &s, int from, int to);   // unescape s[from, to) without a substring copy

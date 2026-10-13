// Info board settings, editable on the web page, stored in NVS ("cfg").
#pragma once
#include <Arduino.h>

struct InfoConfig {
    // screen (same meaning as on the Sonos remote)
    int blBright = 230, blNormal = 140, blDark = 70, blIdle = 40;
    int idleMin = 5;            // dim after N min without touch (0 = never)
    int nightStart = 23, nightEnd = 6;   // screen off between these hours (same value = disabled)
    bool darkOff = true;        // screen off when the room has been dark for darkMin minutes
    int darkMin = 3;
    int wakeSec = 60;           // how long a touch wakes the screen at night
    int rotation = 1;           // 0 portrait USB up, 1 landscape USB right, 2 portrait USB down, 3 landscape USB left
    // prices
    String priceArea = "DK1";
    int surchargeOere = 0;      // øre/kWh added to the spot price (nettarif + elafgift), ex VAT
    bool vat = true;            // show prices incl. 25 % moms
    // weather
    float lat = 56.19f, lon = 10.10f;
    String place = "Tilst";
    // departures
    String rpKey;               // Rejseplanen API key
    String stopQuery = "Skjoldhøjskolen";
    String line = "3A";
    String direction = "Aarhus Ø";
    // Home Assistant ("Hjem" page): REST API with a long-lived access token the user creates in HA (profile -> security)
    String haUrl = "http://192.168.68.96:8123";
    String haToken;
    String haEntities;          // one per line: entity_id|Label  (label optional -> friendly_name)
    String meater = "sensor.kokken_meater_pro";   // "Stegning" page: Meater entity prefix (or any full Meater entity id), empty = off
    bool meaterAlert = true;    // blink the red LED and wake the screen when the meat is ready to come out
    // remote log / ota
    String otaPassword;
    String logHost;         // UDP syslog receiver; normally empty = found via the SONOSLOG beacon. Name/IP only as fallback.
    int logPort = 5514;
};
extern InfoConfig cfg;
extern volatile bool cfgRotationDirty;

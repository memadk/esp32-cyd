// User-adjustable settings, editable on the board's web page (http://sonos-koekken.local/), stored in NVS.
#pragma once
#include <Arduino.h>

struct AppConfig {
    int blBright = 230;     // backlight in a bright room (LDR < 1200)
    int blNormal = 140;     // normal room light
    int blDark   = 70;      // dark room (LDR > 3000)
    int blIdle   = 25;      // after idleMin minutes without touch
    int idleMin  = 2;       // 0 = never dim
    int sleepMin = 5;       // screen off when idle and nothing plays; 0 = never
    int volStep  = 2;       // volume change per +/- tap
    int rotation = 0;       // 0 = USB at top, 2 = USB at bottom (stand)
    String otaPassword;     // empty = use compile-time OTA_PASSWORD
    String logHost;         // UDP syslog receiver; normally empty = found via the SONOSLOG beacon. Name/IP only as fallback.
    int logPort = 5514;
    // Touch calibration (screen = raw * a / 1000 + b per axis), from the on-screen calibration; only applied while
    // the board is in the rotation it was calibrated in (tcRot = -1: not calibrated).
    int tcAx = 1000, tcBx = 0, tcAy = 1000, tcBy = 0, tcRot = -1;
};

extern AppConfig cfg;            // read from both cores; written only by the web handler (Sonos task)
extern volatile bool cfgRotationDirty;
extern volatile bool touchCalRequest;   // set by the web page, run by loop()
void config_load();
void config_save();

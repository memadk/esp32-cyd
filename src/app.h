// Shared definitions between the UI (core 1, Arduino loop) and the Sonos task (core 0)
#pragma once
#include <Arduino.h>

enum class AppCmd : uint8_t {
    Play, Pause, Next, Prev, SetVolume, ToggleMute,
    PlayFavorite, RefreshFavorites,
    RefreshGroups, JoinGroup, LeaveGroup, ControlRoom,   // value = index into the last group list
    SwipeRoom,                                           // like ControlRoom but not persisted (page swipe)
    SetHomeRoom,                                         // room name in Shared::pendingRoom (from the web page)
    RefreshQueue, PlayQueueTrack,                        // value = 1-based track number
    WifiSetup,                                           // reboot into the captive-portal setup
    Rediscover
};

// Called from UI callbacks; queues a command for the Sonos task.
void app_send(AppCmd cmd, int value = 0);

static constexpr int ART_SIZE = 120;   // album art square, pixels

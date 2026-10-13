// LVGL user interface. All functions must be called from the UI thread (Arduino loop).
#pragma once
#include <lvgl.h>
#include <vector>
#include "sonos.h"

void ui_init(const char *roomName, const uint16_t *artBuffer);   // artBuffer: ART_SIZE^2 RGB565, owned by caller
void ui_set_status(const char *message);                          // boot / error / setup screen text
void ui_show_now_playing();
void ui_update_state(const SonosState &state);
void ui_update_favorites(const std::vector<SonosFavorite> &favs);
void ui_update_groups(const std::vector<ZoneGroupInfo> &groups);
void ui_update_queue(const std::vector<QueueItem> &items, int total, int currentNo);
void ui_set_art(bool valid);
void ui_set_wifi_ok(bool ok);
void ui_set_room(const char *room);
void ui_apply_rotation();                                          // after cfg.rotation changed

#pragma once
#include <lvgl.h>
#include "data.h"
#include "info_config.h"

void ui_init(int width, int height);
void ui_set_status(const char *message);
void ui_show_pages();
void ui_update_prices(const PriceData &p);
void ui_update_weather(const WeatherData &w);
void ui_update_departures(const DepartureData &d);
void ui_update_home(const HaData &h);
bool ui_update_meater(const MeaterData &m);   // true = the meat just became ready to take out (caller wakes the screen)
void ui_set_wifi_ok(bool ok);
void ui_tick();                          // once per second: clock, countdowns, current-slot highlight
void ui_apply_rotation();

// Data sources for the info board: day-ahead electricity prices (Energi Data Service), weather (Open-Meteo),
// bus departures (Rejseplanen: official HAFAS ReST API with a key, or the legacy key-less station board over plain
// HTTP). Fetched on the network task.
#pragma once
#include <Arduino.h>
#include <vector>
#include <time.h>

struct PriceSlot { time_t t; int16_t oere; };        // start of a 15-min slot (local time), spot price in øre/kWh (ex. surcharge/VAT)
struct PriceData {
    std::vector<PriceSlot> slots;                    // today + tomorrow when published (~13:00)
    time_t fetchedAt = 0;
    bool ok = false;
    String error;
};

struct WeatherDay { time_t date; int code; float tmax, tmin, rain; };
struct WeatherData {
    bool ok = false; time_t fetchedAt = 0; String error;
    float temp = 0, wind = 0, precip = 0; int code = 0;   // current
    time_t hourlyStart = 0; int8_t hourlyTemp[24] = {0}; uint8_t hourlyPop[24] = {0}; int hourlyCount = 0;
    WeatherDay days[4]; int dayCount = 0;
};

struct Departure { time_t planned; time_t realtime; bool hasRealtime; bool cancelled; String line, direction; };
struct DepartureData {
    std::vector<Departure> list;
    time_t fetchedAt = 0;
    bool ok = false;
    String error;                                    // e.g. "API-nøgle mangler"
    String stopName;
};

struct HaItem { String id, label, state, unit; bool ok; };
struct HaData {
    std::vector<HaItem> items;
    time_t fetchedAt = 0;
    bool ok = false;
    bool hostDown = false;                           // HA itself unreachable (refused/timeout): caller backs off
    String error;
};

// Meater probe via Home Assistant's Meater integration (cloud polling, read-only). All entity ids share one prefix,
// e.g. sensor.kokken_meater_pro + _internal_temperature / _cook_state / _time_remaining ...
struct MeaterData {
    bool ok = false;                                 // HA answered
    bool online = false;                             // the probe reports a temperature (app/Block connected to Meater Cloud)
    bool cooking = false;                            // a cook is set up in the app (cook sensors available)
    bool hostDown = false;
    float internal = NAN, ambient = NAN, target = NAN, peak = NAN;
    String state;                                    // HA enum: configured, started, ready_for_resting, resting, finished, ...
    String name;                                     // cook name from the app, e.g. "Oksemørbrad"
    time_t doneAt = 0;                               // absolute ETA from the time_remaining sensor (0 = Meater still estimating)
    time_t startedAt = 0;
    time_t fetchedAt = 0;
    String error;
};

bool fetchPrices(PriceData &out, const char *priceArea);
bool fetchWeather(WeatherData &out, float lat, float lon);
bool resolveStop(const String &apiKey, const String &stopQuery, String &stopIdOut, String &stopNameOut, String &err);
bool fetchDepartures(DepartureData &out, const String &apiKey, const String &stopId, const String &line, const String &directionContains);
bool resolveStopLegacy(const String &stopQuery, String &stopIdOut, String &stopNameOut, String &err);           // no key needed
bool fetchDeparturesLegacy(DepartureData &out, const String &stopId, const String &line, const String &directionContains);

bool fetchHomeAssistant(HaData &out, const String &baseUrl, const String &token, const String &entitiesSpec);
bool fetchMeater(MeaterData &out, const String &baseUrl, const String &token, const String &prefix);
String meaterPrefix(const String &configured);       // accepts a prefix or any full Meater entity id

const char *weatherText(int code);                   // Danish description
const char *weatherSymbol(int code);                 // LVGL symbol / short glyph

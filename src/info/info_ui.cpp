#include "info_ui.h"
#include <time.h>
#include <vector>
#include <math.h>

LV_FONT_DECLARE(font_num_44)

namespace {
const lv_color_t COL_BG     = lv_color_hex(0x101820);
const lv_color_t COL_TEXT   = lv_color_hex(0xF2F4F7);
const lv_color_t COL_MUTED  = lv_color_hex(0x8A94A6);
const lv_color_t COL_ACCENT = lv_color_hex(0x3B82F6);
const lv_color_t COL_CARD   = lv_color_hex(0x1B2634);
const lv_color_t COL_WARN   = lv_color_hex(0xE0533A);
const lv_color_t COL_GREEN  = lv_color_hex(0x34C759);
const lv_color_t COL_YELLOW = lv_color_hex(0xF5C542);

constexpr int PAGES = 5;
constexpr int PAGE_MEATER = 4;
int W = 240, H = 320;            // display size
bool L = false;                  // landscape
int BODY_H = 284;
lv_obj_t *scr_status, *lbl_status;
lv_obj_t *scr, *body, *dots, *lbl_clock, *lbl_wifi, *lbl_title;
lv_obj_t *page[PAGES];
int active = 0;
lv_anim_t slide_anim;

// prices page
lv_obj_t *lbl_price_now, *lbl_price_sub, *lbl_price_minmax, *chart_price, *lbl_price_x0, *lbl_price_x1, *lbl_price_x2, *lbl_price_err;
lv_chart_series_t *ser_price;
int32_t price_lo = 0, price_hi = 1;      // current chart range, for bar colouring
PriceData prices;
// weather page
lv_obj_t *lbl_w_temp, *lbl_w_desc, *lbl_w_wind, *lbl_w_err, *chart_temp, *lbl_w_days[4], *lbl_w_days_t[4], *lbl_w_days_c[4];
lv_chart_series_t *ser_temp;
WeatherData weather;
// departures page
lv_obj_t *lbl_d_head, *list_dep, *lbl_d_err;
DepartureData deps;
// home page (Home Assistant)
lv_obj_t *list_ha, *lbl_ha_err;
HaData ha;
// cooking page (Meater)
lv_obj_t *lbl_m_name, *lbl_m_state, *lbl_m_temp, *lbl_m_target, *lbl_m_left, *lbl_m_left_sub, *bar_m, *lbl_m_amb, *lbl_m_peak, *lbl_m_err;
MeaterData meater;

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_align_t align, int x, int y, int width = 0) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    if (width) { lv_obj_set_width(l, width); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
    lv_obj_align(l, align, x, y);
    lv_label_set_text(l, "");
    return l;
}

void render_dots() {
    lv_obj_clean(dots);
    for (int i = 0; i < PAGES; i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_set_size(d, i == active ? 14 : 5, 5);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, i == active ? COL_ACCENT : COL_MUTED, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
}
void slide_exec(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
void show_page(int idx, int from_x) {
    for (int i = 0; i < PAGES; i++) { if (i == idx) lv_obj_remove_flag(page[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(page[i], LV_OBJ_FLAG_HIDDEN); }
    active = idx;
    render_dots();
    static const char *titles[PAGES] = {"Elpris", "Vejr", "Bus", "Hjem", "Stegning"};
    lv_label_set_text(lbl_title, titles[idx]);
    if (from_x) {
        lv_anim_init(&slide_anim);
        lv_anim_set_var(&slide_anim, body);
        lv_anim_set_exec_cb(&slide_anim, slide_exec);
        lv_anim_set_values(&slide_anim, from_x, 0);
        lv_anim_set_duration(&slide_anim, 180);
        lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
        lv_anim_start(&slide_anim);
    }
}
void on_swipe(lv_event_t *) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) show_page((active + 1) % PAGES, W);
    else if (dir == LV_DIR_RIGHT) show_page((active + PAGES - 1) % PAGES, -W);
}
void on_tap_next(lv_event_t *) { show_page((active + 1) % PAGES, W); }

lv_obj_t *make_page() {
    lv_obj_t *p = lv_obj_create(body);
    lv_obj_set_size(p, W, BODY_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

lv_obj_t *make_chart(lv_obj_t *parent, int x, int y, int w, int h, lv_chart_type_t type, int points, lv_chart_series_t **ser) {
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, x, y);
    lv_chart_set_type(c, type);
    lv_chart_set_point_count(c, points);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_line_width(c, 0, LV_PART_MAIN);          // no grid
    lv_chart_set_div_line_count(c, 0, 0);
    if (type == LV_CHART_TYPE_BAR) { lv_obj_set_style_pad_column(c, 2, LV_PART_MAIN); lv_obj_set_style_radius(c, 2, LV_PART_ITEMS); }
    else { lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR); lv_obj_set_style_line_width(c, 3, LV_PART_ITEMS); }
    *ser = lv_chart_add_series(c, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    return c;
}

// ---- pages
void build_prices(lv_obj_t *p) {
    if (L) {   // 320x204: text column left, chart right
        lbl_price_now = make_label(p, &font_dk_20, COL_TEXT, LV_ALIGN_TOP_LEFT, 12, 8);
        lbl_price_sub = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 12, 38, 110);
        lv_label_set_long_mode(lbl_price_sub, LV_LABEL_LONG_WRAP);
        lbl_price_minmax = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 12, 110, 110);
        lv_label_set_long_mode(lbl_price_minmax, LV_LABEL_LONG_WRAP);
        chart_price = make_chart(p, 126, 6, 186, 156, LV_CHART_TYPE_BAR, 24, &ser_price);
        lbl_price_x0 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 132, 166);
        lbl_price_x1 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 213, 166);
        lbl_price_x2 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_RIGHT, -12, 166);
        lbl_price_err = make_label(p, &font_dk_12, COL_WARN, LV_ALIGN_BOTTOM_MID, 0, -2, 300);
    } else {
        lbl_price_now = make_label(p, &font_dk_20, COL_TEXT, LV_ALIGN_TOP_MID, 0, 6);
        lbl_price_sub = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_MID, 0, 34);
        chart_price = make_chart(p, 12, 56, 216, 120, LV_CHART_TYPE_BAR, 24, &ser_price);
        lbl_price_x0 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 14, 178);
        lbl_price_x1 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_MID, 0, 178);
        lbl_price_x2 = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_RIGHT, -14, 178);
        lbl_price_minmax = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_MID, 0, 198, 224);
        lv_obj_set_style_text_align(lbl_price_minmax, LV_TEXT_ALIGN_CENTER, 0);
        lbl_price_err = make_label(p, &font_dk_12, COL_WARN, LV_ALIGN_TOP_MID, 0, 218, 224);
    }
    lv_obj_set_style_text_align(lbl_price_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(chart_price, [](lv_event_t *e) {          // colour bars by level: green/yellow/red
        lv_draw_task_t *t = lv_event_get_draw_task(e);
        lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(t);
        if (!base || base->part != LV_PART_ITEMS) return;
        lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
        if (!fill) return;
        int32_t *ys = lv_chart_get_y_array(chart_price, ser_price);
        if (!ys || base->id2 >= (uint32_t)lv_chart_get_point_count(chart_price)) return;
        int32_t v = ys[base->id2];
        int span = price_hi - price_lo ? price_hi - price_lo : 1;
        int pct = (v - price_lo) * 100 / span;
        fill->color = pct < 34 ? COL_GREEN : pct < 67 ? COL_YELLOW : COL_WARN;
    }, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    lv_obj_add_flag(chart_price, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_label_set_text(lbl_price_now, "Henter priser...");
}

void build_weather(lv_obj_t *p) {
    int right = W - 14;
    lbl_w_temp = make_label(p, &font_dk_20, COL_TEXT, LV_ALIGN_TOP_LEFT, 14, 6);
    lbl_w_desc = make_label(p, &font_dk_14, COL_TEXT, LV_ALIGN_TOP_RIGHT, -14, 4, W - 110);
    lv_obj_set_style_text_align(lbl_w_desc, LV_TEXT_ALIGN_RIGHT, 0);
    lbl_w_wind = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_RIGHT, -14, 24, W - 110);
    lv_obj_set_style_text_align(lbl_w_wind, LV_TEXT_ALIGN_RIGHT, 0);
    int chartH = L ? 62 : 84;
    chart_temp = make_chart(p, 12, 46, W - 24, chartH, LV_CHART_TYPE_LINE, 24, &ser_temp);
    lv_obj_t *cap = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 14, 46 + chartH + 2);
    lv_label_set_text(cap, "Næste 24 timer");
    int cardW = (W - 24 - 3 * 5) / 4, cardY = 46 + chartH + 20, cardH = BODY_H - cardY - 6;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_obj_create(p);
        lv_obj_set_size(card, cardW, cardH);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 12 + i * (cardW + 5), cardY);
        lv_obj_set_style_bg_color(card, COL_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lbl_w_days[i]   = make_label(card, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_MID, 0, 0);
        lbl_w_days_c[i] = make_label(card, &font_dk_12, COL_TEXT, LV_ALIGN_CENTER, 0, 0, cardW - 8);
        lv_obj_set_style_text_align(lbl_w_days_c[i], LV_TEXT_ALIGN_CENTER, 0);
        lbl_w_days_t[i] = make_label(card, &font_dk_12, COL_TEXT, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    (void)right;
    lbl_w_err = make_label(p, &font_dk_12, COL_WARN, LV_ALIGN_BOTTOM_MID, 0, -2, W - 16);
    lv_obj_set_style_text_align(lbl_w_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_w_temp, "--°");
    lv_label_set_text(lbl_w_desc, "Henter vejr...");
}

void build_departures(lv_obj_t *p) {
    lbl_d_head = make_label(p, &font_dk_14, COL_MUTED, LV_ALIGN_TOP_MID, 0, 6, W - 16);
    lv_obj_set_style_text_align(lbl_d_head, LV_TEXT_ALIGN_CENTER, 0);
    list_dep = lv_obj_create(p);
    lv_obj_set_size(list_dep, W - 16, BODY_H - 28 - 18);
    lv_obj_align(list_dep, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_opa(list_dep, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_dep, 0, 0);
    lv_obj_set_style_pad_all(list_dep, 0, 0);
    lv_obj_set_style_pad_row(list_dep, 6, 0);
    lv_obj_set_flex_flow(list_dep, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list_dep, LV_OBJ_FLAG_SCROLLABLE);
    lbl_d_err = make_label(p, &font_dk_12, COL_WARN, LV_ALIGN_BOTTOM_MID, 0, -2, W - 16);
    lv_obj_set_style_text_align(lbl_d_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_d_head, "Henter afgange...");
}

void build_home(lv_obj_t *p) {
    list_ha = lv_obj_create(p);
    lv_obj_set_size(list_ha, W - 16, BODY_H - 8 - 18);
    lv_obj_align(list_ha, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_opa(list_ha, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_ha, 0, 0);
    lv_obj_set_style_pad_all(list_ha, 0, 0);
    lv_obj_set_style_pad_row(list_ha, 5, 0);
    lv_obj_set_flex_flow(list_ha, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list_ha, LV_OBJ_FLAG_SCROLLABLE);
    lbl_ha_err = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -2, W - 16);
    lv_obj_set_style_text_align(lbl_ha_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_ha_err, "Home Assistant: indtast token og enheder på websiden");
}

void build_meater(lv_obj_t *p) {
    lbl_m_name = make_label(p, &font_dk_16, COL_TEXT, LV_ALIGN_TOP_LEFT, 12, 4, W - 130);
    lbl_m_state = make_label(p, &font_dk_14, COL_MUTED, LV_ALIGN_TOP_RIGHT, -12, 5, 116);
    lv_obj_set_style_text_align(lbl_m_state, LV_TEXT_ALIGN_RIGHT, 0);
    lbl_m_temp = make_label(p, &font_num_44, COL_TEXT, LV_ALIGN_TOP_LEFT, 10, 28);
    lbl_m_target = make_label(p, &font_dk_14, COL_MUTED, LV_ALIGN_TOP_LEFT, 14, 80, W / 2);
    lbl_m_left = make_label(p, &font_dk_20, COL_TEXT, LV_ALIGN_TOP_RIGHT, -12, 36, W / 2);
    lv_obj_set_style_text_align(lbl_m_left, LV_TEXT_ALIGN_RIGHT, 0);
    lbl_m_left_sub = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_RIGHT, -12, 62, W / 2);
    lv_obj_set_style_text_align(lbl_m_left_sub, LV_TEXT_ALIGN_RIGHT, 0);
    bar_m = lv_bar_create(p);
    lv_obj_set_size(bar_m, W - 24, 12);
    lv_obj_align(bar_m, LV_ALIGN_TOP_LEFT, 12, L ? 106 : 110);
    lv_obj_set_style_bg_color(bar_m, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_m, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_m, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_m, 6, LV_PART_MAIN); lv_obj_set_style_radius(bar_m, 6, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar_m, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(bar_m, 0, 100);
    int cardY = L ? 128 : 136, cardH = L ? 50 : 60, cardW = (W - 24 - 8) / 2;
    lv_obj_t **vals[2] = {&lbl_m_amb, &lbl_m_peak};
    const char *caps[2] = {"Omgivelse", "Højeste"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *card = lv_obj_create(p);
        lv_obj_set_size(card, cardW, cardH);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 12 + i * (cardW + 8), cardY);
        lv_obj_set_style_bg_color(card, COL_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 10, 0); lv_obj_set_style_pad_ver(card, 5, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *c = make_label(card, &font_dk_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_label_set_text(c, caps[i]);
        *vals[i] = make_label(card, &font_dk_16, COL_TEXT, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lbl_m_err = make_label(p, &font_dk_12, COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -2, W - 16);
    lv_obj_set_style_text_align(lbl_m_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_m_err, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_m_temp, "--°");
    lv_label_set_text(lbl_m_name, "Meater");
}

// ---- rendering helpers
int displayedOere(int spotOere) {
    float v = spotOere + cfg.surchargeOere;
    if (cfg.vat) v *= 1.25f;
    return (int)lroundf(v);
}
String kr(int oere) { char b[16]; snprintf(b, sizeof b, "%d,%02d kr", oere / 100, abs(oere % 100)); return String(b); }
const char *dayName(time_t t) {
    static const char *n[] = {"søn", "man", "tir", "ons", "tor", "fre", "lør"};
    struct tm tm_; localtime_r(&t, &tm_); return n[tm_.tm_wday];
}

void render_prices() {
    time_t now = time(nullptr);
    if (!prices.ok || prices.slots.empty()) {
        lv_label_set_text(lbl_price_now, prices.error.length() ? "Ingen priser" : "Henter priser...");
        lv_label_set_text(lbl_price_err, prices.error.c_str());
        return;
    }
    lv_label_set_text(lbl_price_err, "");
    // current slot
    int cur = -1;
    for (size_t i = 0; i < prices.slots.size(); i++) if (prices.slots[i].t <= now && now < prices.slots[i].t + 900) cur = i;
    if (cur >= 0) {
        lv_label_set_text(lbl_price_now, kr(displayedOere(prices.slots[cur].oere)).c_str());
        struct tm a, b; time_t te = prices.slots[cur].t + 900; localtime_r(&prices.slots[cur].t, &a); localtime_r(&te, &b);
        int next = cur + 4 < (int)prices.slots.size() ? displayedOere(prices.slots[cur + 4].oere) : -1;
        int d = next >= 0 ? next - displayedOere(prices.slots[cur].oere) : 0;
        lv_label_set_text_fmt(lbl_price_sub, "kl. %02d:%02d-%02d:%02d  %s  %s", a.tm_hour, a.tm_min, b.tm_hour, b.tm_min,
                              cfg.vat ? "inkl. moms" : "ekskl. moms", next < 0 ? "" : (d > 5 ? LV_SYMBOL_UP : d < -5 ? LV_SYMBOL_DOWN : LV_SYMBOL_MINUS));
    } else {
        lv_label_set_text(lbl_price_now, "--");
        lv_label_set_text(lbl_price_sub, "ingen aktuel pris");
    }
    // next 24 hours, hourly averages starting at the current hour
    struct tm h0; localtime_r(&now, &h0); h0.tm_min = 0; h0.tm_sec = 0; time_t hourStart = mktime(&h0);
    int32_t vals[24]; int count = 0, lo = 1 << 20, hi = -(1 << 20), loIdx = 0, hiIdx = 0;
    for (int h = 0; h < 24; h++) {
        time_t hs = hourStart + h * 3600; int sum = 0, n = 0;
        for (const PriceSlot &s : prices.slots) if (s.t >= hs && s.t < hs + 3600) { sum += displayedOere(s.oere); n++; }
        if (!n) break;
        vals[h] = sum / n; count = h + 1;
        if (vals[h] < lo) { lo = vals[h]; loIdx = h; }
        if (vals[h] > hi) { hi = vals[h]; hiIdx = h; }
    }
    // Never fewer than 2 points: LVGL's bar/line drawing divides by (point_cnt - 1), and a 1-point chart panicked the
    // board three times (IntegerDivideByZero in draw_series_bar) when the price data ended within the current hour.
    int pts = count >= 2 ? count : 2;
    lv_chart_set_point_count(chart_price, pts);
    int32_t axisLo = lo > 0 ? 0 : lo, axisHi = hi > 0 ? hi + hi / 10 : 1;
    if (!count) { axisLo = 0; axisHi = 1; lo = hi = 0; }
    lv_chart_set_range(chart_price, LV_CHART_AXIS_PRIMARY_Y, axisLo, axisHi);
    price_lo = lo; price_hi = hi;
    for (int h = 0; h < pts; h++) lv_chart_set_value_by_id(chart_price, ser_price, h, h < count ? vals[h] : LV_CHART_POINT_NONE);
    lv_chart_refresh(chart_price);
    struct tm t0, t1, t2; time_t x1 = hourStart + 12 * 3600, x2 = hourStart + (count ? count - 1 : 0) * 3600;
    localtime_r(&hourStart, &t0); localtime_r(&x1, &t1); localtime_r(&x2, &t2);
    lv_label_set_text_fmt(lbl_price_x0, "%02d", t0.tm_hour);
    lv_label_set_text_fmt(lbl_price_x1, "%02d", t1.tm_hour);
    lv_label_set_text_fmt(lbl_price_x2, "%02d", t2.tm_hour);
    if (count) {
        struct tm tl, th; time_t tlo = hourStart + loIdx * 3600, thi = hourStart + hiIdx * 3600; localtime_r(&tlo, &tl); localtime_r(&thi, &th);
        lv_label_set_text_fmt(lbl_price_minmax, L ? "Billigst kl. %02d\n%s\n\nDyrest kl. %02d\n%s" : "Billigst kl. %02d: %s  ·  Dyrest kl. %02d: %s",
                              tl.tm_hour, kr(lo).c_str(), th.tm_hour, kr(hi).c_str());
    }
}

void render_weather() {
    if (!weather.ok) { lv_label_set_text(lbl_w_err, weather.error.c_str()); return; }
    lv_label_set_text(lbl_w_err, "");
    lv_label_set_text_fmt(lbl_w_temp, "%.0f°", weather.temp);
    lv_label_set_text(lbl_w_desc, weatherText(weather.code));
    lv_label_set_text_fmt(lbl_w_wind, "Vind %.0f m/s  ·  %s", weather.wind, cfg.place.c_str());
    int n = weather.hourlyCount;
    if (n > 0) {
        int lo = 99, hi = -99;
        for (int i = 0; i < n; i++) { lo = min<int>(lo, weather.hourlyTemp[i]); hi = max<int>(hi, weather.hourlyTemp[i]); }
        int pts = n >= 2 ? n : 2;                       // same divide-by-zero guard as the price chart
        lv_chart_set_point_count(chart_temp, pts);
        lv_chart_set_range(chart_temp, LV_CHART_AXIS_PRIMARY_Y, lo - 2, hi + 2);
        for (int i = 0; i < pts; i++) lv_chart_set_value_by_id(chart_temp, ser_temp, i, i < n ? weather.hourlyTemp[i] : LV_CHART_POINT_NONE);
        lv_chart_refresh(chart_temp);
    }
    for (int i = 0; i < 4; i++) {
        if (i >= weather.dayCount) { lv_label_set_text(lbl_w_days[i], ""); lv_label_set_text(lbl_w_days_c[i], ""); lv_label_set_text(lbl_w_days_t[i], ""); continue; }
        const WeatherDay &d = weather.days[i];
        lv_label_set_text(lbl_w_days[i], i == 0 ? "i dag" : dayName(d.date));
        lv_label_set_text(lbl_w_days_c[i], weatherText(d.code));
        lv_label_set_text_fmt(lbl_w_days_t[i], "%.0f/%.0f", d.tmax, d.tmin);
    }
}

void render_departures() {
    lv_obj_clean(list_dep);
    String head = cfg.line + " " LV_SYMBOL_RIGHT " " + cfg.direction + "  ·  " + (deps.stopName.length() ? deps.stopName : cfg.stopQuery);
    lv_label_set_text(lbl_d_head, head.c_str());
    if (!deps.ok) { lv_label_set_text(lbl_d_err, deps.error.c_str()); return; }
    lv_label_set_text(lbl_d_err, "");
    time_t now = time(nullptr);
    int shown = 0;
    for (const Departure &d : deps.list) {
        long secs = (long)(d.realtime - now);
        if (secs < -60) continue;
        lv_obj_t *row = lv_obj_create(list_dep);
        lv_obj_set_size(row, W - 16, L ? 34 : 40);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        struct tm tp; localtime_r(&d.realtime, &tp);
        lv_obj_t *t = make_label(row, &font_dk_16, d.cancelled ? COL_WARN : COL_TEXT, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text_fmt(t, "%02d:%02d", tp.tm_hour, tp.tm_min);
        lv_obj_t *m = make_label(row, &font_dk_16, COL_TEXT, LV_ALIGN_RIGHT_MID, 0, 0);
        if (d.cancelled) lv_label_set_text(m, "aflyst");
        else if (secs < 60) lv_label_set_text(m, "nu");
        else lv_label_set_text_fmt(m, "%ld min", (secs + 30) / 60);
        lv_obj_t *s = make_label(row, &font_dk_12, COL_MUTED, LV_ALIGN_CENTER, 0, 0);
        if (d.hasRealtime && d.realtime != d.planned) {
            long delay = (long)(d.realtime - d.planned) / 60;
            lv_label_set_text_fmt(s, "%s%ld min", delay > 0 ? "+" : "", delay);
            lv_obj_set_style_text_color(s, delay > 0 ? COL_YELLOW : COL_GREEN, 0);
        } else lv_label_set_text(s, d.hasRealtime ? "realtid" : "plan");
        if (++shown >= (L ? 4 : 4)) break;
    }
    if (!shown) { lv_obj_t *l = lv_label_create(list_dep); lv_label_set_text(l, "Ingen afgange de næste timer"); lv_obj_set_style_text_color(l, COL_MUTED, 0); }
}
} // namespace

void ui_init(int width, int height) {
    W = width; H = height; L = W > H; BODY_H = H - 36;
    scr_status = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_status, COL_BG, 0);
    lv_obj_t *t = lv_label_create(scr_status);
    lv_label_set_text(t, LV_SYMBOL_CHARGE "  Infotavle");
    lv_obj_set_style_text_font(t, &font_dk_20, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, L ? 40 : 70);
    lbl_status = lv_label_create(scr_status);
    lv_obj_set_style_text_font(lbl_status, &font_dk_14, 0);
    lv_obj_set_style_text_color(lbl_status, COL_MUTED, 0);
    lv_obj_set_width(lbl_status, W - 16);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, L ? 90 : 130);

    scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lbl_title = make_label(scr, &font_dk_14, COL_MUTED, LV_ALIGN_TOP_LEFT, 10, 8);
    lbl_clock = make_label(scr, &font_dk_14, COL_TEXT, LV_ALIGN_TOP_MID, 0, 8);
    lbl_wifi = make_label(scr, &font_dk_14, COL_MUTED, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    dots = lv_obj_create(scr);
    lv_obj_set_size(dots, W, 8);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 5, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);

    body = lv_obj_create(scr);
    lv_obj_set_size(body, W, BODY_H);
    lv_obj_set_pos(body, 0, 36);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, on_swipe, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(scr, on_swipe, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(body, on_tap_next, LV_EVENT_CLICKED, nullptr);   // tap anywhere = next page

    for (int i = 0; i < PAGES; i++) page[i] = make_page();
    build_prices(page[0]);
    build_weather(page[1]);
    build_departures(page[2]);
    build_home(page[3]);
    build_meater(page[PAGE_MEATER]);
    show_page(0, 0);
    lv_screen_load(scr_status);
}

void ui_set_status(const char *message) {
    lv_label_set_text(lbl_status, message);
    if (lv_screen_active() != scr_status) lv_screen_load(scr_status);
}
void ui_show_pages() { if (lv_screen_active() == scr_status) lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false); }
void ui_set_wifi_ok(bool ok) { lv_obj_set_style_text_color(lbl_wifi, ok ? COL_MUTED : COL_WARN, 0); }
void ui_update_prices(const PriceData &p) { prices = p; render_prices(); }
void ui_update_weather(const WeatherData &w) { weather = w; render_weather(); }
void ui_update_departures(const DepartureData &d) { deps = d; render_departures(); }

void render_home() {
    lv_obj_clean(list_ha);
    if (ha.items.empty()) { lv_label_set_text(lbl_ha_err, ha.error.length() ? ha.error.c_str() : "Henter..."); return; }
    lv_obj_set_style_text_color(lbl_ha_err, ha.ok ? COL_MUTED : COL_WARN, 0);
    lv_label_set_text(lbl_ha_err, ha.error.c_str());
    int rowH = L ? 30 : 34, maxRows = (BODY_H - 26) / (rowH + 5);
    int shown = 0;
    for (const HaItem &it : ha.items) {
        lv_obj_t *row = lv_obj_create(list_ha);
        lv_obj_set_size(row, W - 16, rowH);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = make_label(row, &font_dk_14, it.ok ? COL_MUTED : COL_WARN, LV_ALIGN_LEFT_MID, 0, 0, (W - 36) * 55 / 100);
        lv_label_set_text(n, it.label.length() ? it.label.c_str() : it.id.c_str());
        lv_obj_t *v = make_label(row, &font_dk_16, it.ok ? COL_TEXT : COL_WARN, LV_ALIGN_RIGHT_MID, 0, 0);
        String val = it.state; if (it.ok && it.unit.length()) { val += it.unit == "%" || it.unit.startsWith("°") ? "" : " "; val += it.unit; }
        lv_label_set_text(v, val.c_str());
        if (++shown >= maxRows) break;
    }
}
void ui_update_home(const HaData &h) { ha = h; render_home(); }
// ---- cooking page
namespace {
struct CookState { const char *key, *text; lv_color_t color; bool alert; };
const CookState *cookState(const String &k) {
    static const CookState st[] = {
        {"not_started", "Ikke startet", COL_MUTED, false}, {"configured", "Klar til start", COL_MUTED, false},
        {"started", "I gang", COL_GREEN, false}, {"ready_for_resting", "Tag den ud nu!", COL_YELLOW, true},
        {"resting", "Hviler", COL_ACCENT, false}, {"slightly_underdone", "Lidt for lidt", COL_YELLOW, false},
        {"finished", "Færdig", COL_GREEN, false}, {"slightly_overdone", "Lidt for meget", COL_WARN, true},
        {"overcooked", "Overstegt!", COL_WARN, true}};
    for (const CookState &c : st) if (k == c.key) return &c;
    return nullptr;
}
String deg(float v) { if (isnan(v)) return "--°"; char b[12]; snprintf(b, sizeof b, "%.0f°", v); return String(b); }

void render_meater() {
    time_t now = time(nullptr);
    const MeaterData &m = meater;
    if (!m.online) {
        lv_label_set_text(lbl_m_name, "Meater");
        lv_label_set_text(lbl_m_state, m.ok ? "Ikke forbundet" : "");
        lv_obj_set_style_text_color(lbl_m_state, COL_MUTED, 0);
        lv_label_set_text(lbl_m_temp, "--°");
        lv_label_set_text(lbl_m_target, ""); lv_label_set_text(lbl_m_left, ""); lv_label_set_text(lbl_m_left_sub, "");
        lv_bar_set_value(bar_m, 0, LV_ANIM_OFF);
        lv_label_set_text(lbl_m_amb, "--°"); lv_label_set_text(lbl_m_peak, "--°");
        lv_obj_set_style_text_color(lbl_m_err, m.ok ? COL_MUTED : COL_WARN, 0);
        lv_label_set_text(lbl_m_err, !m.ok ? (m.error.length() ? m.error.c_str() : "Henter...")
                                           : "Proben sender ikke. Åbn Meater-appen tæt på proben.");
        return;
    }
    const CookState *cs = m.cooking ? cookState(m.state) : nullptr;
    lv_label_set_text(lbl_m_name, m.cooking && m.name.length() ? m.name.c_str() : "Meater");
    lv_label_set_text(lbl_m_state, cs ? cs->text : m.cooking ? m.state.c_str() : "Ingen stegning");
    lv_obj_set_style_text_color(lbl_m_state, cs ? cs->color : COL_MUTED, 0);
    lv_label_set_text(lbl_m_temp, deg(m.internal).c_str());
    lv_obj_set_style_text_color(lbl_m_temp, cs && cs->alert ? cs->color : COL_TEXT, 0);
    lv_label_set_text(lbl_m_amb, deg(m.ambient).c_str());
    lv_label_set_text(lbl_m_peak, deg(m.cooking ? m.peak : NAN).c_str());
    if (m.cooking && !isnan(m.target) && m.target > 0) {
        lv_label_set_text_fmt(lbl_m_target, "Mål %s", deg(m.target).c_str());
        int pct = isnan(m.internal) ? 0 : (int)lroundf(m.internal * 100.0f / m.target);
        lv_bar_set_value(bar_m, pct < 0 ? 0 : pct > 100 ? 100 : pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_m, pct >= 100 ? COL_GREEN : COL_ACCENT, LV_PART_INDICATOR);
    } else {
        lv_label_set_text(lbl_m_target, m.cooking ? "" : "Sæt en stegning op i appen");
        lv_bar_set_value(bar_m, 0, LV_ANIM_OFF);
    }
    // remaining time counts down locally between fetches
    bool past = m.state == "ready_for_resting" || m.state == "resting" || m.state == "finished" || m.state == "slightly_overdone" || m.state == "overcooked";
    if (!m.cooking) { lv_label_set_text(lbl_m_left, ""); lv_label_set_text(lbl_m_left_sub, ""); }
    else if (past) {
        lv_label_set_text(lbl_m_left, m.state == "resting" ? "Hviler" : "Klar");
        if (m.startedAt) { long e = (long)(now - m.startedAt) / 60; lv_label_set_text_fmt(lbl_m_left_sub, "i alt %ld t %02ld min", e / 60, e % 60); }
        else lv_label_set_text(lbl_m_left_sub, "");
    } else if (!m.doneAt) { lv_label_set_text(lbl_m_left, "Beregner..."); lv_label_set_text(lbl_m_left_sub, "Meater regner tiden ud"); }
    else {
        long s = (long)(m.doneAt - now);
        if (s < 60) lv_label_set_text(lbl_m_left, "< 1 min");
        else if (s < 3600) lv_label_set_text_fmt(lbl_m_left, "%ld min", (s + 30) / 60);
        else lv_label_set_text_fmt(lbl_m_left, "%ld t %02ld min", s / 3600, (s % 3600) / 60);
        struct tm t; localtime_r(&m.doneAt, &t);
        lv_label_set_text_fmt(lbl_m_left_sub, "tilbage · klar ca. %02d:%02d", t.tm_hour, t.tm_min);
    }
    lv_obj_set_style_text_color(lbl_m_err, COL_MUTED, 0);
    long age = m.fetchedAt ? (long)(now - m.fetchedAt) : 0;
    if (!m.error.isEmpty()) { lv_obj_set_style_text_color(lbl_m_err, COL_WARN, 0); lv_label_set_text(lbl_m_err, m.error.c_str()); }
    else if (age > 120) lv_label_set_text_fmt(lbl_m_err, "Sidst opdateret for %ld min siden", age / 60);
    else lv_label_set_text(lbl_m_err, "");
}
} // namespace

bool ui_update_meater(const MeaterData &m) {
    MeaterData prev = meater;
    // a failed fetch keeps the last good values on screen, with the error underneath
    if (!m.ok && prev.online) { meater.error = m.error; render_meater(); return false; }
    meater = m;
    render_meater();
    const CookState *cs = m.cooking ? cookState(m.state) : nullptr, *ps = prev.cooking ? cookState(prev.state) : nullptr;
    bool alert = cs && cs->alert && cs != ps;
    if ((m.cooking && !prev.cooking) || alert) show_page(PAGE_MEATER, active == PAGE_MEATER ? 0 : W);   // a cook starts / meat is ready
    return alert;
}

void ui_apply_rotation() { lv_obj_invalidate(lv_screen_active()); }

void ui_tick() {
    time_t now = time(nullptr);
    struct tm t; localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) lv_label_set_text_fmt(lbl_clock, "%02d:%02d", t.tm_hour, t.tm_min);
    else lv_label_set_text(lbl_clock, "");
    if (meater.online) render_meater();       // countdown + "last updated"
    // during a cook an untouched board drifts back to the cooking page after a minute
    if (meater.cooking && active != PAGE_MEATER && lv_display_get_inactive_time(nullptr) > 60000) show_page(PAGE_MEATER, 0);
    static int lastMin = -1;
    if (t.tm_min != lastMin) {                 // minute boundary: countdowns and the current price slot
        lastMin = t.tm_min;
        if (deps.ok) render_departures();
        if (prices.ok && (t.tm_min % 15) == 0) render_prices();
    }
}

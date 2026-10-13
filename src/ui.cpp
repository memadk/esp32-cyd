#include "ui.h"
#include "app.h"
#include "config.h"
#include <time.h>

namespace {

const lv_color_t COL_BG     = lv_color_hex(0x101820);
const lv_color_t COL_TEXT   = lv_color_hex(0xF2F4F7);
const lv_color_t COL_MUTED  = lv_color_hex(0x8A94A6);
const lv_color_t COL_ACCENT = lv_color_hex(0x3B82F6);
const lv_color_t COL_CARD   = lv_color_hex(0x1B2634);
const lv_color_t COL_HILITE = lv_color_hex(0x1E3A5F);
const lv_color_t COL_WARN   = lv_color_hex(0xE0533A);


lv_obj_t *scr_status, *lbl_status;
lv_obj_t *scr_np, *np_body, *dots, *lbl_room, *lbl_clock, *lbl_wifi, *img_art, *art_placeholder, *lbl_title, *lbl_artist, *lbl_station;
lv_obj_t *bar_prog, *lbl_pos, *lbl_dur;
lv_obj_t *btn_prev, *btn_next, *btn_mute, *btn_favs, *btn_play, *lbl_play_icon, *lbl_mute_icon;
lv_obj_t *btn_vol_down, *btn_vol_up, *lbl_vol, *lbl_vol_icon, *bar_vol;
lv_obj_t *scr_fav, *list_fav;
lv_obj_t *scr_rooms, *list_rooms;
lv_obj_t *scr_group, *lbl_group_title, *lbl_group_info, *btn_group_a, *btn_group_b;
lv_obj_t *scr_queue, *list_queue, *lbl_queue_hdr;

const uint16_t *art_pixels = nullptr;
lv_image_dsc_t art_dsc;
bool art_visible = false;
std::vector<SonosFavorite> favorites;
std::vector<ZoneGroupInfo> groups;
int selected_group = -1;               // -2 = Wi-Fi setup confirmation
SonosTransport last_transport = SonosTransport::Unknown;
bool last_muted = false;
bool radio_mode = false;
String room_name;

int ui_volume = -1;
int page_count = 0, page_active = -1;
lv_anim_t slide_anim;
uint32_t vol_touched_ms = 0;
// progress extrapolation
int prog_pos = -1, prog_dur = -1;
uint32_t prog_base_ms = 0;
bool prog_playing = false;

lv_obj_t *make_icon_button(lv_obj_t *parent, const char *symbol, int w, int h, lv_event_cb_t cb, const lv_font_t *font = &font_dk_20) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, COL_CARD, 0);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y, bool scroll) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, 224);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, scroll ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

void style_list_button(lv_obj_t *btn, bool highlight, lv_color_t text) {
    lv_obj_set_style_bg_color(btn, highlight ? COL_HILITE : COL_CARD, 0);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, text, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 14, 0);
}

void show_play_icon(SonosTransport t) {
    lv_label_set_text(lbl_play_icon, t == SonosTransport::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
void show_mute_icons(bool muted) {
    lv_obj_set_style_bg_color(bar_vol, muted ? COL_MUTED : COL_ACCENT, LV_PART_INDICATOR);
    lv_label_set_text(lbl_mute_icon, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_label_set_text(lbl_vol_icon, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vol_icon, muted ? COL_WARN : COL_MUTED, 0);
}
void show_volume(int v) {
    ui_volume = v;
    if (v >= 0) { lv_label_set_text_fmt(lbl_vol, "%d", v); lv_bar_set_value(bar_vol, v, LV_ANIM_ON); }
    else { lv_label_set_text(lbl_vol, "--"); lv_bar_set_value(bar_vol, 0, LV_ANIM_OFF); }
}
void set_radio_mode(bool radio) {
    radio_mode = radio;
    auto vis = [](lv_obj_t *o, bool show) { if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); };
    vis(btn_prev, !radio); vis(btn_next, !radio);
    vis(btn_mute, radio);  vis(btn_favs, radio);
}
void show_progress(bool visible) {
    auto vis = [](lv_obj_t *o, bool show) { if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); };
    vis(bar_prog, visible); vis(lbl_pos, visible); vis(lbl_dur, visible);
}
void fmt_time(char *out, size_t n, int sec) {
    if (sec < 0) sec = 0;
    if (sec >= 3600) snprintf(out, n, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    else snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}
void refresh_progress() {
    if (prog_dur <= 0) return;
    int pos = prog_pos;
    if (prog_playing && prog_base_ms) pos += (int)((millis() - prog_base_ms) / 1000);
    if (pos > prog_dur) pos = prog_dur;
    char a[16], b[16];
    fmt_time(a, sizeof a, pos); fmt_time(b, sizeof b, prog_dur);
    lv_label_set_text(lbl_pos, a);
    lv_label_set_text(lbl_dur, b);
    lv_bar_set_value(bar_prog, pos, LV_ANIM_OFF);
}

// ---- event handlers. UI reacts immediately; the Sonos state corrects it afterwards if needed.
void on_play_pause(lv_event_t *) {
    bool playing = last_transport == SonosTransport::Playing;
    app_send(playing ? AppCmd::Pause : AppCmd::Play);
    last_transport = playing ? SonosTransport::Paused : SonosTransport::Playing;
    prog_playing = !playing; prog_base_ms = millis();
    if (playing && prog_dur > 0) { /* freeze position */ }
    show_play_icon(last_transport);
}
void on_mute(lv_event_t *) {
    app_send(AppCmd::ToggleMute);
    last_muted = !last_muted;
    show_mute_icons(last_muted);
}
void on_next(lv_event_t *) { app_send(AppCmd::Next); }
void on_prev(lv_event_t *) { app_send(AppCmd::Prev); }
void change_volume(int delta) {
    if (ui_volume < 0) return;
    int v = ui_volume + delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v == ui_volume) return;
    show_volume(v);
    vol_touched_ms = millis();
    app_send(AppCmd::SetVolume, v);
}
void on_vol_down(lv_event_t *) { change_volume(-cfg.volStep); }
void on_vol_up(lv_event_t *)   { change_volume(+cfg.volStep); }
void on_open_favorites(lv_event_t *) {
    app_send(AppCmd::RefreshFavorites);
    lv_screen_load_anim(scr_fav, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
void on_close_to_np(lv_event_t *) {
    if (lv_screen_active() == scr_queue) lv_obj_clean(list_queue);   // 30 rows x 4 widgets: give the heap back
    lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}
void on_favorite_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < (int)favorites.size() && favorites[idx].playable()) {
        app_send(AppCmd::PlayFavorite, idx);
        lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    }
}
void on_art_clicked(lv_event_t *) {          // queue for tracks, favourites for radio
    if (radio_mode) { on_open_favorites(nullptr); return; }
    app_send(AppCmd::RefreshQueue);
    lv_label_set_text(lbl_queue_hdr, "Kø");
    lv_obj_clean(list_queue);
    lv_obj_t *l = lv_list_add_text(list_queue, "Henter...");
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_screen_load_anim(scr_queue, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
void on_queue_item_clicked(lv_event_t *e) {
    int number = (int)(intptr_t)lv_event_get_user_data(e);
    if (number > 0) {
        app_send(AppCmd::PlayQueueTrack, number);
        lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        lv_obj_clean(list_queue);
    }
}

void on_open_rooms(lv_event_t *) {
    app_send(AppCmd::RefreshGroups);
    lv_screen_load_anim(scr_rooms, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
void on_close_group(lv_event_t *) { lv_screen_load_anim(scr_rooms, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false); }
void set_button_text(lv_obj_t *btn, const String &text) { lv_label_set_text(lv_obj_get_child(btn, 0), text.c_str()); }

void on_group_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)groups.size()) return;
    selected_group = idx;
    const ZoneGroupInfo &g = groups[idx];
    lv_label_set_text(lbl_group_title, g.label().c_str());
    if (g.containsRoom) {
        if (g.zoneNames.size() > 1) {
            lv_label_set_text_fmt(lbl_group_info, "%s spiller sammen med gruppen", room_name.c_str());
            set_button_text(btn_group_a, LV_SYMBOL_CLOSE "  Fjern " + room_name + " fra gruppen");
            lv_obj_remove_flag(btn_group_a, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text_fmt(lbl_group_info, "%s spiller alene", room_name.c_str());
            lv_obj_add_flag(btn_group_a, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(btn_group_b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_group_info, "");
        set_button_text(btn_group_a, LV_SYMBOL_PLUS "  Saml " + room_name + " med gruppen");
        set_button_text(btn_group_b, LV_SYMBOL_SHUFFLE "  Styr " + g.zoneNames[0] + " i stedet");
        lv_obj_remove_flag(btn_group_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_group_b, LV_OBJ_FLAG_HIDDEN);
    }
    lv_screen_load_anim(scr_group, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
void on_wifi_setup_clicked(lv_event_t *) {
    selected_group = -2;
    lv_label_set_text(lbl_group_title, "Wi-Fi-opsætning");
    lv_label_set_text(lbl_group_info, "Enheden genstarter og opretter netværket \"Sonos-Remote\", hvor du kan vælge et nyt Wi-Fi.");
    set_button_text(btn_group_a, LV_SYMBOL_WIFI "  Start opsætning");
    lv_obj_remove_flag(btn_group_a, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_group_b, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load_anim(scr_group, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
void on_group_action_a(lv_event_t *) {
    if (selected_group == -2) { app_send(AppCmd::WifiSetup); return; }
    if (selected_group < 0 || selected_group >= (int)groups.size()) return;
    app_send(groups[selected_group].containsRoom ? AppCmd::LeaveGroup : AppCmd::JoinGroup, selected_group);
    lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}
void on_group_action_b(lv_event_t *) {
    if (selected_group < 0 || selected_group >= (int)groups.size()) return;
    app_send(AppCmd::ControlRoom, selected_group);
    lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void render_dots() {
    lv_obj_clean(dots);
    if (page_count < 2) return;
    for (int i = 0; i < page_count; i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_set_size(d, i == page_active ? 14 : 5, 5);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, i == page_active ? COL_ACCENT : COL_MUTED, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
}
void slide_exec(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
void slide_in(int from_x) {
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, np_body);
    lv_anim_set_exec_cb(&slide_anim, slide_exec);
    lv_anim_set_values(&slide_anim, from_x, 0);
    lv_anim_set_duration(&slide_anim, 180);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_start(&slide_anim);
}
void on_swipe(lv_event_t *) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (page_count < 2 || page_active < 0) return;
    int next = page_active;
    if (dir == LV_DIR_LEFT) next = (page_active + 1) % page_count;
    else if (dir == LV_DIR_RIGHT) next = (page_active + page_count - 1) % page_count;
    else return;
    if (next == page_active || next >= (int)groups.size()) return;
    page_active = next;
    render_dots();
    // optimistic UI: new room name, placeholders until the Sonos task has switched
    room_name = groups[next].zoneNames[0];
    lv_label_set_text(lbl_room, room_name.c_str());
    lv_label_set_text_fmt(lbl_title, "Forbinder til %s...", room_name.c_str());
    lv_label_set_text(lbl_artist, "");
    lv_label_set_text(lbl_station, "");
    show_progress(false);
    ui_set_art(false);
    slide_in(dir == LV_DIR_LEFT ? 240 : -240);
    app_send(AppCmd::SwipeRoom, next);
}

void second_timer(lv_timer_t *) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) lv_label_set_text_fmt(lbl_clock, "%02d:%02d", t.tm_hour, t.tm_min);
    else lv_label_set_text(lbl_clock, "");
    refresh_progress();
}

// ---- screens
lv_obj_t *make_list_screen(const char *title, lv_event_cb_t back_cb, lv_obj_t **list_out, lv_obj_t **hdr_out = nullptr) {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_t *back = make_icon_button(scr, LV_SYMBOL_LEFT, 72, 34, back_cb, &font_dk_16);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_ext_click_area(back, 4);
    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_font(hdr, &font_dk_16, 0);
    lv_obj_set_style_text_color(hdr, COL_TEXT, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 8);
    if (hdr_out) *hdr_out = hdr;
    if (list_out) {
        lv_obj_t *list = lv_list_create(scr);
        lv_obj_set_size(list, 240, 320 - 40);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_style_bg_color(list, COL_BG, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_row(list, 6, 0);
        lv_obj_set_style_radius(list, 0, 0);
        *list_out = list;
    }
    return scr;
}

lv_obj_t *make_wide_button(lv_obj_t *parent, int y, lv_event_cb_t cb, bool accent) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 216, 52);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, accent ? COL_ACCENT : COL_CARD, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &font_dk_14, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_width(l, 200);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return btn;
}

void build_status_screen() {
    scr_status = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_status, COL_BG, 0);
    lv_obj_t *t = lv_label_create(scr_status);
    lv_label_set_text(t, LV_SYMBOL_AUDIO "  Sonos");
    lv_obj_set_style_text_font(t, &font_dk_20, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 70);
    lbl_status = make_label(scr_status, &font_dk_14, COL_MUTED, 130, false);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
}

void build_now_playing() {
    scr_np = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_np, COL_BG, 0);
    lv_obj_remove_flag(scr_np, LV_OBJ_FLAG_SCROLLABLE);

    // status bar: the two tappable items are full-height buttons (0..28 px, +4 px ext click area) so a
    // finger near the top edge of the glass still lands on them; the clock and Wi-Fi icon sit between them.
    lv_obj_t *btn_room = lv_button_create(scr_np);
    lv_obj_set_size(btn_room, 118, 28);
    lv_obj_align(btn_room, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_style_bg_color(btn_room, COL_CARD, 0);
    lv_obj_set_style_bg_color(btn_room, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_room, 10, 0);
    lv_obj_set_style_shadow_width(btn_room, 0, 0);
    lv_obj_set_style_pad_hor(btn_room, 8, 0);
    lv_obj_set_ext_click_area(btn_room, 4);
    lv_obj_add_event_cb(btn_room, on_open_rooms, LV_EVENT_CLICKED, nullptr);
    lbl_room = lv_label_create(btn_room);
    lv_label_set_text(lbl_room, room_name.c_str());
    lv_obj_set_style_text_font(lbl_room, &font_dk_14, 0);
    lv_obj_set_style_text_color(lbl_room, COL_TEXT, 0);
    lv_obj_set_width(lbl_room, 86);
    lv_label_set_long_mode(lbl_room, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_room, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *chev = lv_label_create(btn_room);
    lv_label_set_text(chev, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(chev, &font_dk_12, 0);
    lv_obj_set_style_text_color(chev, COL_MUTED, 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);

    lbl_clock = lv_label_create(scr_np);
    lv_label_set_text(lbl_clock, "");
    lv_obj_set_style_text_font(lbl_clock, &font_dk_14, 0);
    lv_obj_set_style_text_color(lbl_clock, COL_TEXT, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 125, 6);

    lbl_wifi = lv_label_create(scr_np);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(lbl_wifi, &font_dk_12, 0);
    lv_obj_set_style_text_color(lbl_wifi, COL_MUTED, 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_RIGHT, -64, 8);

    lv_obj_t *fav_btn = make_icon_button(scr_np, LV_SYMBOL_LIST, 58, 28, on_open_favorites, &font_dk_16);
    lv_obj_set_style_radius(fav_btn, 10, 0);
    lv_obj_align(fav_btn, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_set_ext_click_area(fav_btn, 4);

    // page dots (one per zone group) just under the status bar
    dots = lv_obj_create(scr_np);
    lv_obj_set_size(dots, 240, 5);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 29);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 5, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);

    // sliding body: art, text, progress, transport row. Swipe left/right switches room.
    np_body = lv_obj_create(scr_np);
    lv_obj_set_size(np_body, 240, 320 - 34 - 40);
    lv_obj_set_pos(np_body, 0, 34);
    lv_obj_set_style_bg_opa(np_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(np_body, 0, 0);
    lv_obj_set_style_pad_all(np_body, 0, 0);
    lv_obj_set_style_radius(np_body, 0, 0);
    lv_obj_remove_flag(np_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(np_body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(np_body, on_swipe, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(scr_np, on_swipe, LV_EVENT_GESTURE, nullptr);
    const int B = -34;   // body is offset 34 px from the screen top

    // album art (tap: queue / favourites)
    art_placeholder = lv_obj_create(np_body);
    lv_obj_set_size(art_placeholder, ART_SIZE, ART_SIZE);
    lv_obj_align(art_placeholder, LV_ALIGN_TOP_MID, 0, 32 + B + 2);
    lv_obj_set_style_bg_color(art_placeholder, COL_CARD, 0);
    lv_obj_set_style_border_width(art_placeholder, 0, 0);
    lv_obj_set_style_radius(art_placeholder, 10, 0);
    lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(art_placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(art_placeholder, on_art_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ph = lv_label_create(art_placeholder);
    lv_label_set_text(ph, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ph, &font_dk_20, 0);
    lv_obj_set_style_text_color(ph, COL_MUTED, 0);
    lv_obj_center(ph);

    img_art = lv_image_create(np_body);
    lv_obj_align(img_art, LV_ALIGN_TOP_MID, 0, 32 + B + 2);
    lv_obj_set_style_radius(img_art, 10, 0);
    lv_obj_set_style_clip_corner(img_art, true, 0);
    lv_obj_add_flag(img_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img_art, on_art_clicked, LV_EVENT_CLICKED, nullptr);

    // text
    lbl_title   = make_label(np_body, &font_dk_16, COL_TEXT, 158 + B, true);
    lbl_artist  = make_label(np_body, &font_dk_14, COL_MUTED, 180 + B, true);
    lbl_station = make_label(np_body, &font_dk_12, COL_MUTED, 199 + B, false);

    // progress
    lbl_pos = lv_label_create(np_body);
    lv_obj_set_style_text_font(lbl_pos, &font_dk_12, 0);
    lv_obj_set_style_text_color(lbl_pos, COL_MUTED, 0);
    lv_obj_align(lbl_pos, LV_ALIGN_TOP_LEFT, 12, 214 + B);
    lbl_dur = lv_label_create(np_body);
    lv_obj_set_style_text_font(lbl_dur, &font_dk_12, 0);
    lv_obj_set_style_text_color(lbl_dur, COL_MUTED, 0);
    lv_obj_align(lbl_dur, LV_ALIGN_TOP_RIGHT, -12, 214 + B);
    bar_prog = lv_bar_create(np_body);
    lv_obj_set_size(bar_prog, 130, 4);
    lv_obj_align(bar_prog, LV_ALIGN_TOP_MID, 0, 220 + B);
    lv_obj_set_style_bg_color(bar_prog, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_prog, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_prog, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_prog, 2, LV_PART_INDICATOR);
    lv_label_set_text(lbl_pos, ""); lv_label_set_text(lbl_dur, "");
    show_progress(false);

    // transport row: [prev|mute] [play/pause] [next|favourites]
    const int ROW_Y = 236 + B;
    btn_prev = make_icon_button(np_body, LV_SYMBOL_PREV, 56, 42, on_prev);
    lv_obj_align(btn_prev, LV_ALIGN_TOP_MID, -70, ROW_Y);
    btn_mute = make_icon_button(np_body, LV_SYMBOL_VOLUME_MAX, 56, 42, on_mute);
    lv_obj_align(btn_mute, LV_ALIGN_TOP_MID, -70, ROW_Y);
    lbl_mute_icon = lv_obj_get_child(btn_mute, 0);
    btn_play = make_icon_button(np_body, LV_SYMBOL_PLAY, 68, 42, on_play_pause);
    lv_obj_set_style_bg_color(btn_play, COL_ACCENT, 0);
    lv_obj_align(btn_play, LV_ALIGN_TOP_MID, 0, ROW_Y);
    lbl_play_icon = lv_obj_get_child(btn_play, 0);
    btn_next = make_icon_button(np_body, LV_SYMBOL_NEXT, 56, 42, on_next);
    lv_obj_align(btn_next, LV_ALIGN_TOP_MID, 70, ROW_Y);
    btn_favs = make_icon_button(np_body, LV_SYMBOL_LIST, 56, 42, on_open_favorites);
    lv_obj_align(btn_favs, LV_ALIGN_TOP_MID, 70, ROW_Y);
    set_radio_mode(false);

    // volume row: [icon] [-] [nn] [+]
    lbl_vol_icon = lv_label_create(scr_np);
    lv_label_set_text(lbl_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vol_icon, COL_MUTED, 0);
    lv_obj_align(lbl_vol_icon, LV_ALIGN_BOTTOM_LEFT, 14, -12);
    lv_obj_add_flag(lbl_vol_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_vol_icon, 10);
    lv_obj_add_event_cb(lbl_vol_icon, on_mute, LV_EVENT_CLICKED, nullptr);

    btn_vol_down = make_icon_button(scr_np, LV_SYMBOL_MINUS, 56, 32, on_vol_down, &font_dk_16);
    lv_obj_align(btn_vol_down, LV_ALIGN_BOTTOM_MID, -60, -4);
    lv_obj_add_event_cb(btn_vol_down, on_vol_down, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    btn_vol_up = make_icon_button(scr_np, LV_SYMBOL_PLUS, 56, 32, on_vol_up, &font_dk_16);
    lv_obj_align(btn_vol_up, LV_ALIGN_BOTTOM_MID, 60, -4);
    lv_obj_add_event_cb(btn_vol_up, on_vol_up, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

    lbl_vol = lv_label_create(scr_np);
    lv_label_set_text(lbl_vol, "--");
    lv_obj_set_style_text_font(lbl_vol, &font_dk_20, 0);
    lv_obj_set_style_text_color(lbl_vol, COL_TEXT, 0);
    lv_obj_align(lbl_vol, LV_ALIGN_BOTTOM_MID, 0, -13);

    // read-only level bar under the number (the +/- buttons remain the way to change it)
    bar_vol = lv_bar_create(scr_np);
    lv_obj_set_size(bar_vol, 64, 4);
    lv_bar_set_range(bar_vol, 0, 100);
    lv_obj_set_style_bg_color(bar_vol, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vol, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vol, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_vol, 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar_vol, 150, 0);
    lv_obj_remove_flag(bar_vol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bar_vol, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_timer_create(second_timer, 1000, nullptr);
}

void build_lists() {
    scr_fav   = make_list_screen("Favoritter", on_close_to_np, &list_fav);
    scr_rooms = make_list_screen("Rum", on_close_to_np, &list_rooms);
    scr_queue = make_list_screen("Kø", on_close_to_np, &list_queue, &lbl_queue_hdr);

    scr_group = make_list_screen("", on_close_group, nullptr);
    lbl_group_title = make_label(scr_group, &font_dk_16, COL_TEXT, 60, false);
    lv_label_set_long_mode(lbl_group_title, LV_LABEL_LONG_WRAP);
    lbl_group_info = make_label(scr_group, &font_dk_12, COL_MUTED, 104, false);
    lv_label_set_long_mode(lbl_group_info, LV_LABEL_LONG_WRAP);
    btn_group_a = make_wide_button(scr_group, 170, on_group_action_a, true);
    btn_group_b = make_wide_button(scr_group, 234, on_group_action_b, false);
}

} // namespace

// ---------------------------------------------------------------- public API
void ui_init(const char *roomName, const uint16_t *artBuffer) {
    room_name = roomName;
    art_pixels = artBuffer;
    art_dsc = {};
    art_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    art_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    art_dsc.header.w      = ART_SIZE;
    art_dsc.header.h      = ART_SIZE;
    art_dsc.header.stride = ART_SIZE * 2;
    art_dsc.data_size     = ART_SIZE * ART_SIZE * 2;
    art_dsc.data          = reinterpret_cast<const uint8_t *>(art_pixels);

    build_status_screen();
    build_now_playing();
    build_lists();
    lv_screen_load(scr_status);
}

void ui_set_status(const char *message) {
    lv_label_set_text(lbl_status, message);
    if (lv_screen_active() != scr_status) lv_screen_load(scr_status);
}

void ui_show_now_playing() {
    if (lv_screen_active() == scr_status) lv_screen_load_anim(scr_np, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
}

void ui_set_wifi_ok(bool ok) {
    lv_obj_set_style_text_color(lbl_wifi, ok ? COL_MUTED : COL_WARN, 0);
}

void ui_set_room(const char *room) {
    room_name = room;
    lv_label_set_text(lbl_room, room);
}

void ui_update_state(const SonosState &s) {
    last_transport = s.transport;
    show_play_icon(s.transport);
    last_muted = s.muted;
    show_mute_icons(s.muted);

    if (s.groupZones > 1) lv_label_set_text_fmt(lbl_room, "%s +%d", room_name.c_str(), s.groupZones - 1);
    else lv_label_set_text(lbl_room, room_name.c_str());

    const SonosTrack &t = s.track;
    bool radio = t.streamContent.length() > 0 || (t.station.length() > 0 && t.artist.isEmpty() && t.durationSec <= 0);
    if (!s.online) {
        lv_label_set_text(lbl_title, "Sonos ikke fundet");
        lv_label_set_text(lbl_artist, "");
        lv_label_set_text(lbl_station, "");
    } else if (t.title.isEmpty() && t.station.isEmpty()) {
        const char *msg = s.transport == SonosTransport::Stopped ? "Intet afspilles"
                        : s.transport == SonosTransport::Transitioning ? "Indlæser..."
                        : s.transport == SonosTransport::Paused ? "Pause" : "Klar";
        lv_label_set_text(lbl_title, msg);
        lv_label_set_text(lbl_artist, "");
        lv_label_set_text(lbl_station, "");
    } else if (radio) {
        lv_label_set_text(lbl_title, t.streamContent.length() ? t.streamContent.c_str()
                                                              : (t.station.length() ? t.station.c_str() : t.title.c_str()));
        lv_label_set_text(lbl_artist, t.streamContent.length() ? t.station.c_str() : "");
        lv_label_set_text(lbl_station, LV_SYMBOL_AUDIO " Radio");
    } else {
        lv_label_set_text(lbl_title, t.title.c_str());
        lv_label_set_text(lbl_artist, t.artist.c_str());
        if (t.fromQueue && t.queueLength > 0) lv_label_set_text_fmt(lbl_station, "%s  ·  %d/%d", t.album.c_str(), t.trackNo, t.queueLength);
        else lv_label_set_text(lbl_station, t.album.c_str());
    }
    if (s.online && s.transport == SonosTransport::Transitioning)
        lv_label_set_text(lbl_station, LV_SYMBOL_REFRESH " Indlæser...");     // buffering before playback starts
    if (s.online && radio != radio_mode) set_radio_mode(radio);

    // progress
    if (s.online && !radio && t.durationSec > 0) {
        prog_dur = t.durationSec;
        prog_pos = t.positionSec < 0 ? 0 : t.positionSec;
        prog_base_ms = s.positionAtMs ? s.positionAtMs : millis();
        prog_playing = s.transport == SonosTransport::Playing;
        lv_bar_set_range(bar_prog, 0, prog_dur);
        show_progress(true);
        refresh_progress();
    } else {
        prog_dur = -1;
        show_progress(false);
    }

    if (s.volume >= 0 && millis() - vol_touched_ms > 1500) show_volume(s.volume);
}

void ui_update_favorites(const std::vector<SonosFavorite> &favs) {
    favorites = favs;
    lv_obj_clean(list_fav);
    for (size_t i = 0; i < favorites.size(); i++) {
        const SonosFavorite &f = favorites[i];
        lv_obj_t *btn = lv_list_add_button(list_fav, f.playable() ? LV_SYMBOL_PLAY : LV_SYMBOL_CLOSE, f.title.c_str());
        style_list_button(btn, false, f.playable() ? COL_TEXT : COL_MUTED);
        lv_obj_add_event_cb(btn, on_favorite_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (favorites.empty()) {
        lv_obj_t *l = lv_list_add_text(list_fav, "Ingen favoritter fundet");
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
    }
}

void ui_update_groups(const std::vector<ZoneGroupInfo> &gs) {
    groups = gs;
    page_count = groups.size();
    page_active = -1;
    for (size_t i = 0; i < groups.size(); i++) if (groups[i].containsRoom) page_active = i;
    render_dots();
    lv_obj_clean(list_rooms);
    for (size_t i = 0; i < groups.size(); i++) {
        const ZoneGroupInfo &g = groups[i];
        lv_obj_t *btn = lv_list_add_button(list_rooms, g.containsRoom ? LV_SYMBOL_OK : (g.zoneNames.size() > 1 ? LV_SYMBOL_LIST : LV_SYMBOL_AUDIO), g.label().c_str());
        style_list_button(btn, g.containsRoom, COL_TEXT);
        lv_obj_add_event_cb(btn, on_group_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (groups.empty()) {
        lv_obj_t *l = lv_list_add_text(list_rooms, "Ingen rum fundet");
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
    }
    lv_obj_t *wifi = lv_list_add_button(list_rooms, LV_SYMBOL_WIFI, "Wi-Fi-opsætning");
    style_list_button(wifi, false, COL_MUTED);
    lv_obj_add_event_cb(wifi, on_wifi_setup_clicked, LV_EVENT_CLICKED, nullptr);
}

void ui_update_queue(const std::vector<QueueItem> &items, int total, int currentNo) {
    lv_obj_clean(list_queue);
    if (total > 0) lv_label_set_text_fmt(lbl_queue_hdr, "Kø  %d/%d", currentNo, total);
    else lv_label_set_text(lbl_queue_hdr, "Kø");
    if (items.empty()) {
        lv_obj_t *l = lv_list_add_text(list_queue, total == 0 ? "Køen er tom" : "Kunne ikke hente køen");
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
        return;
    }
    lv_obj_t *current_btn = nullptr;
    for (const QueueItem &q : items) {
        bool cur = q.number == currentNo;
        lv_obj_t *btn = lv_button_create(list_queue);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 50);
        style_list_button(btn, cur, COL_TEXT);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_queue_item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)q.number);

        lv_obj_t *num = lv_label_create(btn);
        lv_label_set_text_fmt(num, cur ? LV_SYMBOL_PLAY : "%d", q.number);
        lv_obj_set_style_text_font(num, &font_dk_12, 0);
        lv_obj_set_style_text_color(num, COL_MUTED, 0);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *title = lv_label_create(btn);
        lv_label_set_text(title, q.title.c_str());
        lv_obj_set_style_text_font(title, &font_dk_14, 0);
        lv_obj_set_width(title, 170);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, -2);

        lv_obj_t *artist = lv_label_create(btn);
        lv_label_set_text(artist, q.artist.c_str());
        lv_obj_set_style_text_font(artist, &font_dk_12, 0);
        lv_obj_set_style_text_color(artist, COL_MUTED, 0);
        lv_obj_set_width(artist, 170);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
        lv_obj_align(artist, LV_ALIGN_BOTTOM_LEFT, 30, 2);
        if (cur) current_btn = btn;
    }
    if (current_btn) lv_obj_scroll_to_view(current_btn, LV_ANIM_OFF);
}

void ui_set_art(bool valid) {
    if (!art_pixels) return;
    if (!valid) {
        if (art_visible) {
            lv_obj_add_flag(img_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
            art_visible = false;
        }
        return;
    }
    lv_image_cache_drop(&art_dsc);
    lv_image_set_src(img_art, &art_dsc);
    lv_obj_invalidate(img_art);
    if (!art_visible) {
        lv_obj_remove_flag(img_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        art_visible = true;
    }
}

void ui_apply_rotation() {
    // The panel itself is rotated by LovyanGFX (lcd.setRotation); LVGL just needs a full redraw.
    lv_obj_invalidate(lv_screen_active());
}

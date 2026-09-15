#include "ui.h"
#include "display.h"
#include "board.h"
#include "app_config.h"
#include "player.h"
#include "file_list.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

LV_FONT_DECLARE(cjk16);  /* GB2312 中文字库,见 cjk16.c */

static const char *TAG = "UI";

static lv_obj_t *s_title_label    = NULL;
static lv_obj_t *s_state_label    = NULL;
static lv_obj_t *s_song_label     = NULL;
static lv_obj_t *s_vol_label     = NULL;
static lv_obj_t *s_vol_bar        = NULL;
static lv_obj_t *s_progress_bar   = NULL;
static lv_obj_t *s_time_label     = NULL;

/* Full-screen song list overlay */
static lv_obj_t *s_list_scr       = NULL;  /* full-screen container */
static lv_obj_t *s_list_title     = NULL;
static lv_obj_t *s_list_widget    = NULL;

static size_t s_list_selected = 0;
static bool   s_list_visible  = false;

static const char *state_to_str(player_state_t s)
{
    switch (s) {
    case PLAYER_STATE_PLAYING:  return ">> PLAYING";
    case PLAYER_STATE_PAUSED:   return "|| PAUSED";
    case PLAYER_STATE_STOPPED:
    default:                    return "[] STOPPED";
    }
}

static void fmt_time(int sec, char *buf, size_t buf_size)
{
    if (sec < 0) {
        snprintf(buf, buf_size, "--:--");
    } else {
        int m = sec / 60;
        int s = sec % 60;
        snprintf(buf, buf_size, "%02d:%02d", m, s);
    }
}

/* Extract file basename and (optionally) strip the .mp3 extension. */
static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void display_name(const char *path, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s", basename_of(path));
    size_t len = strlen(buf);
    if (len >= 4 &&
        (buf[len - 4] == '.') &&
        (buf[len - 3] == 'm' || buf[len - 3] == 'M') &&
        (buf[len - 2] == 'p' || buf[len - 2] == 'P') &&
        (buf[len - 1] == '3')) {
        buf[len - 4] = '\0';
    }
}

static void refresh_file_list(void)
{
    if (!s_list_widget) return;
    lv_obj_clean(s_list_widget);
    size_t total = file_list_count();
    for (size_t i = 0; i < total; i++) {
        const char *path = file_list_get(i);
        if (!path) continue;
        char name[MAX_FILE_PATH_LEN];
        display_name(path, name, sizeof(name));
        /* Prefix with marker so selection is visible even if bg style fails */
        char display[MAX_FILE_PATH_LEN + 4];
        if (i == s_list_selected) {
            snprintf(display, sizeof(display), "> %s", name);
        } else {
            snprintf(display, sizeof(display), "  %s", name);
        }
        lv_obj_t *btn = lv_list_add_button(s_list_widget, NULL, display);
        /* Set font on the child label (where text actually lives) */
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_font(label, &cjk16, 0);
            if (i == s_list_selected) {
                lv_obj_set_style_text_color(label, lv_color_make(0x00, 0xFF, 0x00), 0);
            }
        }
        if (i == s_list_selected) {
            /* Bright blue background for the selected row */
            lv_obj_set_style_bg_color(btn, lv_color_make(0x00, 0x80, 0xFF), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_radius(btn, 0, 0);
        }
    }
    /* Scroll the list so the selected item is visible */
    uint32_t child_cnt = lv_obj_get_child_count(s_list_widget);
    if (s_list_selected < child_cnt) {
        lv_obj_t *sel_btn = lv_obj_get_child(s_list_widget, s_list_selected);
        if (sel_btn) {
            lv_obj_scroll_to_view(sel_btn, LV_ANIM_OFF);
        }
    }
}

/* ── Playback screen widgets ── */

static void hide_playback_widgets(void)
{
    lv_obj_add_flag(s_title_label,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_state_label,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_song_label,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_vol_label,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_vol_bar,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_progress_bar,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_label,    LV_OBJ_FLAG_HIDDEN);
}

static void show_playback_widgets(void)
{
    lv_obj_clear_flag(s_title_label,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_state_label,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_song_label,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_vol_label,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_vol_bar,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_progress_bar,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_time_label,    LV_OBJ_FLAG_HIDDEN);
}

/* ── Public API ── */

esp_err_t ui_init(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(NULL);

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* Title at top. */
    s_title_label = lv_label_create(scr);
    lv_label_set_text(s_title_label, "MP3 Player");
    lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 10);

    /* State label. */
    s_state_label = lv_label_create(scr);
    lv_label_set_text(s_state_label, state_to_str(player_get_state()));
    lv_obj_set_style_text_color(s_state_label, lv_color_make(0x80, 0xFF, 0x80), 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 40);

    /* Song name label. */
    s_song_label = lv_label_create(scr);
    lv_label_set_text(s_song_label, "No file");
    lv_obj_set_style_text_color(s_song_label, lv_color_make(0xC0, 0xC0, 0xFF), 0);
    lv_obj_set_style_text_font(s_song_label, &cjk16, 0);
    lv_label_set_long_mode(s_song_label, LV_LABEL_LONG_SCROLL);
    lv_obj_set_width(s_song_label, LCD_H_RES - 20);
    lv_obj_align(s_song_label, LV_ALIGN_TOP_MID, 0, 75);

    /* Progress bar. */
    s_progress_bar = lv_bar_create(scr);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_size(s_progress_bar, 200, 15);
    lv_obj_align(s_progress_bar, LV_ALIGN_TOP_MID, 0, 115);

    /* Time label. */
    s_time_label = lv_label_create(scr);
    lv_label_set_text(s_time_label, "00:00 / --:--");
    lv_obj_set_style_text_color(s_time_label, lv_color_make(0xFF, 0xFF, 0x80), 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 140);

    /* Volume label. */
    s_vol_label = lv_label_create(scr);
    lv_label_set_text_fmt(s_vol_label, "Vol: %d%%", player_get_volume());
    lv_obj_set_style_text_color(s_vol_label, lv_color_make(0xFF, 0xFF, 0x80), 0);
    lv_obj_align(s_vol_label, LV_ALIGN_TOP_LEFT, 20, 180);

    /* Volume bar. */
    s_vol_bar = lv_bar_create(scr);
    lv_bar_set_range(s_vol_bar, 0, 100);
    lv_bar_set_value(s_vol_bar, player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_size(s_vol_bar, 200, 20);
    lv_obj_align(s_vol_bar, LV_ALIGN_TOP_LEFT, 20, 205);

    /* ── Full-screen song list overlay (hidden until long-press) ── */
    s_list_scr = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list_scr);
    lv_obj_set_style_bg_color(s_list_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_list_scr, LV_OPA_COVER, 0);
    lv_obj_set_size(s_list_scr, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_list_scr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_list_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_list_scr, LV_OBJ_FLAG_HIDDEN);

    /* List title */
    s_list_title = lv_label_create(s_list_scr);
    lv_label_set_text(s_list_title, "Song List");
    lv_obj_set_style_text_color(s_list_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_list_title, &cjk16, 0);
    lv_obj_align(s_list_title, LV_ALIGN_TOP_MID, 0, 6);

    /* List widget (fills most of the screen) */
    s_list_widget = lv_list_create(s_list_scr);
    lv_obj_set_size(s_list_widget, LCD_H_RES - 8, LCD_V_RES - 50);
    lv_obj_align(s_list_widget, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_text_font(s_list_widget, &cjk16, 0);

    refresh_file_list();

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI initialized");
    return ESP_OK;
}

void ui_update(void)
{
    if (!s_state_label) return;
    if (s_list_visible) return;  /* Don't update playback widgets while list is showing */
    if (!lvgl_port_lock(50)) return;

    lv_label_set_text(s_state_label, state_to_str(player_get_state()));
    lv_label_set_text_fmt(s_vol_label, "Vol: %d%%", player_get_volume());
    lv_bar_set_value(s_vol_bar, player_get_volume(), LV_ANIM_OFF);

    const char *cur = player_get_current_file();
    if (cur && cur[0]) {
        char name[MAX_FILE_PATH_LEN];
        display_name(cur, name, sizeof(name));
        lv_label_set_text(s_song_label, name);
    } else {
        lv_label_set_text(s_song_label, "No file");
    }

    int played = player_get_played_sec();
    int total = player_get_total_sec();
    char time_buf[32];
    if (total > 0) {
        int pct = total > 0 ? (played * 100) / total : 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
        char tot_buf[16];
        fmt_time(total, tot_buf, sizeof(tot_buf));
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d / %s", played / 60, played % 60, tot_buf);
    } else {
        int pct = (played * 100) / 180;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d / --:--", played / 60, played % 60);
    }
    lv_label_set_text(s_time_label, time_buf);

    lvgl_port_unlock();
}

void ui_list_enter(void)
{
    if (!s_list_scr) return;
    if (!lvgl_port_lock(50)) return;
    /* Start selection on the currently playing file */
    s_list_selected = file_list_current_index();
    refresh_file_list();
    hide_playback_widgets();
    lv_obj_clear_flag(s_list_scr, LV_OBJ_FLAG_HIDDEN);
    s_list_visible = true;
    lvgl_port_unlock();
}

void ui_list_exit(void)
{
    if (!s_list_scr) return;
    if (!lvgl_port_lock(50)) return;
    lv_obj_add_flag(s_list_scr, LV_OBJ_FLAG_HIDDEN);
    show_playback_widgets();
    s_list_visible = false;
    lvgl_port_unlock();
}

bool ui_list_is_visible(void)
{
    return s_list_visible;
}

void ui_list_move_up(void)
{
    size_t total = file_list_count();
    if (total == 0) return;
    if (s_list_selected == 0) s_list_selected = total - 1;
    else s_list_selected--;
    /* Refresh highlight */
    if (s_list_visible && lvgl_port_lock(50)) {
        refresh_file_list();
        lvgl_port_unlock();
    }
}

void ui_list_move_down(void)
{
    size_t total = file_list_count();
    if (total == 0) return;
    s_list_selected = (s_list_selected + 1) % total;
    /* Refresh highlight */
    if (s_list_visible && lvgl_port_lock(50)) {
        refresh_file_list();
        lvgl_port_unlock();
    }
}

size_t ui_list_get_selected(void)
{
    return s_list_selected;
}

#include "focus_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define C_BG      0x0B0D12
#define C_TRACK   0x1E222C
#define C_MUTED   0x8A90A2
#define C_TEXT    0xF2F4F8
#define C_IDLE    0x9AA3B8
#define C_FOCUS   0xFF6B4A
#define C_PAUSED  0xF5B83D
#define C_DONE    0x4ADE80
#define C_BREAK   0x4AA8FF

static ui_callbacks_t s_cb;

static lv_obj_t *s_title;
static lv_obj_t *s_arc;
static lv_obj_t *s_time;
static lv_obj_t *s_hint;
static lv_obj_t *s_presets;
static lv_obj_t *s_end_btn;
static lv_obj_t *s_end_lbl;
static lv_obj_t *s_stats;

static fc_state_t s_last_state = (fc_state_t)-1;
static uint32_t s_last_preset = UINT32_MAX;
static int32_t s_last_arc = -1;
static char s_time_txt[8];
static char s_stats_txt[64];

static void preset_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id != LV_BUTTONMATRIX_BUTTON_NONE && s_cb.on_preset) s_cb.on_preset(id);
}

static void end_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb.on_end) s_cb.on_end();
}

void focus_ui_create(const char *const *preset_labels, const ui_callbacks_t *cb)
{
    s_cb = *cb;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(s_title, 4, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 22);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 300, 300);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, -18);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_TRACK), LV_PART_MAIN);

    s_time = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(C_TEXT), 0);
    lv_obj_align_to(s_time, s_arc, LV_ALIGN_CENTER, 0, -10);

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hint, 220);
    lv_obj_align_to(s_hint, s_arc, LV_ALIGN_CENTER, 0, 42);

    s_presets = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(s_presets, (const char **)preset_labels);
    lv_buttonmatrix_set_button_ctrl_all(s_presets, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_presets, true);
    lv_obj_set_size(s_presets, 360, 52);
    lv_obj_align(s_presets, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_set_style_bg_opa(s_presets, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_presets, 0, 0);
    lv_obj_set_style_pad_all(s_presets, 4, 0);
    lv_obj_set_style_pad_gap(s_presets, 10, 0);
    lv_obj_set_style_bg_color(s_presets, lv_color_hex(C_TRACK), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_presets, lv_color_hex(C_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_presets, 26, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_presets, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_presets, lv_color_hex(C_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_presets, lv_color_hex(C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_presets, preset_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_end_btn = lv_button_create(scr);
    lv_obj_set_size(s_end_btn, 200, 52);
    lv_obj_align(s_end_btn, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_set_style_radius(s_end_btn, 26, 0);
    lv_obj_set_style_bg_color(s_end_btn, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_shadow_width(s_end_btn, 0, 0);
    lv_obj_add_event_cb(s_end_btn, end_cb, LV_EVENT_CLICKED, NULL);
    s_end_lbl = lv_label_create(s_end_btn);
    lv_obj_set_style_text_color(s_end_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(s_end_lbl);

    s_stats = lv_label_create(scr);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_stats, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_stats, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static void show(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_state(fc_state_t st)
{
    const char *title = "";
    const char *hint = "";
    uint32_t color = C_IDLE;
    switch (st) {
    case FC_IDLE:
        title = "READY";
        hint = "Flip face-down to start";
        color = C_IDLE;
        break;
    case FC_FOCUS:
        title = "FOCUSING";
        hint = "Screen off. Stay focused.";
        color = C_FOCUS;
        break;
    case FC_PAUSED:
        title = "PAUSED";
        hint = "Flip face-down to resume";
        color = C_PAUSED;
        break;
    case FC_DONE:
        title = "DONE";
        hint = "Pick me up for your break";
        color = C_DONE;
        break;
    case FC_BREAK:
        title = "BREAK";
        hint = "Flip face-down to start the next one";
        color = C_BREAK;
        break;
    }
    lv_label_set_text(s_title, title);
    lv_obj_set_style_text_color(s_title, lv_color_hex(color), 0);
    lv_label_set_text(s_hint, hint);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(color), LV_PART_INDICATOR);

    show(s_presets, st == FC_IDLE);
    show(s_end_btn, st == FC_PAUSED || st == FC_BREAK);
    lv_label_set_text(s_end_lbl, st == FC_BREAK ? "Skip break" : LV_SYMBOL_STOP "  End session");
}

void focus_ui_update(const ui_model_t *m)
{
    if (m->state != s_last_state) {
        apply_state(m->state);
        s_last_state = m->state;
    }

    /* Only touch widgets whose content changed: every invalidation costs
     * render time and battery on the device. */
    char buf[64];

    /* round up so "25:00" shows for the whole first second */
    uint32_t sec = (m->remaining_ms + 999) / 1000;
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    if (strcmp(buf, s_time_txt) != 0) {
        strcpy(s_time_txt, buf);
        lv_label_set_text(s_time, s_time_txt);
    }

    int32_t arc = m->state == FC_IDLE ? 0 : (int32_t)m->progress_permille;
    if (arc != s_last_arc) {
        s_last_arc = arc;
        lv_arc_set_value(s_arc, arc);
    }

    if (m->preset_idx != s_last_preset) {
        s_last_preset = m->preset_idx;
        lv_buttonmatrix_set_button_ctrl(s_presets, m->preset_idx, LV_BUTTONMATRIX_CTRL_CHECKED);
    }

    uint32_t min = m->stats->focus_ms / 60000u;
    snprintf(buf, sizeof(buf), "Today  %u done  " LV_SYMBOL_BULLET "  %uh %02um focused",
             (unsigned)m->stats->sessions, (unsigned)(min / 60), (unsigned)(min % 60));
    if (strcmp(buf, s_stats_txt) != 0) {
        strcpy(s_stats_txt, buf);
        lv_label_set_text(s_stats, s_stats_txt);
    }
}

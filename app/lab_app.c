/*
 * Sensor Lab — exercise every on-board sensor of the ESP-Mosaico.
 *
 *   Level    BMI270 accelerometer  tilt, pose, bubble level
 *   Compass  2x BMM150             tilt-compensated heading, interference
 *   Gyro     BMI270 gyroscope      rates, integrated turns, shake gesture
 *   Sound    mic + ES8311          dBFS meter, clap detection
 *   Power    BQ27220               charge, current, time left
 *   I/O      CST9220 touch, motor, speaker, FN button
 *
 * The FN button jumps to the next page.
 */
#include "lab_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "focus_core.h"
#include "hal.h"
#include "lvgl.h"
#include "sensors.h"

#define TICK_MS 50

#define C_BG     0x0B0D12
#define C_PANEL  0x151922
#define C_TRACK  0x1E222C
#define C_MUTED  0x8A90A2
#define C_TEXT   0xF2F4F8
#define C_GOOD   0x4ADE80
#define C_WARN   0xF5B83D
#define C_HOT    0xFF6B4A
#define C_COOL   0x4AA8FF

enum { PG_LEVEL, PG_COMPASS, PG_GYRO, PG_SOUND, PG_POWER, PG_IO, PG_COUNT };

static lv_obj_t *s_tv;

/* algorithm state (always running, whatever page is shown) */
static sx_shake_t s_shake;
static sx_yaw_t s_yaw;
static sx_clap_t s_clap;
static uint32_t s_shakes, s_claps;
static uint32_t s_shake_at, s_clap_at;
static bool s_fn_was_down;
static float s_peak_db = -90;
static uint32_t s_peak_at;
static uint32_t s_power_sample_at;

/* ---------- small helpers --------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *txt)
{
    /* skip identical text: no invalidation, no redraw */
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(l, txt);
}

static lv_obj_t *page(const char *tab, const char *chip)
{
    lv_obj_t *p = lv_tabview_add_tab(s_tv, tab);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_t *t = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(t, chip);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
    return p;
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t d, uint32_t border)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *dot(lv_obj_t *parent, int32_t d, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *chart(lv_obj_t *parent, int32_t w, int32_t h, uint32_t points, int32_t lo, int32_t hi,
                       uint32_t color, lv_chart_series_t **ser)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_size(c, w, h);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, points);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_line_color(c, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR); /* no point markers */
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    *ser = lv_chart_add_series(c, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    for (uint32_t i = 0; i < points; i++) lv_chart_set_next_value(c, *ser, lo);
    return c;
}

/* ====================================================================== */
/* Level — accelerometer                                                   */

static struct {
    lv_obj_t *bubble, *angles, *pose;
} L;

#define LEVEL_RING 300
#define LEVEL_BUBBLE 52
#define LEVEL_MAX_DEG 30.0f

static void level_create(lv_obj_t *p)
{
    lv_obj_t *ring = circle(p, LEVEL_RING, C_TRACK);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *target = circle(ring, 70, C_MUTED);
    lv_obj_center(target);

    L.bubble = dot(ring, LEVEL_BUBBLE, C_WARN);
    lv_obj_set_style_bg_opa(L.bubble, LV_OPA_80, 0);
    lv_obj_center(L.bubble);

    L.angles = label(p, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(L.angles, LV_ALIGN_BOTTOM_MID, 0, -34);
    L.pose = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(L.pose, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void level_update(sx_vec3_t a)
{
    float pitch, roll;
    sx_tilt_deg(a, &pitch, &roll);

    /* the bubble floats to the high side */
    float k = (LEVEL_RING / 2 - LEVEL_BUBBLE / 2 - 4) / LEVEL_MAX_DEG;
    float x = -LV_CLAMP(-LEVEL_MAX_DEG, roll, LEVEL_MAX_DEG) * k;
    float y = LV_CLAMP(-LEVEL_MAX_DEG, pitch, LEVEL_MAX_DEG) * k;
    lv_obj_align(L.bubble, LV_ALIGN_CENTER, (int32_t)x, (int32_t)y);

    bool level = fabsf(pitch) < 1.0f && fabsf(roll) < 1.0f;
    lv_obj_set_style_bg_color(L.bubble, lv_color_hex(level ? C_GOOD : C_WARN), 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "Pitch %+.1f\xC2\xB0    Roll %+.1f\xC2\xB0", (double)pitch, (double)roll);
    set_text(L.angles, buf);

    static const char *const poses[] = {"No reading", "Face up", "Face down", "Tilted / in hand"};
    fc_pose_t pose = fc_classify_pose(a.x, a.y, a.z);
    snprintf(buf, sizeof(buf), "%s%s", poses[pose], level ? "  " LV_SYMBOL_OK " level" : "");
    set_text(L.pose, buf);
}

/* ====================================================================== */
/* Compass — two magnetometers                                             */

static struct {
    lv_obj_t *dial, *cards[4], *heading, *cardinal, *detail, *warn;
} C;

#define DIAL 300

static void compass_create(lv_obj_t *p)
{
    C.dial = circle(p, DIAL, C_TRACK);
    lv_obj_align(C.dial, LV_ALIGN_CENTER, 0, -14);

    /* fixed marker: the direction the top edge points to */
    lv_obj_t *mark = dot(p, 10, C_TEXT);
    lv_obj_set_size(mark, 6, 26);
    lv_obj_set_style_radius(mark, 3, 0);
    lv_obj_align_to(mark, C.dial, LV_ALIGN_TOP_MID, 0, -8);

    static const char *const names[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; i++) {
        C.cards[i] = label(C.dial, &lv_font_montserrat_20, i == 0 ? C_HOT : C_MUTED);
        lv_label_set_text(C.cards[i], names[i]);
    }

    C.heading = label(C.dial, &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(C.heading, LV_ALIGN_CENTER, 0, -12);
    C.cardinal = label(C.dial, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(C.cardinal, LV_ALIGN_CENTER, 0, 30);

    C.detail = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(C.detail, LV_ALIGN_BOTTOM_MID, 0, -34);
    C.warn = label(p, &lv_font_montserrat_14, C_WARN);
    lv_obj_align(C.warn, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void compass_update(sx_vec3_t a, sx_vec3_t m0, sx_vec3_t m1)
{
    sx_vec3_t avg = {(m0.x + m1.x) / 2, (m0.y + m1.y) / 2, (m0.z + m1.z) / 2};
    float h = sx_heading_deg(a, avg);
    float h0 = sx_heading_deg(a, m0), h1 = sx_heading_deg(a, m1);
    float diff = sx_mag_disagreement(m0, m1);
    bool disturbed = diff > SX_MAG_INTERFERENCE_UT;

    /* rotate the card so N points to magnetic north */
    float r = DIAL / 2 - 26;
    for (int i = 0; i < 4; i++) {
        float ang = ((float)i * 90.0f - h) * 3.14159265f / 180.0f;
        lv_obj_align(C.cards[i], LV_ALIGN_CENTER, (int32_t)(sinf(ang) * r), (int32_t)(-cosf(ang) * r));
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(h) % 360);
    set_text(C.heading, buf);
    set_text(C.cardinal, disturbed ? "unreliable" : sx_cardinal(h));
    lv_obj_set_style_text_color(C.heading, lv_color_hex(disturbed ? C_WARN : C_TEXT), 0);

    snprintf(buf, sizeof(buf), "Sensor A %.0f\xC2\xB0   Sensor B %.0f\xC2\xB0   diff %.1f uT", (double)h0, (double)h1,
             (double)diff);
    set_text(C.detail, buf);
    set_text(C.warn, disturbed ? LV_SYMBOL_WARNING " Magnet or metal nearby: sensors disagree" : "");
}

/* ====================================================================== */
/* Gyro — rates, turns, shake                                              */

static struct {
    lv_obj_t *bars[3], *vals[3], *turns, *shake;
} G;

static void gyro_create(lv_obj_t *p)
{
    static const char *const axes[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        int32_t y = 50 + i * 48;
        lv_obj_t *n = label(p, &lv_font_montserrat_20, C_MUTED);
        lv_label_set_text(n, axes[i]);
        lv_obj_set_pos(n, 34, y);

        G.bars[i] = lv_bar_create(p);
        lv_bar_set_mode(G.bars[i], LV_BAR_MODE_SYMMETRICAL);
        lv_bar_set_range(G.bars[i], -360, 360);
        lv_obj_set_size(G.bars[i], 260, 16);
        lv_obj_set_pos(G.bars[i], 64, y + 4);
        lv_obj_set_style_bg_color(G.bars[i], lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_bg_color(G.bars[i], lv_color_hex(C_COOL), LV_PART_INDICATOR);

        G.vals[i] = label(p, &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_pos(G.vals[i], 340, y + 3);
    }

    G.turns = label(p, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(G.turns, LV_ALIGN_CENTER, 0, 70);

    G.shake = label(p, &lv_font_montserrat_28, C_MUTED);
    lv_obj_align(G.shake, LV_ALIGN_BOTTOM_MID, 0, -26);
}

static void gyro_update(sx_vec3_t g, uint32_t now)
{
    float v[3] = {g.x, g.y, g.z};
    char buf[64];
    for (int i = 0; i < 3; i++) {
        lv_bar_set_value(G.bars[i], (int32_t)LV_CLAMP(-360, v[i], 360), LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%+6.1f deg/s", (double)v[i]);
        set_text(G.vals[i], buf);
    }
    int32_t turns = sx_yaw_turns(&s_yaw);
    snprintf(buf, sizeof(buf), "%d turn%s %s   (yaw %+.0f\xC2\xB0)", (int)(turns < 0 ? -turns : turns),
             (turns == 1 || turns == -1) ? "" : "s", turns < 0 ? "clockwise" : "counter-cw", (double)s_yaw.yaw_deg);
    set_text(G.turns, buf);

    bool flash = s_shakes && now - s_shake_at < 1200;
    snprintf(buf, sizeof(buf), flash ? "SHAKE!  (%u)" : "Shakes: %u", (unsigned)s_shakes);
    set_text(G.shake, buf);
    lv_obj_set_style_text_color(G.shake, lv_color_hex(flash ? C_HOT : C_MUTED), 0);
}

/* ====================================================================== */
/* Sound — microphone level                                                */

static struct {
    lv_obj_t *db, *cls, *bar, *peak, *claps;
    lv_obj_t *chart;
    lv_chart_series_t *ser;
} S;

#define METER_W 400
#define METER_MIN_DB (-70)

static void sound_create(lv_obj_t *p)
{
    S.db = label(p, &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(S.db, LV_ALIGN_TOP_MID, 0, 40);
    S.cls = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(S.cls, LV_ALIGN_TOP_MID, 0, 100);

    S.bar = lv_bar_create(p);
    lv_bar_set_range(S.bar, METER_MIN_DB, 0);
    lv_obj_set_size(S.bar, METER_W, 20);
    lv_obj_align(S.bar, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(S.bar, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_anim_duration(S.bar, 0, 0);

    S.peak = dot(S.bar, 4, C_TEXT);
    lv_obj_set_size(S.peak, 3, 20);
    lv_obj_set_style_radius(S.peak, 0, 0);

    S.chart = chart(p, METER_W, 140, 100, METER_MIN_DB, 0, C_COOL, &S.ser);
    lv_obj_align(S.chart, LV_ALIGN_TOP_MID, 0, 168);

    S.claps = label(p, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(S.claps, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void sound_update(float db, uint32_t now)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%.0f dBFS", (double)db);
    set_text(S.db, buf);

    static const char *const names[] = {"Quiet", "Talking", "Loud"};
    static const uint32_t colors[] = {C_GOOD, C_COOL, C_HOT};
    sx_sound_class_t cls = sx_sound_class(db);
    set_text(S.cls, names[cls]);
    lv_obj_set_style_bg_color(S.bar, lv_color_hex(colors[cls]), LV_PART_INDICATOR);
    lv_bar_set_value(S.bar, (int32_t)LV_MAX(db, METER_MIN_DB), LV_ANIM_OFF);

    float peak = LV_MAX(s_peak_db, METER_MIN_DB);
    lv_obj_set_x(S.peak, (int32_t)((peak - METER_MIN_DB) / -METER_MIN_DB * (METER_W - 3)));

    bool flash = s_claps && now - s_clap_at < 800;
    snprintf(buf, sizeof(buf), flash ? "CLAP!  (%u)" : "Claps: %u", (unsigned)s_claps);
    set_text(S.claps, buf);
    lv_obj_set_style_text_color(S.claps, lv_color_hex(flash ? C_HOT : C_MUTED), 0);
}

/* ====================================================================== */
/* Power — fuel gauge                                                      */

static struct {
    lv_obj_t *pct, *bar, *rows, *left;
    lv_obj_t *chart;
    lv_chart_series_t *ser;
} P;

static void power_create(lv_obj_t *p)
{
    P.pct = label(p, &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(P.pct, LV_ALIGN_TOP_MID, 0, 36);

    P.bar = lv_bar_create(p);
    lv_bar_set_range(P.bar, 0, 100);
    lv_obj_set_size(P.bar, 300, 18);
    lv_obj_align(P.bar, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_color(P.bar, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_anim_duration(P.bar, 0, 0);

    P.rows = label(p, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_style_text_align(P.rows, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(P.rows, LV_ALIGN_TOP_MID, 0, 132);

    P.left = label(p, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(P.left, LV_ALIGN_TOP_MID, 0, 160);

    P.chart = chart(p, 400, 120, 60, 0, 200, C_WARN, &P.ser);
    lv_obj_align(P.chart, LV_ALIGN_TOP_MID, 0, 200);

    lv_obj_t *note = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(note, "Current draw, last 60 s (0-200 mA)");
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -40);
}

static void power_update(const hal_battery_t *b, uint32_t now)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%.0f%%", b->charging ? LV_SYMBOL_CHARGE " " : "", (double)b->soc_pct);
    set_text(P.pct, buf);
    lv_bar_set_value(P.bar, (int32_t)b->soc_pct, LV_ANIM_OFF);
    uint32_t col = b->charging ? C_GOOD : b->soc_pct < 20 ? C_HOT : C_TEXT;
    lv_obj_set_style_bg_color(P.bar, lv_color_hex(col), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%.2f V    %s %.0f mA    %.0f mAh cell", (double)(b->voltage_mv / 1000.0f),
             b->current_ma >= 0 ? "draw" : "charge", (double)fabsf(b->current_ma),
             (double)HAL_BATTERY_CAPACITY_MAH);
    set_text(P.rows, buf);

    uint32_t min = sx_battery_minutes_left(b->soc_pct, HAL_BATTERY_CAPACITY_MAH, b->current_ma);
    if (min == UINT32_MAX) {
        snprintf(buf, sizeof(buf), "Charging");
    } else {
        snprintf(buf, sizeof(buf), "%uh %02um left at this rate", (unsigned)(min / 60), (unsigned)(min % 60));
    }
    set_text(P.left, buf);

    if (now - s_power_sample_at >= 1000) {
        s_power_sample_at = now;
        lv_chart_set_next_value(P.chart, P.ser, (int32_t)LV_CLAMP(0, b->current_ma, 200));
    }
}

/* ====================================================================== */
/* I/O — touch, motor, speaker, FN button                                  */

static struct {
    lv_obj_t *pad, *finger, *coords, *fn;
} IO;

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    char buf[64];

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_area_t a;
        lv_obj_get_coords(IO.pad, &a);
        lv_obj_remove_flag(IO.finger, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(IO.finger, pt.x - a.x1 - 14, pt.y - a.y1 - 14);
        snprintf(buf, sizeof(buf), "Touch  x %d  y %d", (int)pt.x, (int)pt.y);
        set_text(IO.coords, buf);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_add_flag(IO.finger, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_GESTURE) {
        static const char *names[] = {"", "left", "right", "", "up", "", "", "", "down"};
        lv_dir_t d = lv_indev_get_gesture_dir(indev);
        if (d < sizeof(names) / sizeof(names[0]) && names[d][0]) {
            snprintf(buf, sizeof(buf), "Swipe %s", names[d]);
            set_text(IO.coords, buf);
        }
    }
}

static void vibrate_cb(lv_event_t *e)
{
    (void)e;
    hal_vibrate(400);
}

static void beep_cb(lv_event_t *e)
{
    (void)e;
    hal_sound(HAL_SOUND_DONE);
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 140, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, &lv_font_montserrat_14, C_TEXT);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void io_create(lv_obj_t *p)
{
    IO.pad = lv_obj_create(p);
    lv_obj_remove_style_all(IO.pad);
    lv_obj_set_size(IO.pad, 420, 190);
    lv_obj_align(IO.pad, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_radius(IO.pad, 14, 0);
    lv_obj_set_style_bg_opa(IO.pad, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(IO.pad, lv_color_hex(C_PANEL), 0);
    lv_obj_remove_flag(IO.pad, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(IO.pad, pad_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *hint = label(IO.pad, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(hint, "Touch or swipe here");
    lv_obj_center(hint);

    IO.finger = dot(IO.pad, 28, C_COOL);
    lv_obj_set_style_bg_opa(IO.finger, LV_OPA_70, 0);
    lv_obj_add_flag(IO.finger, LV_OBJ_FLAG_HIDDEN);

    IO.coords = label(p, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_text(IO.coords, "Touch  -");
    lv_obj_align(IO.coords, LV_ALIGN_TOP_MID, 0, 238);

    lv_obj_t *b1 = button(p, LV_SYMBOL_BELL "  Vibrate", vibrate_cb);
    lv_obj_align(b1, LV_ALIGN_TOP_MID, -78, 284);
    lv_obj_t *b2 = button(p, LV_SYMBOL_VOLUME_MAX "  Beep", beep_cb);
    lv_obj_align(b2, LV_ALIGN_TOP_MID, 78, 284);

    IO.fn = label(p, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(IO.fn, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void io_update(bool fn_down)
{
    set_text(IO.fn, fn_down ? "FN button: PRESSED" : "FN button: released  (press = next page)");
    lv_obj_set_style_text_color(IO.fn, lv_color_hex(fn_down ? C_GOOD : C_MUTED), 0);
}

/* ====================================================================== */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = hal_time_ms();

    sx_vec3_t a = {0, 0, 0}, g = {0, 0, 0}, m0 = {0, 0, 0}, m1 = {0, 0, 0};
    hal_imu_read(&a.x, &a.y, &a.z);
    hal_gyro_read(&g.x, &g.y, &g.z);
    hal_mag_read(0, &m0.x, &m0.y, &m0.z);
    hal_mag_read(1, &m1.x, &m1.y, &m1.z);
    float db = sx_dbfs(hal_mic_rms());
    hal_battery_t bat = {0};
    hal_battery_read(&bat);
    bool fn = hal_fn_button();

    /* detectors run regardless of the visible page */
    sx_yaw_update(&s_yaw, g.z, now);
    if (sx_shake_update(&s_shake, a, now)) {
        s_shakes++;
        s_shake_at = now;
        hal_vibrate(150);
    }
    if (sx_clap_update(&s_clap, db, now)) {
        s_claps++;
        s_clap_at = now;
    }
    /* peak hold 1.5 s, then fall 20 dB/s */
    if (db >= s_peak_db) {
        s_peak_db = db;
        s_peak_at = now;
    } else if (now - s_peak_at > 1500) {
        s_peak_db -= 20.0f * TICK_MS / 1000.0f;
    }
    lv_chart_set_next_value(S.chart, S.ser, (int32_t)LV_MAX(db, METER_MIN_DB));

    if (fn && !s_fn_was_down) {
        uint32_t next = (lv_tabview_get_tab_active(s_tv) + 1) % PG_COUNT;
        lv_tabview_set_active(s_tv, next, LV_ANIM_ON);
    }
    s_fn_was_down = fn;

    /* only the visible page touches its widgets */
    switch (lv_tabview_get_tab_active(s_tv)) {
    case PG_LEVEL: level_update(a); break;
    case PG_COMPASS: compass_update(a, m0, m1); break;
    case PG_GYRO: gyro_update(g, now); break;
    case PG_SOUND: sound_update(db, now); break;
    case PG_POWER: power_update(&bat, now); break;
    case PG_IO: io_update(fn); break;
    }
    if (lv_tabview_get_tab_active(s_tv) != PG_POWER && now - s_power_sample_at >= 1000) {
        /* keep the current-draw history going in the background */
        s_power_sample_at = now;
        lv_chart_set_next_value(P.chart, P.ser, (int32_t)LV_CLAMP(0, bat.current_ma, 200));
    }
}

void lab_app_start(void)
{
    sx_shake_init(&s_shake);
    sx_yaw_init(&s_yaw);
    sx_clap_init(&s_clap);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    s_tv = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_size(s_tv, 44);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(C_BG), 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_14, 0);
    /* pages change via tabs or the FN button; swiping is for the touch demo */
    lv_obj_remove_flag(lv_tabview_get_content(s_tv), LV_OBJ_FLAG_SCROLLABLE);

    level_create(page("Level", "BMI270 accelerometer"));
    compass_create(page("Compass", "2x BMM150 magnetometer"));
    gyro_create(page("Gyro", "BMI270 gyroscope"));
    sound_create(page("Sound", "Microphone + ES8311 codec"));
    power_create(page("Power", "BQ27220 fuel gauge"));
    io_create(page("I/O", "CST9220 touch / motor / speaker / FN"));

    lv_timer_create(tick_cb, TICK_MS, NULL);
}

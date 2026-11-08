/*
 * Browser simulator for ESP-Mosaico (480x480).
 *
 * LVGL renders into a full-frame buffer that JS copies to a <canvas>.
 * The HAL is implemented with small EM_JS hooks; JS feeds the fake IMU,
 * pointer and a time-scale knob (so a 25 min session can be tested fast).
 */
#include <emscripten.h>
#include <stdint.h>

#include "focus_app.h"
#include "hal.h"
#include "lvgl.h"

#define W 480
#define H 480

static uint32_t s_fb[W * H];
static int s_fb_dirty;

static float s_ax, s_ay, s_az = 1.0f;
static int s_ptr_x, s_ptr_y, s_ptr_down;
/* A tap shorter than LVGL's input poll period (~33 ms) would be lost;
 * latch the press until LVGL has seen it once. */
static int s_ptr_press_pending;

static double s_last_real_ms = -1;
static double s_sim_ms;
static double s_time_scale = 1.0;

/* ---------- LVGL drivers ---------------------------------------------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(disp)) s_fb_dirty = 1;
    lv_display_flush_ready(disp);
}

static void pointer_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_ptr_x;
    data->point.y = s_ptr_y;
    data->state = (s_ptr_down || s_ptr_press_pending) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_ptr_press_pending = 0;
}

/* ---------- API called from JS ---------------------------------------- */

EMSCRIPTEN_KEEPALIVE void web_init(void)
{
    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, pointer_cb);

    focus_app_start();
}

/* Called from requestAnimationFrame. Returns 1 if the frame changed. */
EMSCRIPTEN_KEEPALIVE int web_frame(double real_ms)
{
    if (s_last_real_ms < 0) s_last_real_ms = real_ms;
    double dt = real_ms - s_last_real_ms;
    s_last_real_ms = real_ms;
    if (dt < 0) dt = 0;
    if (dt > 250) dt = 250; /* tab was in background */

    /* Sub-step so the 50 ms app tick sees every simulated interval
     * (keeps debouncing realistic even at 60x speed). */
    double sim_dt = dt * s_time_scale;
    while (sim_dt > 0) {
        double step = sim_dt > 40 ? 40 : sim_dt;
        s_sim_ms += step;
        lv_tick_inc((uint32_t)(step + 0.5));
        lv_timer_handler();
        sim_dt -= step;
    }
    if (dt == 0) lv_timer_handler();

    int dirty = s_fb_dirty;
    s_fb_dirty = 0;
    return dirty;
}

EMSCRIPTEN_KEEPALIVE uint32_t *web_framebuffer(void) { return s_fb; }

EMSCRIPTEN_KEEPALIVE void web_set_accel(float x, float y, float z)
{
    s_ax = x;
    s_ay = y;
    s_az = z;
}

EMSCRIPTEN_KEEPALIVE void web_pointer(int x, int y, int down)
{
    s_ptr_x = x;
    s_ptr_y = y;
    if (down && !s_ptr_down) s_ptr_press_pending = 1;
    s_ptr_down = down;
}

EMSCRIPTEN_KEEPALIVE void web_set_time_scale(double k) { s_time_scale = k > 0 ? k : 1; }

/* ---------- HAL --------------------------------------------------------- */

uint32_t hal_time_ms(void) { return (uint32_t)(uint64_t)s_sim_ms; }

EM_JS(uint32_t, js_day_index, (void), {
    const d = new Date();
    return Math.floor((d.getTime() - d.getTimezoneOffset() * 60000) / 86400000);
});
uint32_t hal_day_index(void) { return js_day_index(); }

bool hal_imu_read(float *ax, float *ay, float *az)
{
    /* a little sensor noise so the debouncer is exercised */
    float n = ((float)(lv_rand(0, 2000)) - 1000.0f) / 50000.0f;
    *ax = s_ax + n;
    *ay = s_ay - n;
    *az = s_az + n;
    return true;
}

EM_JS(void, js_sound, (int id), { Module.sim && Module.sim.sound(id); });
void hal_sound(hal_sound_t s) { js_sound((int)s); }

EM_JS(void, js_led, (int id), { Module.sim && Module.sim.led(id); });
void hal_status_led(hal_led_t led) { js_led((int)led); }

EM_JS(void, js_awake, (int on), { Module.sim && Module.sim.awake(!!on); });
void hal_display_set_awake(bool awake) { js_awake(awake); }

EM_JS(void, js_record, (uint32_t target, uint32_t focused, uint32_t pauses, int completed), {
    Module.sim && Module.sim.record({target, focused, pauses, completed: !!completed, at: Date.now()});
});
void hal_session_record(const fc_record_t *r)
{
    js_record(r->target_ms, r->focused_ms, r->pauses, r->completed);
}

EM_JS(int, js_stats_load, (uint32_t * out), {
    try {
        const s = JSON.parse(localStorage.getItem("focuscube.stats") || "null");
        if (!s) return 0;
        HEAPU32[(out >> 2) + 0] = s.day;
        HEAPU32[(out >> 2) + 1] = s.sessions;
        HEAPU32[(out >> 2) + 2] = s.interrupted;
        HEAPU32[(out >> 2) + 3] = s.focus_ms;
        return 1;
    } catch (e) { return 0; }
});
bool hal_stats_load(app_stats_t *out)
{
    _Static_assert(sizeof(app_stats_t) == 4 * sizeof(uint32_t), "layout shared with JS");
    return js_stats_load((uint32_t *)out);
}

EM_JS(void, js_stats_save, (uint32_t day, uint32_t sessions, uint32_t interrupted, uint32_t focus_ms), {
    try {
        localStorage.setItem("focuscube.stats", JSON.stringify({day, sessions, interrupted, focus_ms}));
    } catch (e) {}
});
void hal_stats_save(const app_stats_t *s) { js_stats_save(s->day, s->sessions, s->interrupted, s->focus_ms); }

/*
 * Browser simulator for ESP-Mosaico (480x480).
 *
 * LVGL renders into a full-frame buffer that JS copies to a <canvas>.
 * The HAL is implemented here on top of a small physical model of the
 * device (orientation, magnetic field, sound, battery) that JS drives.
 * One wasm build hosts both apps: web_init(0) = Focus Cube, 1 = Sensor Lab.
 */
#include <emscripten.h>
#include <math.h>
#include <stdint.h>

#include "focus_app.h"
#include "hal.h"
#include "lab_app.h"
#include "lvgl.h"
#include "sensors.h"

#define W 480
#define H 480

static uint32_t s_fb[W * H];
static int s_fb_dirty;

static int s_ptr_x, s_ptr_y, s_ptr_down;
/* A tap shorter than LVGL's input poll period (~33 ms) would be lost;
 * latch the press until LVGL has seen it once. */
static int s_ptr_press_pending;

static double s_last_real_ms = -1;
static double s_sim_ms;
static double s_time_scale = 1.0;

/* ---------- device model ------------------------------------------------ */

/* orientation: either a raw accel vector (Focus page) or angles (Lab page) */
static sx_vec3_t s_acc = {0, 0, 1};
static float s_heading, s_pitch, s_roll;
static float s_spin_dps;           /* user spinning the device on the desk */
static sx_vec3_t s_gyro;           /* smoothed body rates                  */
static float s_prev_pitch, s_prev_roll, s_prev_heading;
static double s_shake_until;

/* Seoul-ish geomagnetic field, world frame (x north, y west, z up), µT */
static const sx_vec3_t k_field = {30, 0, -40};
static float s_magnet_ut; /* a magnet near sensor A's corner */

static float s_noise_rms = 0.01f;
static double s_clap_until;

static float s_soc = 82.0f;
static bool s_charging;
static bool s_awake = true;
static double s_vibe_until, s_sound_until;

static bool s_fn;

/* last values handed to the app, for the readout panel */
static float s_readout[16];

/* Uniform noise in [-amp, amp]; its standard deviation is amp / sqrt(3). */
static float noise(float amp) { return ((float)lv_rand(0, 2000) - 1000.0f) / 1000.0f * amp; }

/* Sensor imperfections, sized from the datasheets (per-sample values at
 * ~100 Hz ODR):
 *   BMI270 accel  noise 0.16 mg/sqrt(Hz) -> ~1 mg rms; offset up to +-20 mg
 *   BMI270 gyro   noise 0.007 dps/sqrt(Hz) -> ~0.05 dps rms; offset up to +-0.5 dps
 *   BMM150        ~0.6 uT rms (1 sigma, regular preset)
 * The fixed offsets below are within spec; real parts need calibration. */
#define ACC_NOISE_G 0.0017f
#define GYRO_NOISE_DPS 0.09f
#define MAG_NOISE_UT 1.0f
static const sx_vec3_t k_acc_offset = {0.008f, -0.006f, 0.010f};
static const sx_vec3_t k_gyro_offset = {0.3f, -0.2f, 0.4f};

static float battery_current_ma(void)
{
    if (s_charging) return s_soc < 95 ? -120.0f : -15.0f;
    float ma = s_awake ? 55.0f : 12.0f; /* backlight dominates */
    if (s_sim_ms < s_vibe_until) ma += 80.0f;
    if (s_sim_ms < s_sound_until) ma += 35.0f;
    return ma;
}

static float battery_voltage_mv(float soc, float ma)
{
    /* rough Li-ion open-circuit curve + 0.4 Ω internal resistance */
    float v;
    if (soc < 10) v = 3300 + soc * 40;
    else if (soc < 90) v = 3700 + (soc - 10) * 5.0f;
    else v = 4100 + (soc - 90) * 10;
    return v - ma * 0.4f;
}

static void model_step(double dt_ms)
{
    if (dt_ms <= 0) return;
    float dt = (float)(dt_ms / 1000.0);

    /* spinning on the desk turns the compass heading */
    s_heading = fmodf(s_heading + s_spin_dps * dt + 360.0f, 360.0f);

    /* gyro = rate of change of orientation, lightly smoothed */
    float dh = fmodf(s_heading - s_prev_heading + 540.0f, 360.0f) - 180.0f;
    sx_vec3_t raw = {(s_roll - s_prev_roll) / dt, (s_pitch - s_prev_pitch) / dt, -dh / dt};
    s_prev_heading = s_heading;
    s_prev_pitch = s_pitch;
    s_prev_roll = s_roll;
    float k = dt / (0.08f + dt);
    s_gyro.x += k * (raw.x - s_gyro.x);
    s_gyro.y += k * (raw.y - s_gyro.y);
    s_gyro.z += k * (raw.z - s_gyro.z);

    /* coulomb counting */
    s_soc -= battery_current_ma() * (float)(dt_ms / 3600000.0) / HAL_BATTERY_CAPACITY_MAH * 100.0f;
    s_soc = LV_CLAMP(0.0f, s_soc, 100.0f);
}

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

EMSCRIPTEN_KEEPALIVE void web_init(int app)
{
    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, pointer_cb);

    if (app == 1) {
        lab_app_start();
    } else {
        focus_app_start();
    }
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
     * (keeps debouncing realistic even at high speed). */
    double sim_dt = dt * s_time_scale;
    while (sim_dt > 0) {
        double step = sim_dt > 40 ? 40 : sim_dt;
        s_sim_ms += step;
        model_step(step);
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
    s_acc.x = x;
    s_acc.y = y;
    s_acc.z = z;
}

EMSCRIPTEN_KEEPALIVE void web_set_orientation(float heading, float pitch, float roll)
{
    s_heading = heading;
    s_pitch = pitch;
    s_roll = roll;
    sx_vec3_t up = {0, 0, 1};
    s_acc = sx_world_to_body(heading, pitch, roll, up);
}

EMSCRIPTEN_KEEPALIVE void web_set_spin(float dps) { s_spin_dps = dps; }
EMSCRIPTEN_KEEPALIVE void web_shake(void) { s_shake_until = s_sim_ms + 1200; }
EMSCRIPTEN_KEEPALIVE void web_set_magnet(float ut) { s_magnet_ut = ut; }
EMSCRIPTEN_KEEPALIVE void web_set_noise(float rms) { s_noise_rms = rms; }
EMSCRIPTEN_KEEPALIVE void web_clap(void) { s_clap_until = s_sim_ms + 120; }
EMSCRIPTEN_KEEPALIVE void web_set_battery(float soc, int charging)
{
    if (soc >= 0) s_soc = soc;
    s_charging = charging != 0;
}
EMSCRIPTEN_KEEPALIVE void web_set_fn(int down) { s_fn = down != 0; }
EMSCRIPTEN_KEEPALIVE float web_get_heading(void) { return s_heading; }
EMSCRIPTEN_KEEPALIVE float *web_readout(void) { return s_readout; }

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
    float sx = 0, sy = 0;
    if (s_sim_ms < s_shake_until) {
        float t = (float)(s_sim_ms / 1000.0);
        sx = 2.2f * sinf(2 * 3.14159265f * 5.0f * t);
        sy = 0.6f * sinf(2 * 3.14159265f * 5.0f * t + 1.0f);
    }
    *ax = s_acc.x + sx + k_acc_offset.x + noise(ACC_NOISE_G);
    *ay = s_acc.y + sy + k_acc_offset.y + noise(ACC_NOISE_G);
    *az = s_acc.z + k_acc_offset.z + noise(ACC_NOISE_G);
    s_readout[0] = *ax;
    s_readout[1] = *ay;
    s_readout[2] = *az;
    return true;
}

bool hal_gyro_read(float *gx, float *gy, float *gz)
{
    float shake = s_sim_ms < s_shake_until ? 150.0f * sinf((float)(s_sim_ms / 1000.0) * 31.4f) : 0;
    *gx = s_gyro.x + shake + k_gyro_offset.x + noise(GYRO_NOISE_DPS);
    *gy = s_gyro.y + k_gyro_offset.y + noise(GYRO_NOISE_DPS);
    *gz = s_gyro.z + k_gyro_offset.z + noise(GYRO_NOISE_DPS);
    s_readout[3] = *gx;
    s_readout[4] = *gy;
    s_readout[5] = *gz;
    return true;
}

bool hal_mag_read(int idx, float *mx, float *my, float *mz)
{
    sx_vec3_t m = sx_world_to_body(s_heading, s_pitch, s_roll, k_field);
    /* the magnet sits next to sensor A; sensor B, a few cm away, sees
     * much less of it (dipole field falls off with distance cubed) */
    float magnet = idx == 0 ? s_magnet_ut : s_magnet_ut * 0.12f;
    *mx = m.x + magnet + noise(MAG_NOISE_UT);
    *my = m.y + magnet * 0.5f + noise(MAG_NOISE_UT);
    *mz = m.z + noise(MAG_NOISE_UT);
    s_readout[6 + idx * 3] = *mx;
    s_readout[7 + idx * 3] = *my;
    s_readout[8 + idx * 3] = *mz;
    return true;
}

float hal_mic_rms(void)
{
    float rms = s_noise_rms * (1.0f + noise(0.25f));
    if (s_sim_ms < s_clap_until) rms = 0.6f + noise(0.1f);
    if (s_sim_ms < s_sound_until) rms = LV_MAX(rms, 0.08f); /* hears its own speaker */
    if (rms < 0) rms = 0;
    s_readout[12] = sx_dbfs(rms);
    return rms;
}

bool hal_battery_read(hal_battery_t *out)
{
    out->current_ma = battery_current_ma() + noise(0.8f);
    out->soc_pct = s_soc;
    out->voltage_mv = battery_voltage_mv(s_soc, out->current_ma);
    out->charging = s_charging;
    s_readout[13] = out->soc_pct;
    s_readout[14] = out->voltage_mv;
    s_readout[15] = out->current_ma;
    return true;
}

bool hal_fn_button(void) { return s_fn; }

EM_JS(void, js_vibrate, (int ms), { Module.sim && Module.sim.vibrate && Module.sim.vibrate(ms); });
void hal_vibrate(uint32_t ms)
{
    s_vibe_until = s_sim_ms + ms;
    js_vibrate((int)ms);
}

EM_JS(void, js_sound, (int id), { Module.sim && Module.sim.sound(id); });
void hal_sound(hal_sound_t s)
{
    s_sound_until = s_sim_ms + 600;
    js_sound((int)s);
}

EM_JS(void, js_led, (int id), { Module.sim && Module.sim.led && Module.sim.led(id); });
void hal_status_led(hal_led_t led) { js_led((int)led); }

EM_JS(void, js_awake, (int on), { Module.sim && Module.sim.awake && Module.sim.awake(!!on); });
void hal_display_set_awake(bool awake)
{
    s_awake = awake;
    js_awake(awake);
}

EM_JS(void, js_record, (uint32_t target, uint32_t focused, uint32_t pauses, int completed), {
    Module.sim && Module.sim.record && Module.sim.record({target, focused, pauses, completed: !!completed, at: Date.now()});
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

#include "focus_app.h"

#include <string.h>

#include "focus_core.h"
#include "focus_ui.h"
#include "hal.h"
#include "lvgl.h"

#define TICK_PERIOD_MS 50
#define MIN_MS (60u * 1000u)

typedef struct {
    uint32_t focus_min;
    uint32_t break_min;
} preset_t;

static const preset_t k_presets[] = {{15, 3}, {25, 5}, {50, 10}};
static const char *const k_preset_map[] = {"15 min", "25 min", "50 min", ""};
#define DEFAULT_PRESET 1

static fc_t s_fc;
static app_stats_t s_stats;
static uint32_t s_preset = DEFAULT_PRESET;
static fc_state_t s_applied_state = (fc_state_t)-1;

static void reset_stats_if_new_day(void)
{
    uint32_t today = hal_day_index();
    if (s_stats.day != today) {
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.day = today;
        hal_stats_save(&s_stats);
    }
}

static void handle_events(uint32_t ev)
{
    if (ev & FC_EV_STARTED) hal_sound(HAL_SOUND_START);
    if (ev & FC_EV_TARGET_REACHED) {
        /* face down: the buzz is felt through the desk, even with sound off */
        hal_sound(HAL_SOUND_DONE);
        hal_vibrate(600);
    }
    if (ev & FC_EV_BREAK_OVER) hal_sound(HAL_SOUND_BREAK_OVER);
    if (ev & FC_EV_RECORD) {
        const fc_record_t *r = &s_fc.last_record;
        reset_stats_if_new_day();
        if (r->completed) {
            s_stats.sessions++;
        } else {
            s_stats.interrupted++;
        }
        s_stats.focus_ms += r->focused_ms;
        hal_session_record(r);
        hal_stats_save(&s_stats);
    }
}

/* Screen and LED follow the state, not the events: one source of truth. */
static void apply_outputs(fc_state_t st)
{
    if (st == s_applied_state) return;
    s_applied_state = st;

    bool face_down = (st == FC_FOCUS || st == FC_DONE);
    hal_display_set_awake(!face_down);

    switch (st) {
    case FC_FOCUS: hal_status_led(HAL_LED_FOCUS); break;
    case FC_DONE: hal_status_led(HAL_LED_DONE); break;
    case FC_BREAK: hal_status_led(HAL_LED_BREAK); break;
    default: hal_status_led(HAL_LED_OFF); break;
    }
}

static void refresh_ui(void)
{
    ui_model_t m = {
        .state = s_fc.state,
        .remaining_ms = fc_remaining_ms(&s_fc),
        .progress_permille = fc_progress_permille(&s_fc),
        .preset_idx = s_preset,
        .stats = &s_stats,
    };
    focus_ui_update(&m);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    float ax = 0, ay = 0, az = 0; /* (0,0,0) = "no sample": timers still run */
    hal_imu_read(&ax, &ay, &az);
    handle_events(fc_update(&s_fc, hal_time_ms(), ax, ay, az));
    apply_outputs(s_fc.state);
    refresh_ui();
}

static void on_preset(uint32_t idx)
{
    if (idx >= sizeof(k_presets) / sizeof(k_presets[0])) return;
    if (fc_set_preset(&s_fc, k_presets[idx].focus_min * MIN_MS, k_presets[idx].break_min * MIN_MS)) {
        s_preset = idx;
    }
    refresh_ui();
}

static void on_end(void)
{
    handle_events(fc_end(&s_fc));
    apply_outputs(s_fc.state);
    refresh_ui();
}

void focus_app_start(void)
{
    fc_config_t cfg;
    fc_default_config(&cfg);
    const preset_t *p = &k_presets[DEFAULT_PRESET];
    fc_init(&s_fc, &cfg, p->focus_min * MIN_MS, p->break_min * MIN_MS);

    if (!hal_stats_load(&s_stats)) memset(&s_stats, 0, sizeof(s_stats));
    reset_stats_if_new_day();

    ui_callbacks_t cb = {.on_preset = on_preset, .on_end = on_end};
    focus_ui_create(k_preset_map, &cb);
    apply_outputs(s_fc.state);
    refresh_ui();

    lv_timer_create(tick_cb, TICK_PERIOD_MS, NULL);
}

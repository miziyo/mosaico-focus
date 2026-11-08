/*
 * focus_core — hardware-independent logic of the Focus Cube.
 *
 * Pure C99, no LVGL, no ESP-IDF. Feed it accelerometer samples and a
 * monotonic millisecond clock; it tells you what happened via events.
 * Everything here is unit-tested on the host (see tests/).
 */
#ifndef FOCUS_CORE_H
#define FOCUS_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Pose detection ------------------------------------------- */

typedef enum {
    FC_POSE_UNKNOWN = 0,
    FC_POSE_FACE_UP,   /* screen towards the ceiling             */
    FC_POSE_FACE_DOWN, /* screen on the desk -> "do not disturb" */
    FC_POSE_TILTED,    /* in hand / on its side: counts as "not focusing" */
} fc_pose_t;

/* Classify one accelerometer sample (units: g). */
fc_pose_t fc_classify_pose(float ax, float ay, float az);

/* ---------- Session state machine ------------------------------------ */

typedef enum {
    FC_IDLE = 0, /* face up, waiting                         */
    FC_FOCUS,    /* face down, focus timer running           */
    FC_PAUSED,   /* picked up before the target was reached  */
    FC_DONE,     /* target reached while still face down     */
    FC_BREAK,    /* break timer running                      */
} fc_state_t;

/* Events returned by fc_update()/fc_end() as a bitmask. */
enum {
    FC_EV_STARTED        = 1u << 0,
    FC_EV_PAUSED         = 1u << 1,
    FC_EV_RESUMED        = 1u << 2,
    FC_EV_TARGET_REACHED = 1u << 3, /* play the chime                   */
    FC_EV_RECORD         = 1u << 4, /* fc->last_record is ready to store */
    FC_EV_BREAK_STARTED  = 1u << 5,
    FC_EV_BREAK_OVER     = 1u << 6,
};

typedef struct {
    uint32_t pose_debounce_ms;   /* pose must be stable this long          */
    uint32_t min_record_ms;      /* shorter interrupted sessions are dropped */
    uint32_t pause_timeout_ms;   /* paused this long -> session abandoned  */
} fc_config_t;

typedef struct {
    uint32_t target_ms;  /* planned focus length              */
    uint32_t focused_ms; /* time actually spent face down     */
    uint32_t pauses;     /* how many times it was picked up   */
    bool completed;      /* reached target_ms                 */
} fc_record_t;

typedef struct {
    fc_config_t cfg;
    fc_state_t state;

    /* preset */
    uint32_t focus_target_ms;
    uint32_t break_target_ms;

    /* pose debouncing */
    fc_pose_t pose;           /* last *stable* pose        */
    fc_pose_t candidate;      /* pose currently being seen */
    uint32_t candidate_since; /* ms                        */

    /* timing */
    bool has_last_now;
    uint32_t last_now;
    uint32_t focused_ms;
    uint32_t break_ms;
    uint32_t paused_ms;
    uint32_t pauses;

    fc_record_t last_record;
} fc_t;

void fc_default_config(fc_config_t *cfg);
void fc_init(fc_t *fc, const fc_config_t *cfg, uint32_t focus_ms, uint32_t break_ms);

/* Change the preset. Ignored unless idle. Returns true if applied. */
bool fc_set_preset(fc_t *fc, uint32_t focus_ms, uint32_t break_ms);

/* Call periodically (every 20-100 ms) with a fresh IMU sample. */
uint32_t fc_update(fc_t *fc, uint32_t now_ms, float ax, float ay, float az);

/* User pressed "End" (paused) or "Skip" (break). */
uint32_t fc_end(fc_t *fc);

uint32_t fc_remaining_ms(const fc_t *fc);
/* 0..1000 progress of the current timer (focus or break). */
uint32_t fc_progress_permille(const fc_t *fc);

const char *fc_state_name(fc_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* FOCUS_CORE_H */

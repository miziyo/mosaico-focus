#include "focus_core.h"

#include <string.h>

/* |z| above this (in g) counts as lying flat. ~37 deg of tilt allowed. */
#define FLAT_THRESHOLD_G 0.8f

fc_pose_t fc_classify_pose(float ax, float ay, float az)
{
    float mag2 = ax * ax + ay * ay + az * az;
    /* Free fall or garbage sample: don't guess. */
    if (mag2 < 0.25f || mag2 > 4.0f) return FC_POSE_UNKNOWN;
    if (az > FLAT_THRESHOLD_G) return FC_POSE_FACE_UP;
    if (az < -FLAT_THRESHOLD_G) return FC_POSE_FACE_DOWN;
    return FC_POSE_TILTED;
}

void fc_default_config(fc_config_t *cfg)
{
    cfg->pose_debounce_ms = 1200;
    cfg->min_record_ms = 60 * 1000;
    cfg->pause_timeout_ms = 5 * 60 * 1000;
}

void fc_init(fc_t *fc, const fc_config_t *cfg, uint32_t focus_ms, uint32_t break_ms)
{
    memset(fc, 0, sizeof(*fc));
    if (cfg) {
        fc->cfg = *cfg;
    } else {
        fc_default_config(&fc->cfg);
    }
    fc->state = FC_IDLE;
    fc->focus_target_ms = focus_ms;
    fc->break_target_ms = break_ms;
    fc->pose = FC_POSE_UNKNOWN;
    fc->candidate = FC_POSE_UNKNOWN;
}

bool fc_set_preset(fc_t *fc, uint32_t focus_ms, uint32_t break_ms)
{
    if (fc->state != FC_IDLE || focus_ms == 0) return false;
    fc->focus_target_ms = focus_ms;
    fc->break_target_ms = break_ms;
    return true;
}

/* Feed a raw pose; returns true when the stable pose changed. */
static bool debounce_pose(fc_t *fc, fc_pose_t raw, uint32_t now)
{
    /* Unknown samples (free fall, glitches) never become the stable
     * pose; they only reset the candidate. */
    if (raw == FC_POSE_UNKNOWN) {
        fc->candidate = raw;
        fc->candidate_since = now;
        return false;
    }
    if (raw != fc->candidate) {
        fc->candidate = raw;
        fc->candidate_since = now;
    }
    /* The very first reading is trusted immediately so the device
     * boots into the right state. */
    bool stable = (fc->pose == FC_POSE_UNKNOWN) ||
                  (uint32_t)(now - fc->candidate_since) >= fc->cfg.pose_debounce_ms;
    if (stable && raw != fc->pose) {
        fc->pose = raw;
        return true;
    }
    return false;
}

static uint32_t finish_session(fc_t *fc, bool completed)
{
    uint32_t ev = 0;
    fc->last_record.target_ms = fc->focus_target_ms;
    fc->last_record.focused_ms = fc->focused_ms;
    fc->last_record.pauses = fc->pauses;
    fc->last_record.completed = completed;
    if (completed || fc->focused_ms >= fc->cfg.min_record_ms) ev |= FC_EV_RECORD;
    fc->focused_ms = 0;
    fc->paused_ms = 0;
    fc->pauses = 0;
    return ev;
}

static uint32_t start_focus(fc_t *fc)
{
    fc->state = FC_FOCUS;
    fc->focused_ms = 0;
    fc->paused_ms = 0;
    fc->pauses = 0;
    return FC_EV_STARTED;
}

static uint32_t start_break(fc_t *fc)
{
    fc->break_ms = 0;
    if (fc->break_target_ms == 0) {
        fc->state = FC_IDLE;
        return 0;
    }
    fc->state = FC_BREAK;
    return FC_EV_BREAK_STARTED;
}

uint32_t fc_update(fc_t *fc, uint32_t now, float ax, float ay, float az)
{
    uint32_t ev = 0;
    uint32_t dt = fc->has_last_now ? (uint32_t)(now - fc->last_now) : 0;
    fc->last_now = now;
    fc->has_last_now = true;

    /* 1. advance timers with the state we were in during dt */
    switch (fc->state) {
    case FC_FOCUS:
        fc->focused_ms += dt;
        if (fc->focused_ms >= fc->focus_target_ms) {
            fc->focused_ms = fc->focus_target_ms;
            fc->state = FC_DONE;
            ev |= FC_EV_TARGET_REACHED;
        }
        break;
    case FC_PAUSED:
        fc->paused_ms += dt;
        if (fc->paused_ms >= fc->cfg.pause_timeout_ms) {
            ev |= finish_session(fc, false);
            fc->state = FC_IDLE;
        }
        break;
    case FC_BREAK:
        fc->break_ms += dt;
        if (fc->break_ms >= fc->break_target_ms) {
            fc->break_ms = fc->break_target_ms;
            fc->state = FC_IDLE;
            ev |= FC_EV_BREAK_OVER;
        }
        break;
    default:
        break;
    }

    /* 2. react to pose changes */
    if (!debounce_pose(fc, fc_classify_pose(ax, ay, az), now)) return ev;

    if (fc->pose == FC_POSE_FACE_DOWN) {
        switch (fc->state) {
        case FC_IDLE:
        case FC_BREAK: /* flipping during a break = skip it */
            ev |= start_focus(fc);
            break;
        case FC_PAUSED:
            fc->state = FC_FOCUS;
            fc->paused_ms = 0;
            ev |= FC_EV_RESUMED;
            break;
        default:
            break;
        }
    } else {
        /* face up OR held in the hand: either way, not focusing */
        switch (fc->state) {
        case FC_FOCUS:
            fc->state = FC_PAUSED;
            fc->pauses++;
            ev |= FC_EV_PAUSED;
            break;
        case FC_DONE:
            ev |= finish_session(fc, true);
            ev |= start_break(fc);
            break;
        default:
            break;
        }
    }
    return ev;
}

uint32_t fc_end(fc_t *fc)
{
    uint32_t ev = 0;
    switch (fc->state) {
    case FC_PAUSED:
        ev |= finish_session(fc, false);
        fc->state = FC_IDLE;
        break;
    case FC_BREAK:
        fc->state = FC_IDLE;
        break;
    default:
        break;
    }
    return ev;
}

uint32_t fc_remaining_ms(const fc_t *fc)
{
    switch (fc->state) {
    case FC_FOCUS:
    case FC_PAUSED:
        return fc->focus_target_ms - fc->focused_ms;
    case FC_BREAK:
        return fc->break_target_ms - fc->break_ms;
    case FC_DONE:
        return 0;
    case FC_IDLE:
    default:
        return fc->focus_target_ms;
    }
}

uint32_t fc_progress_permille(const fc_t *fc)
{
    uint64_t num, den;
    switch (fc->state) {
    case FC_FOCUS:
    case FC_PAUSED:
        num = fc->focused_ms;
        den = fc->focus_target_ms;
        break;
    case FC_BREAK:
        num = fc->break_ms;
        den = fc->break_target_ms;
        break;
    case FC_DONE:
        return 1000;
    default:
        return 0;
    }
    if (den == 0) return 0;
    return (uint32_t)(num * 1000u / den);
}

const char *fc_state_name(fc_state_t s)
{
    switch (s) {
    case FC_IDLE: return "IDLE";
    case FC_FOCUS: return "FOCUS";
    case FC_PAUSED: return "PAUSED";
    case FC_DONE: return "DONE";
    case FC_BREAK: return "BREAK";
    }
    return "?";
}

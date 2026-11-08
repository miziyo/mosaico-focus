#include "sensors.h"

#include <math.h>
#include <string.h>

#define DEG (180.0f / 3.14159265f)
#define RAD (3.14159265f / 180.0f)

/* ---------- accelerometer --------------------------------------------- */

void sx_tilt_deg(sx_vec3_t a, float *pitch_deg, float *roll_deg)
{
    /* Matches sx_world_to_body(): a = (-sinθ, sinφ·cosθ, cosφ·cosθ) */
    *roll_deg = atan2f(a.y, a.z) * DEG;
    *pitch_deg = atan2f(-a.x, sqrtf(a.y * a.y + a.z * a.z)) * DEG;
}

#define SHAKE_PEAK_G 0.9f      /* | |a| - 1g | above this is a peak  */
#define SHAKE_RELEASE_G 0.4f   /* hysteresis: must drop below this   */
#define SHAKE_WINDOW_MS 1000u  /* peaks must fall inside this window */
#define SHAKE_PEAKS 3u
#define SHAKE_COOLDOWN_MS 1000u

void sx_shake_init(sx_shake_t *s) { memset(s, 0, sizeof(*s)); }

bool sx_shake_update(sx_shake_t *s, sx_vec3_t a, uint32_t now)
{
    float dev = fabsf(sqrtf(a.x * a.x + a.y * a.y + a.z * a.z) - 1.0f);

    if (s->in_peak) {
        if (dev < SHAKE_RELEASE_G) s->in_peak = false;
        return false;
    }
    if (dev < SHAKE_PEAK_G) return false;
    s->in_peak = true;
    if ((int32_t)(now - s->cooldown_until) < 0) return false;

    /* drop peaks that fell out of the window */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < s->n_peaks; i++) {
        if (now - s->peak_times[i] <= SHAKE_WINDOW_MS) s->peak_times[kept++] = s->peak_times[i];
    }
    s->n_peaks = kept;
    if (s->n_peaks < sizeof(s->peak_times) / sizeof(s->peak_times[0])) {
        s->peak_times[s->n_peaks++] = now;
    }
    if (s->n_peaks >= SHAKE_PEAKS) {
        s->n_peaks = 0;
        s->cooldown_until = now + SHAKE_COOLDOWN_MS;
        return true;
    }
    return false;
}

/* ---------- gyroscope ------------------------------------------------- */

#define GYRO_DEADBAND_DPS 1.5f

void sx_yaw_init(sx_yaw_t *y) { memset(y, 0, sizeof(*y)); }

void sx_yaw_update(sx_yaw_t *y, float gz, uint32_t now)
{
    if (y->has_last && fabsf(gz) > GYRO_DEADBAND_DPS) {
        y->yaw_deg += gz * (float)(uint32_t)(now - y->last_ms) / 1000.0f;
    }
    y->last_ms = now;
    y->has_last = true;
}

int32_t sx_yaw_turns(const sx_yaw_t *y) { return (int32_t)(y->yaw_deg / 360.0f); }

/* ---------- magnetometer ---------------------------------------------- */

float sx_heading_deg(sx_vec3_t a, sx_vec3_t m)
{
    float roll = atan2f(a.y, a.z);
    float pitch = atan2f(-a.x, sqrtf(a.y * a.y + a.z * a.z));
    float sr = sinf(roll), cr = cosf(roll);
    float sp = sinf(pitch), cp = cosf(pitch);

    /* de-rotate the field back to a level frame */
    float mx = cp * m.x + sp * (sr * m.y + cr * m.z);
    float my = cr * m.y - sr * m.z;

    float h = atan2f(my, mx) * DEG;
    if (h < 0) h += 360.0f;
    if (h >= 360.0f) h -= 360.0f;
    return h;
}

float sx_mag_disagreement(sx_vec3_t a, sx_vec3_t b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

const char *sx_cardinal(float h)
{
    static const char *const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = (int)floorf((h + 22.5f) / 45.0f) & 7;
    return names[i];
}

sx_vec3_t sx_world_to_body(float heading_deg, float pitch_deg, float roll_deg, sx_vec3_t v)
{
    /* body->world is Rz(yaw)·Ry(pitch)·Rx(roll); yaw is counter-clockwise
     * in this frame, compass heading is clockwise. Apply the transpose. */
    float yaw = -heading_deg * RAD, p = pitch_deg * RAD, r = roll_deg * RAD;
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(p), sp = sinf(p);
    float cr = cosf(r), sr = sinf(r);

    sx_vec3_t t1 = {cy * v.x + sy * v.y, -sy * v.x + cy * v.y, v.z};     /* Rz^T */
    sx_vec3_t t2 = {cp * t1.x - sp * t1.z, t1.y, sp * t1.x + cp * t1.z}; /* Ry^T */
    sx_vec3_t t3 = {t2.x, cr * t2.y + sr * t2.z, -sr * t2.y + cr * t2.z}; /* Rx^T */
    return t3;
}

/* ---------- microphone ------------------------------------------------ */

float sx_dbfs(float rms)
{
    if (rms <= 0.0000316f) return -90.0f;
    float db = 20.0f * log10f(rms);
    return db > 0 ? 0 : db;
}

sx_sound_class_t sx_sound_class(float dbfs)
{
    if (dbfs < -50.0f) return SX_SOUND_QUIET;
    if (dbfs < -25.0f) return SX_SOUND_TALK;
    return SX_SOUND_LOUD;
}

#define CLAP_JUMP_DB 15.0f
#define CLAP_MIN_DBFS -25.0f
#define CLAP_COOLDOWN_MS 300u
#define CLAP_BG_ALPHA 0.05f /* ~1 s time constant at 20 Hz */

void sx_clap_init(sx_clap_t *c) { memset(c, 0, sizeof(*c)); }

bool sx_clap_update(sx_clap_t *c, float db, uint32_t now)
{
    if (!c->primed) {
        c->background_db = db;
        c->primed = true;
        return false;
    }
    bool clap = db > c->background_db + CLAP_JUMP_DB && db > CLAP_MIN_DBFS &&
                (int32_t)(now - c->cooldown_until) >= 0;
    if (clap) {
        c->cooldown_until = now + CLAP_COOLDOWN_MS;
    } else {
        /* claps must not drag the background up */
        c->background_db += CLAP_BG_ALPHA * (db - c->background_db);
    }
    return clap;
}

/* ---------- battery --------------------------------------------------- */

uint32_t sx_battery_minutes_left(float soc_pct, float capacity_mah, float discharge_ma)
{
    if (discharge_ma <= 0.5f) return UINT32_MAX;
    if (soc_pct < 0) soc_pct = 0;
    float mah = soc_pct / 100.0f * capacity_mah;
    return (uint32_t)(mah / discharge_ma * 60.0f);
}

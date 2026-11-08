/*
 * sensors — hardware-independent signal processing for the ESP-Mosaico
 * on-board sensors. Pure C99 + libm, unit tested on the host.
 *
 * Frame conventions (device body frame, as the BMI270/BMM150 report it
 * after axis alignment):
 *   +x  towards the top edge of the screen
 *   +y  towards the left edge
 *   +z  out of the screen
 * Lying flat, face up, the accelerometer reads (0, 0, +1 g).
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, z;
} sx_vec3_t;

/* ---------- accelerometer --------------------------------------------- */

/* Pitch/roll in degrees from a gravity vector (g). */
void sx_tilt_deg(sx_vec3_t acc, float *pitch_deg, float *roll_deg);

/* Shake = several strong acceleration peaks within a short window. */
typedef struct {
    uint32_t peak_times[4];
    uint32_t n_peaks;
    uint32_t last_peak;
    uint32_t cooldown_until;
    bool in_peak;
} sx_shake_t;

void sx_shake_init(sx_shake_t *s);
/* Returns true once per detected shake gesture. */
bool sx_shake_update(sx_shake_t *s, sx_vec3_t acc, uint32_t now_ms);

/* ---------- gyroscope ------------------------------------------------- */

typedef struct {
    float yaw_deg; /* integrated rotation about z, unbounded */
    bool has_last;
    uint32_t last_ms;
} sx_yaw_t;

void sx_yaw_init(sx_yaw_t *y);
/* gz in deg/s; rates below the dead band are treated as sensor noise. */
void sx_yaw_update(sx_yaw_t *y, float gz_dps, uint32_t now_ms);
/* Whole turns, signed (counter-clockwise positive). */
int32_t sx_yaw_turns(const sx_yaw_t *y);

/* ---------- magnetometer ---------------------------------------------- */

/* Tilt-compensated compass heading in degrees, 0 = north, 90 = east. */
float sx_heading_deg(sx_vec3_t acc, sx_vec3_t mag);

/* |a - b| in µT. Two sensors a few cm apart should agree unless a magnet
 * or steel object is close to one of them. */
float sx_mag_disagreement(sx_vec3_t a, sx_vec3_t b);
#define SX_MAG_INTERFERENCE_UT 12.0f

/* "N", "NE", ... for a heading. */
const char *sx_cardinal(float heading_deg);

/* Rotate a world vector (x north, y west, z up) into the body frame of a
 * device with the given compass heading, pitch and roll (degrees).
 * Used by tests and the simulator to synthesise sensor readings. */
sx_vec3_t sx_world_to_body(float heading_deg, float pitch_deg, float roll_deg, sx_vec3_t v);

/* ---------- microphone ------------------------------------------------ */

/* RMS (0..1 of full scale) -> dBFS, floored at -90. */
float sx_dbfs(float rms);

typedef enum { SX_SOUND_QUIET, SX_SOUND_TALK, SX_SOUND_LOUD } sx_sound_class_t;
sx_sound_class_t sx_sound_class(float dbfs);

/* Clap = a sudden jump well above the recent background level. */
typedef struct {
    float background_db;
    bool primed;
    uint32_t cooldown_until;
} sx_clap_t;

void sx_clap_init(sx_clap_t *c);
bool sx_clap_update(sx_clap_t *c, float dbfs, uint32_t now_ms);

/* ---------- battery (BQ27220 fuel gauge) ------------------------------ */

/* Minutes until empty at the given discharge current (mA, > 0).
 * Returns UINT32_MAX when not discharging. */
uint32_t sx_battery_minutes_left(float soc_pct, float capacity_mah, float discharge_ma);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */

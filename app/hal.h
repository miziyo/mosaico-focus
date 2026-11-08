/*
 * Hardware abstraction layer.
 *
 * The app only talks to the outside world through these functions.
 *   platform/web/   -> implemented with JS (browser simulator)
 *   platform/esp32/ -> to be implemented with the ESP-Mosaico BSP
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "focus_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_SOUND_START,      /* short tick: "focus started"    */
    HAL_SOUND_DONE,       /* chime: target reached          */
    HAL_SOUND_BREAK_OVER, /* soft double beep               */
} hal_sound_t;

typedef enum {
    HAL_LED_OFF,
    HAL_LED_FOCUS, /* red: "do not disturb" for people around you */
    HAL_LED_DONE,  /* green pulse                                 */
    HAL_LED_BREAK, /* blue                                        */
} hal_led_t;

typedef struct {
    uint32_t day;         /* hal_day_index() these stats belong to */
    uint32_t sessions;    /* completed sessions                    */
    uint32_t interrupted; /* ended early but long enough to keep   */
    uint32_t focus_ms;    /* total focused time                    */
} app_stats_t;

/* Monotonic milliseconds (may wrap). */
uint32_t hal_time_ms(void);
/* Local calendar day number; used to reset daily stats. */
uint32_t hal_day_index(void);

/* Accelerometer in g. Returns false if no sample is available. */
bool hal_imu_read(float *ax, float *ay, float *az);

void hal_sound(hal_sound_t s);
/* RGB LED on the interaction module; no-op if module not attached. */
void hal_status_led(hal_led_t led);
/* Turn the backlight off when face down: the battery is only 65 mAh. */
void hal_display_set_awake(bool awake);

/* Gyroscope (BMI270) in deg/s. */
bool hal_gyro_read(float *gx, float *gy, float *gz);
/* Magnetometers (2x BMM150), idx 0 or 1, in µT, body frame. */
bool hal_mag_read(int idx, float *mx, float *my, float *mz);
/* Microphone (via ES8311): RMS of the latest ~50 ms block, 0..1 FS. */
float hal_mic_rms(void);

typedef struct {
    float soc_pct;    /* state of charge                       */
    float voltage_mv;
    float current_ma; /* > 0 discharging, < 0 charging         */
    bool charging;
} hal_battery_t;
/* Fuel gauge (BQ27220). */
bool hal_battery_read(hal_battery_t *out);
#define HAL_BATTERY_CAPACITY_MAH 65.0f

/* Vibration motor (GPIO8) for the given duration. */
void hal_vibrate(uint32_t ms);
/* Function / AI button (GPIO7), true while held. */
bool hal_fn_button(void);

/* Persist one finished session (flash / sync queue). */
void hal_session_record(const fc_record_t *rec);
bool hal_stats_load(app_stats_t *out);
void hal_stats_save(const app_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* HAL_H */

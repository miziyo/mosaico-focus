/* LVGL screen for the 480x480 ESP-Mosaico display. */
#ifndef FOCUS_UI_H
#define FOCUS_UI_H

#include "focus_core.h"
#include "hal.h"

typedef struct {
    fc_state_t state;
    uint32_t remaining_ms;
    uint32_t progress_permille;
    uint32_t preset_idx;
    const app_stats_t *stats;
} ui_model_t;

typedef struct {
    void (*on_preset)(uint32_t idx);
    void (*on_end)(void);
} ui_callbacks_t;

/* labels: e.g. {"15 min", "25 min", "50 min", ""} (LVGL btnmatrix map) */
void focus_ui_create(const char *const *preset_labels, const ui_callbacks_t *cb);
void focus_ui_update(const ui_model_t *m);

#endif

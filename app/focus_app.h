/* Glue between focus_core, the UI and the HAL. Platform-independent. */
#ifndef FOCUS_APP_H
#define FOCUS_APP_H

/* Call once after lv_init() and the display/input drivers are set up. */
void focus_app_start(void);

#endif

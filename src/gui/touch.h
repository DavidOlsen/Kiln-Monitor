#ifndef GUI_TOUCH_H
#define GUI_TOUCH_H

#include <Arduino.h>
#include "userSetup.h"

// Forward declarations for touch libraries
#if defined(TOUCH_XPT2046)
#include <XPT2046_Touchscreen.h>
extern XPT2046_Touchscreen ts;
#endif

#define TOUCH_CS 33

// Global variables for touch state
extern int16_t touch_max_x;
extern int16_t touch_max_y;
extern int16_t touch_raw_x;
extern int16_t touch_raw_y;
extern int16_t touch_last_x;
extern int16_t touch_last_y;

// Calibration parameters for E32R40T
extern float cal_x_scale;
extern float cal_y_scale;
extern int cal_x_offset;
extern int cal_y_offset;

void touch_init(int16_t w, int16_t h, uint8_t r);
bool touch_has_signal();
bool touch_touched();
bool touch_released();

// Helper functions
int get_touch_x();
int get_touch_y();

#endif // GUI_TOUCH_H

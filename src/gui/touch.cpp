#include "touch.h"
#include "EKcommon.h" // for g_spiMutex — this shares a physical SPI bus with the thermocouple driver

// Global variables for touch coordinates (these need to be defined in the .cpp file)
int16_t touch_last_x = 0;
int16_t touch_last_y = 0;

// Touch controller variables (from header)
int16_t touch_max_x = 0;
int16_t touch_max_y = 0;
int16_t touch_raw_x = 0;
int16_t touch_raw_y = 0;

// Calibration parameters for E32R40T
float cal_x_scale = 1.0f;
float cal_y_scale = 1.0f;
int cal_x_offset = 0;
int cal_y_offset = 0;

#if defined(TOUCH_XPT2046)
#include <XPT2046_Touchscreen.h>
// We need to make sure the touch object is properly declared
// The actual pin definition should come from userSetup.h or similar configuration
// For now, we'll use a default approach - if TOUCH_CS is defined, use it, otherwise use 15 as default
#if !defined(TOUCH_CS)
#define TOUCH_CS 15
#endif

// Create touch screen object with correct constructor (pin only, not SPI object)
XPT2046_Touchscreen ts(TOUCH_CS);
#endif

// Touch initialization function
void touch_init(int16_t width, int16_t height, uint8_t rotation) {
    // Initialize touch controller with specified parameters
    #if defined(TOUCH_XPT2046)
    ts.begin();
    ts.setRotation(rotation);
    #endif

    touch_max_x = width;
    touch_max_y = height;

    // Scale from the measured raw ADC range (userSetup.h) to the *runtime*
    // display size passed in here — not the compile-time TFT_WIDTH/HEIGHT,
    // which are the panel's native (unrotated) dimensions and don't match
    // width/height once the display is rotated to landscape.
    cal_x_scale = (float)(width - 1) / (TOUCH_MAX_X - TOUCH_MIN_X);
    cal_y_scale = (float)(height - 1) / (TOUCH_MAX_Y - TOUCH_MIN_Y);
    cal_x_offset = -TOUCH_MIN_X;
    cal_y_offset = -TOUCH_MIN_Y;
}

// Takes g_spiMutex and calls ts.touched(), bounded so a wedged SPI bus times
// out and gets logged instead of hanging main_task (and the whole GUI)
// forever. Returns false (not touched) on timeout.
#if defined(TOUCH_XPT2046)
static bool touchedLocked() {
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        log_e("touch: timed out waiting for g_spiMutex in ts.touched()");
        return false;
    }
    bool touched = ts.touched();
    xSemaphoreGive(g_spiMutex);
    return touched;
}
#endif

// Check if there's a touch signal
bool touch_has_signal() {
    #if defined(TOUCH_XPT2046)
    return touchedLocked();
    #else
    return false; // No touch controller available
    #endif
}

// Check if screen is currently touched
bool touch_touched() {
    #if defined(TOUCH_XPT2046)
    return touchedLocked();
    #else
    return false; // No touch controller available
    #endif
}

// Check if touch was released
bool touch_released() {
    #if defined(TOUCH_XPT2046)
    // This would check if the touch state changed from touched to not touched
    return !touchedLocked();
    #else
    return false; // No touch controller available
    #endif
}

// Get touch X coordinate with proper conversion from raw to display coordinates
int get_touch_x() {
    #if defined(TOUCH_XPT2046)
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        log_e("touch: timed out waiting for g_spiMutex in get_touch_x()");
        return touch_last_x;
    }
    TS_Point p = ts.getPoint();
    xSemaphoreGive(g_spiMutex);
    #if TOUCH_SWAP_XY
    touch_raw_x = p.y;
    #else
    touch_raw_x = p.x;
    #endif

    // Map the calibrated raw ADC range to display coordinates.
    int display_x = (int)((touch_raw_x + cal_x_offset) * cal_x_scale);
    #if TOUCH_INVERT_X
    display_x = touch_max_x - display_x;
    #endif

    // Ensure the value stays within valid range
    if (display_x < 0) display_x = 0;
    if (display_x > touch_max_x) display_x = touch_max_x;

    touch_last_x = display_x;
    return display_x;
    #else
    return touch_last_x; // Return last known value
    #endif
}

// Get touch Y coordinate with proper conversion from raw to display coordinates
int get_touch_y() {
    #if defined(TOUCH_XPT2046)
    if (xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        log_e("touch: timed out waiting for g_spiMutex in get_touch_y()");
        return touch_last_y;
    }
    TS_Point p = ts.getPoint();
    xSemaphoreGive(g_spiMutex);
    #if TOUCH_SWAP_XY
    touch_raw_y = p.x;
    #else
    touch_raw_y = p.y;
    #endif

    // Map the calibrated raw ADC range to display coordinates.
    int display_y = (int)((touch_raw_y + cal_y_offset) * cal_y_scale);
    #if TOUCH_INVERT_Y
    display_y = touch_max_y - display_y;
    #endif

    // Ensure the value stays within valid range
    if (display_y < 0) display_y = 0;
    if (display_y > touch_max_y) display_y = touch_max_y;

    touch_last_y = display_y;
    return display_y;
    #else
    return touch_last_y; // Return last known value
    #endif
}
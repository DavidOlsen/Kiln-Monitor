#ifndef USERSETUP_H
#define USERSETUP_H

#include <Arduino.h>

const bool DEBUG = true;

const int MAX_TOUCH_POINTS = 1;
const int TOUCH_TIMEOUT = 2000;

#define TOUCH_XPT2046

// E32R40T specific touch configuration
#ifndef TOUCH_XPT2046_ROTATION
#define TOUCH_XPT2046_ROTATION 2
#endif

const int TFT_WIDTH = 320;
const int TFT_HEIGHT = 480;

// Multi-zone thermocouple config (MCP9600, I2C). One zone per kiln section
// being monitored (upper/middle/lower). Zone count is auto-detected at boot
// by probing these addresses on the bus, not manually configured — plug in
// 1-3 probes as needed. KilnMonitor has no heater outputs; it's a passive
// logger, unlike ElectricKiln.
#define MAX_ZONES 3
#define TC_I2C_SDA_PIN 32
#define TC_I2C_SCL_PIN 25
const uint8_t TC_ZONE_I2C_ADDR[MAX_ZONES] = {0x67, 0x66, 0x65};
// Only K and S are exposed in the Config screen — narrower than the 8 types
// the MCP9600 itself supports, matching this device's expected probes.
#define TC_DEFAULT_TYPE 'K'

// Touch configuration for E32R40T
#ifndef TOUCH_XPT2046_SCK
#define TOUCH_XPT2046_SCK 14
#endif
#ifndef TOUCH_XPT2046_MISO
#define TOUCH_XPT2046_MISO 12
#endif
#ifndef TOUCH_XPT2046_MOSI
#define TOUCH_XPT2046_MOSI 13
#endif
#ifndef TOUCH_XPT2046_CS
#define TOUCH_XPT2046_CS 15
#endif
#ifndef TOUCH_XPT2046_INT
#define TOUCH_XPT2046_INT 39
#endif

#define BAR_COLOR 0x53D2

// Touch calibration values for E32R40T (these need to be calibrated based on your actual hardware)
// You can find these by testing with a simple calibration script or measuring the raw values
const int TOUCH_MIN_X = 100;   // Minimum raw X value from touch controller
const int TOUCH_MAX_X = 3900;  // Maximum raw X value from touch controller
const int TOUCH_MIN_Y = 100;   // Minimum raw Y value from touch controller
const int TOUCH_MAX_Y = 3900;  // Maximum raw Y value from touch controller

// Touch axis orientation relative to the display. The XPT2046 touch
// controller's rotation and the ST7796 display driver's rotation don't
// necessarily agree, so if touches don't land on what's drawn there, use
// the raw/calibrated coordinates logged per-touch (see readButtons() in
// gui.cpp) to work out which of these needs to flip, then reflash.
// Derived from a 4-corner test on the E32R40T panel (2026-07-24):
// raw X tracked top/bottom (inverted) and raw Y tracked left/right.
#define TOUCH_SWAP_XY   1
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  1

#endif // USERSETUP_H

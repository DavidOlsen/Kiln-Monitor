#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

// Function to be executed as a task for the ESP32
void telemetry_task(void *parameter);

// Ramp rate (degrees C per hour) of the max active-zone temperature over the
// trailing windowMinutes, computed from an in-memory rolling history kept by
// telemetry_task — not an InfluxDB query, so it works even when InfluxDB is
// unreachable/unconfigured. Returns NAN if not enough history has
// accumulated yet to cover the requested window.
double getRampRate(uint32_t windowMinutes);

#endif

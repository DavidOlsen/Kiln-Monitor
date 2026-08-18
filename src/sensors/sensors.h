#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>

// Function declarations
void sensor_task(void *pvParameter);

// Pure fault-decision logic, split out from the live I2C read so it can be
// exercised with fabricated status/temperature values (see test/test_sensors).
struct ZoneFaultResult {
    bool faulted;
    const char* errMsg; // "" when not faulted
};
ZoneFaultResult evaluateZoneFault(float temperature, uint8_t status);

#endif 
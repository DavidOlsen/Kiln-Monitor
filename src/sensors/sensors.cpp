#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP9601.h>

#include "userSetup.h"
#include "EKcommon.h"
#include "sensors.h"

static const char* TAG = "sensor_task";

namespace {
  char lastAppliedTcType = '\0';
}

static MCP9600_ThemocoupleType mcp9600TypeFromChar(char t) {
    switch (t) {
        case 'K': return MCP9600_TYPE_K;
        case 'S': return MCP9600_TYPE_S;
        default:  return MCP9600_TYPE_K;
    }
}

static bool mcp9600TypeCharValid(char t) {
    return t == 'K' || t == 'S';
}

static void mcp9600ConfigureZone(Adafruit_MCP9601 &dev, char tcType) {
    dev.setAmbientResolution(RES_ZERO_POINT_0625);
    dev.setADCresolution(MCP9600_ADCRESOLUTION_18);
    dev.setThermocoupleType(mcp9600TypeFromChar(tcType));
    dev.setFilterCoefficient(3);
    dev.enable(true);
}

// Probes any zone not currently marked present. Safe to call repeatedly —
// a missing device just NACKs and begin() returns false, so this both
// performs the initial boot-time scan and lets probes be hot-plugged later.
static void mcp9600ScanZones(Adafruit_MCP9601 mcp[], bool present[], char tcType) {
    for (int i = 0; i < MAX_ZONES; i++) {
        if (present[i]) continue;
        if (mcp[i].begin(TC_ZONE_I2C_ADDR[i], &Wire)) {
            mcp9600ConfigureZone(mcp[i], tcType);
            present[i] = true;

            xSemaphoreTake(mutex, portMAX_DELAY);
            g_zones[i].active = true;
            g_zones[i].fault = false;
            g_zones[i].errMsg = "";
            xSemaphoreGive(mutex);

            log_i("Zone %d: MCP9601 detected at 0x%02X", i + 1, TC_ZONE_I2C_ADDR[i]);
        }
    }
}

// Re-applies g_tcType to every present zone when it changes. All zones
// share one thermocouple type setting (Config screen: K or S).
static void mcp9600HandleTcType(Adafruit_MCP9601 mcp[], bool present[]) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    char currentTcType = g_tcType;
    xSemaphoreGive(mutex);

    if (currentTcType == lastAppliedTcType) return;

    if (!mcp9600TypeCharValid(currentTcType)) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_ZONES; i++) {
            if (g_zones[i].active) {
                g_zones[i].fault = true;
                g_zones[i].errMsg = String("Invalid TC type: ") + currentTcType;
            }
        }
        xSemaphoreGive(mutex);
        lastAppliedTcType = currentTcType;
        return;
    }

    for (int i = 0; i < MAX_ZONES; i++) {
        if (present[i]) mcp[i].setThermocoupleType(mcp9600TypeFromChar(currentTcType));
    }
    lastAppliedTcType = currentTcType;
}

// Pure decision logic: given a raw reading + status byte, decide whether the
// zone is faulted. No hardware/mutex access here so it's callable from
// on-device unit tests with fabricated inputs (e.g. simulating a probe that's
// been unplugged, which reports via the same status bit as MCP9601_STATUS_OPENCIRCUIT).
ZoneFaultResult evaluateZoneFault(float temperature, uint8_t status) {
    bool badReading = isnan(temperature) || temperature < -50.0f || temperature > 1400.0f;
    bool inputRangeFault = (status & MCP960X_STATUS_INPUTRANGE) != 0;
    if (badReading) return {true, "Reading out of range"};
    if (inputRangeFault) return {true, "Input range fault"};
    return {false, ""};
}

// A faulted zone just stops contributing readings — other active zones
// keep logging independently, and the ambient/trigger logic in
// telemetry.cpp already skips faulted zones.
static void mcp9600ReadZone(int i, Adafruit_MCP9601 &dev) {
    uint8_t status = dev.getStatus();
    float t = dev.readThermocouple();

    ZoneFaultResult result = evaluateZoneFault(t, status);

    xSemaphoreTake(mutex, portMAX_DELAY);
    g_zones[i].fault = result.faulted;
    if (result.faulted) {
        g_zones[i].errMsg = result.errMsg;
    } else {
        g_zones[i].errMsg = "";
        g_zones[i].pv = t + (float)tempOffset;
    }
    xSemaphoreGive(mutex);
}

static void mcp9600ReadAllZones(Adafruit_MCP9601 mcp[], bool present[]) {
    for (int i = 0; i < MAX_ZONES; i++) {
        if (present[i]) mcp9600ReadZone(i, mcp[i]);
    }
}

void sensor_task(void *pvParameter) {
  static Adafruit_MCP9601 mcp[MAX_ZONES];
  static bool present[MAX_ZONES] = {false, false, false};
  unsigned long tempStart = 0;
  unsigned long lastRescan = 0;

  Wire.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN);

  xSemaphoreTake(mutex, portMAX_DELAY);
  char tcType = g_tcType;
  xSemaphoreGive(mutex);

  mcp9600ScanZones(mcp, present, tcType);
  lastAppliedTcType = tcType;
  lastRescan = millis();

  xSemaphoreTake(mutex, portMAX_DELAY);
  g_zonesScanned = true;
  xSemaphoreGive(mutex);
  log_i("Initial I2C zone scan complete");

  for (;;) {
    mcp9600HandleTcType(mcp, present);

    if (millis() - tempStart >= (unsigned long)tempCycle) {
      mcp9600ReadAllZones(mcp, present);
      tempStart = millis();
    }

    // Periodically re-probe any address that hasn't answered yet, so a
    // probe plugged in after boot gets picked up without a reflash.
    if (millis() - lastRescan >= 5000) {
      xSemaphoreTake(mutex, portMAX_DELAY);
      char currentTcType = g_tcType;
      xSemaphoreGive(mutex);
      mcp9600ScanZones(mcp, present, currentTcType);
      lastRescan = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay to prevent busy waiting
  }
}

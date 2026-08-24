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

// Max plausible raw ADC magnitude for any real thermocouple reading across
// the full operating range of either supported type: K tops out around
// 1372C (~55mV EMF), S around 1768C (~19mV EMF) — K governs. At the MCP9600's
// ~2uV/LSB ADC weighting that's roughly 27,000 counts at the extreme high
// end. Field testing showed a genuinely disconnected/floating input reads
// ~120,000+ counts, so this threshold has wide margin on both sides. Not yet
// validated against a real high-temperature firing — worth confirming adcRaw
// stays comfortably under this near a zone's actual max operating temp.
static const int32_t ADC_RAW_OPEN_CIRCUIT_THRESHOLD = 50000;

// A dead short ties both thermocouple leads to the same potential, so a
// genuine short should read close to zero EMF, not a real thermal signal.
// This is the inverse shape of the open-circuit check, but UNLIKE that one
// it isn't yet backed by field comparisons — the only real data point so far
// is a normal at-rest connected probe (Zone 3) sitting at ~15-34 counts, so
// this threshold is set tighter than that as a starting guess. Needs the same
// kind of field validation open-circuit got: capture RAW debug lines for a
// deliberately shorted probe vs. a normal one and confirm/tune this value.
static const int32_t ADC_RAW_SHORT_CIRCUIT_THRESHOLD = 10;

// Pure decision logic: given a raw reading + status byte + raw ADC counts,
// decide whether the zone is faulted. No hardware/mutex access here so it's
// callable from on-device unit tests with fabricated inputs.
//
// MCP9601_STATUS_OPENCIRCUIT and MCP9601_STATUS_SHORTCIRCUIT are MCP9601-
// specific bits (the base MCP9600 only has the generic
// MCP960X_STATUS_INPUTRANGE at the same 0x10 position as OPENCIRCUIT, with
// no short-circuit detection at all).
//
// Both status bits are noisy alone (flicker on marginal-but-connected
// probes), and the linearized hotJunction reading is NOT a reliable
// corroborating signal for either: field testing showed it can land on a
// plausible-but-bogus non-zero value on a disconnected input, and — worse —
// it can freeze at the last good value if a probe is disconnected mid-run,
// giving no indication anything's wrong. The raw ADC magnitude is the
// reliable signal instead: open circuit rails it far outside any real
// thermocouple's EMF range, short circuit pins it near zero — both in real
// time, regardless of what hotJunction is doing.
ZoneFaultResult evaluateZoneFault(float temperature, uint8_t status, int32_t adcRaw) {
    bool badReading = isnan(temperature) || temperature < -50.0f || temperature > 1400.0f;
    if (badReading) return {true, "Reading out of range"};

    bool adcOutOfRange = adcRaw > ADC_RAW_OPEN_CIRCUIT_THRESHOLD || adcRaw < -ADC_RAW_OPEN_CIRCUIT_THRESHOLD;
    bool adcNearZero   = adcRaw <= ADC_RAW_SHORT_CIRCUIT_THRESHOLD && adcRaw >= -ADC_RAW_SHORT_CIRCUIT_THRESHOLD;
    bool openCircuit  = (status & MCP9601_STATUS_OPENCIRCUIT) != 0 && adcOutOfRange;
    bool shortCircuit = (status & MCP9601_STATUS_SHORTCIRCUIT) != 0 && adcNearZero;
    if (openCircuit)  return {true, "Probe disconnected (open circuit)"};
    if (shortCircuit) return {true, "Probe shorted (short circuit)"};

    return {false, ""};
}

static const char* tcTypeCharFromEnum(MCP9600_ThemocoupleType t) {
    switch (t) {
        case MCP9600_TYPE_K: return "K";
        case MCP9600_TYPE_J: return "J";
        case MCP9600_TYPE_T: return "T";
        case MCP9600_TYPE_N: return "N";
        case MCP9600_TYPE_S: return "S";
        case MCP9600_TYPE_E: return "E";
        case MCP9600_TYPE_B: return "B";
        case MCP9600_TYPE_R: return "R";
        default:             return "?";
    }
}

// A faulted zone just stops contributing readings — other active zones
// keep logging independently, and the ambient/trigger logic in
// telemetry.cpp already skips faulted zones.
static void mcp9600ReadZone(int i, Adafruit_MCP9601 &dev) {
    uint8_t status = dev.getStatus();
    float t = dev.readThermocouple();

    // TEMP DEBUG: raw chip state, to diagnose K vs S readings. getThermocoupleType()
    // re-reads the chip's actual live SENSORCONFIG register rather than trusting
    // whatever we last told it to set, to catch any desync.
    float ambientRaw = dev.readAmbient();
    int32_t adcRaw = dev.readADC();
    MCP9600_ThemocoupleType chipType = dev.getThermocoupleType();
    log_i("Zone %d RAW: type=%s hotJunction=%.4fC ambient=%.4fC adcRaw=%ld status=0x%02X",
          i + 1, tcTypeCharFromEnum(chipType), t, ambientRaw, (long)adcRaw, status);

    ZoneFaultResult result = evaluateZoneFault(t, status, adcRaw);

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

#ifndef COMMON_H
#define COMMON_H

#include <Arduino.h>
#include <Preferences.h>
#include "userSetup.h"

#define SIMULATION false // Set to true for simulation mode

const int tempCycle = 500;               // Temperature reading cycle (ms)
const int tempOffset = 0;                // Temp offset (degrees), e.g. for a known cold-junction bias
const int topBarCycle = 4000;            // Refresh rate to update top info bar on TFT (ms)

// Auto start/stop of InfluxDB logging, based on the average of active
// zones at rest ("ambient"). Any zone rising AMBIENT_START_DELTA above
// ambient starts a logging run; logging stops once every active zone has
// returned within AMBIENT_STOP_DELTA of ambient. Ambient is re-captured
// after each run ends, so the device is ready for a repeat test without a
// reboot even if room temperature has drifted.
const double AMBIENT_START_DELTA = 15.0; // degrees above ambient — any zone
const double AMBIENT_STOP_DELTA = 5.0;   // degrees above ambient — every active zone

// Thermocouple driver selection
#define TC_DRIVER_MCP9600

struct InfluxDbConfig {
  String url;
  String token;
  String org;
  String bucket;
  String tzInfo;
  bool configured = false;
};

enum class OtaStatus {
  IDLE,
  CHECKING,
  UPDATE_AVAILABLE,
  UP_TO_DATE,
  UPDATING,
  ERROR
};

// One monitored kiln section's state (upper/middle/lower). A single-probe
// setup only ever has zones[0].active == true; zones 1/2 are simply absent
// from the I2C bus and stay inactive.
struct ZoneState {
  bool active = false;     // detected on the I2C bus at scan time
  bool fault = false;      // sensor fault/disconnected/out of range
  String errMsg;
  double pv = 0;           // process variable: this zone's measured temperature
};

extern InfluxDbConfig g_influxConfig; // InfluxDB connection settings loaded from LittleFS
extern OtaStatus g_ota_status;        // OTA state machine status
extern String g_ota_latest_version;   // Latest release name from GitHub
extern String g_ota_latest_tag;       // Latest release tag from GitHub
extern SemaphoreHandle_t mutex;        // For thread safety
extern SemaphoreHandle_t disp_mutex;   // For display calls
extern SemaphoreHandle_t g_spiMutex;   // SPI bus mutex — guards the touch controller's SPI bus
extern Preferences preferences;        // NVS-backed settings, namespace "kilnmonitor"

/* global variables: used between concurrent tasks -> mutex */
extern ZoneState g_zones[MAX_ZONES];   // Per-zone thermocouple state
extern bool g_zonesScanned;            // True once the initial I2C zone-detection scan has completed
extern bool g_connected;               // Is the ESP connected to WiFi?
extern bool g_connecting;              // Is the ESP trying to connect to WiFi
extern bool g_published;               // Is the ESP publishing to InfluxDB?
extern char g_tcType;                  // Runtime thermocouple type, shared by all zones ('K' or 'S')
extern char g_tempScale;               // Runtime temperature unit, 'C' or 'F'

extern double g_ambientBaseline;       // Average PV of active zones at rest
extern bool g_loggingActive;           // True while a test run is actively being logged to InfluxDB
extern uint32_t g_sessionId;           // Increments on each WAITING->LOGGING transition; persisted in NVS across resets
extern String g_kilnName;              // User-set label tagged onto InfluxDB points, so multiple kilns can be told apart

// Peak PV (Celsius) seen across active zones during the current logging run;
// reset when a new session starts, otherwise holds the last completed
// session's peak. -1000 means no session has run yet this boot (matches the
// same sentinel convention as updateLoggingTrigger()'s local maxPV).
extern double g_maxTemperature;

// Fallback for g_kilnName when the user hasn't set one. g_kilnName must
// never be empty — InfluxDB tags are only written when non-empty (see
// telemetry.cpp), so an empty name means points silently lose the KilnName
// tag entirely, making that device's data unreachable from a Grafana
// "select kiln" dropdown. Derived from the factory-programmed MAC so it's
// stable and unique per device with no configuration required.
inline String defaultKilnName() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[16];
  snprintf(buf, sizeof(buf), "Kiln-%06X", (uint32_t)(mac & 0xFFFFFF));
  return String(buf);
}

#endif

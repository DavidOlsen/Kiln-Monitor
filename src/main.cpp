#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LittleFS.h>

#include "userSetup.h"      // Setup user variables (CHANGE THESE IN HEADER FILE)
#include "EKcommon.h"         // Common variables and functions

#include "gui/gui.h"            // Graphical user interface source file
#include "network/network.h"        // WiFi and Server related code
#include "ota/ota.h"       // OTA firmware update task
#include "telemetry/telemetry.h"  // Influx DB publishing task
#include "sensors/sensors.h"    // Thermocouple reading task

// Include touch functionality
#include "gui/touch.h"

// global (shared) variables definition
ZoneState g_zones[MAX_ZONES];
bool g_zonesScanned;
bool g_connected;
bool g_connecting;
bool g_published;
char g_tcType;
double g_ambientBaseline;
bool g_loggingActive;
char g_tempScale = 'C';
uint32_t g_sessionId = 0;
String g_kilnName;
double g_maxTemperature = -1000;

// External objects initialization
InfluxDbConfig g_influxConfig;
OtaStatus g_ota_status = OtaStatus::IDLE;
String g_ota_latest_version;
String g_ota_latest_tag;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
SemaphoreHandle_t disp_mutex = xSemaphoreCreateMutex();
SemaphoreHandle_t g_spiMutex = xSemaphoreCreateMutex();
MyNetwork network(mutex, LittleFS);

// put function declarations here:
void main_task(void* parameter);

 #include <esp_task_wdt.h>

#ifndef UNIT_TEST
void setup() {

  // Option 1: Disable completely
  esp_task_wdt_init(30, false); // Initialize with 30s timeout and set panic to false (disable reset)
  esp_task_wdt_delete(NULL);    // Delete the WDT for the current running task

  Serial.begin(115200);
  SPI.begin();

  delay(1500);

  // Touch is initialized inside gui_start() (needs tft->width()/height()
  // after the display is up).
  gui_start(); // also seeds g_tcType from preferences (falls back to TC_DEFAULT_TYPE)

  // Mount LittleFS file system
  while (!LittleFS.begin(true)) {
    disp_error_msg("LittleFS Error", "Can't setup file system.", "Make sure files are uploaded.");
    delay(200);
  }

  // Load WiFi credentials
  network.loadWifiCredentials();
  // Start the web server early so the config page is reachable even before
  // the ESP has connected to Wi-Fi.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("ESP32 Kiln Monitor", NULL);
  network.setupServer();
  // Load InfluxDB credentials
  network.loadInfluxDbCredentials();

  // Create the main task and set its affinity to core 1
  xTaskCreatePinnedToCore(main_task, "Main", 8192, NULL, 1, NULL, 1);
  // Create the sensor task and set its affinity to core 1
  xTaskCreatePinnedToCore(sensor_task, "Sensor", 8192, NULL, 1, NULL, 1);
  // Create the telemetry task (also runs the auto start/stop trigger) and set its affinity to core 0
  xTaskCreatePinnedToCore(telemetry_task, "Telemetry", 32768, NULL, 1, NULL, 0);
  // Create the OTA task and set its affinity to core 0
  xTaskCreatePinnedToCore(ota_task, "OTA", 32768, NULL, 1, NULL, 0);

  log_i("Total heap: %d", ESP.getHeapSize());
  log_i("Free heap: %d", ESP.getFreeHeap());
}

// Task function to run the GUI on core 1
void main_task(void* parameter) {

  while (1) {
    gui_run();
  }
}

//*******************************************************************************************************************************

void loop() {
  vTaskDelay(10);
}
#endif // UNIT_TEST

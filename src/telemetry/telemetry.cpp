#include <Arduino.h>
#include <math.h>
#include <InfluxDbClient.h> // Write data to Influx Data Base
#include <InfluxDbCloud.h>  // Enable Influx Data Cloud storage

#include "EKcommon.h"
#include "userSetup.h"

#include "../network/network.h"
#include "telemetry.h"

extern MyNetwork network;

Point sensor("KILN MONITOR");

// Ambient/auto-start-stop trigger, evaluated once per cycle before deciding
// whether to publish. See EKcommon.h for AMBIENT_START_DELTA/AMBIENT_STOP_DELTA.
static bool ambientCaptured = false;

static void updateLoggingTrigger() {
  bool sessionStarted = false;
  uint32_t newSessionId = 0;
  String kilnName;

  xSemaphoreTake(mutex, portMAX_DELAY);
  bool scanned = g_zonesScanned;
  double sum = 0;
  int count = 0;
  double maxPV = -1000;
  double ambient = g_ambientBaseline;
  bool loggingActive = g_loggingActive;
  bool allReturned = true;
  for (int i = 0; i < MAX_ZONES; i++) {
    if (!g_zones[i].active || g_zones[i].fault) continue;
    sum += g_zones[i].pv;
    count++;
    if (g_zones[i].pv > maxPV) maxPV = g_zones[i].pv;
    if (fabs(g_zones[i].pv - ambient) > AMBIENT_STOP_DELTA) allReturned = false;
  }

  if (scanned && count > 0) {
    double avg = sum / count;
    if (!ambientCaptured) {
      g_ambientBaseline = avg;
      ambientCaptured = true;
      log_i("Ambient baseline captured: %.1f", avg);
    } else if (!loggingActive && maxPV >= ambient + AMBIENT_START_DELTA) {
      g_loggingActive = true;
      g_sessionId++;
      newSessionId = g_sessionId;
      kilnName = g_kilnName;
      sessionStarted = true;
      log_i("Logging started: zone at %.1f, ambient %.1f (session %u)", maxPV, ambient, g_sessionId);
    } else if (loggingActive && count > 0 && allReturned) {
      g_loggingActive = false;
      g_ambientBaseline = avg; // re-baseline so a repeat test doesn't need a reboot
      log_i("Logging stopped: zones back to ambient (%.1f)", avg);
    }
  }
  xSemaphoreGive(mutex);

  // Flash write and Point tag update kept outside the shared mutex so a
  // slow NVS commit can't stall the GUI/sensor tasks waiting on it. The tag
  // is set once per session here rather than per-write — Point::addTag()
  // appends unconditionally with no dedupe, so calling it from the write
  // loop would keep piling up duplicate SessionId tags on every publish.
  if (sessionStarted) {
    preferences.putUInt("sessionId", newSessionId);
    sensor.clearTags();
    sensor.addTag("SessionId", String(newSessionId));
    if (!kilnName.isEmpty()) {
      sensor.addTag("KilnName", kilnName);
    }
  }
}

void telemetry_task(void* parameter) {
  InfluxDBClient* client = nullptr;
  bool connected = false;
  bool prevConnected = false;
  bool published = false;

  while (1) {

    updateLoggingTrigger();

    xSemaphoreTake(mutex, portMAX_DELAY);
    g_published = published;
    xSemaphoreGive(mutex);

    bool captiveMode = network.get_captive_mode();
    prevConnected = connected;
    connected = network.checkWiFi();

    if (captiveMode || !connected) {
      published = false;
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // (Re)initialize client when credentials are loaded or changed
    if (client == nullptr || network.hasNewInfluxCredentials()) {
      // Copy config under mutex to avoid cross-core data race on String fields
      InfluxDbConfig cfg;
      xSemaphoreTake(mutex, portMAX_DELAY);
      network.clearInfluxCredentialsFlag();
      cfg = g_influxConfig;
      xSemaphoreGive(mutex);

      if (!cfg.configured) {
        log_i("InfluxDB not configured. Skipping publish.\n");
        published = false;
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        continue;
      }

      delete client;
      client = new InfluxDBClient(
        cfg.url.c_str(),
        cfg.org.c_str(),
        cfg.bucket.c_str(),
        cfg.token.c_str(),
        InfluxDbCloud2CACert
      );
      log_i("InfluxDB client (re)initialized: url=%s org=%s bucket=%s",
            cfg.url.c_str(), cfg.org.c_str(), cfg.bucket.c_str());
      log_i("Free heap after InfluxDB client init: %d", ESP.getFreeHeap());
      timeSync(cfg.tzInfo.c_str(), "pool.ntp.org", "time.nis.gov");

      // Actively probes the server (auth + org/bucket lookup) right away,
      // instead of waiting for the first writePoint() to surface a problem.
      if (client->validateConnection()) {
        log_i("InfluxDB connection validated OK: %s", client->getServerUrl().c_str());
      } else {
        log_e("InfluxDB connection validation FAILED (status %d): %s",
              client->getLastStatusCode(), client->getLastErrorMessage().c_str());
      }
    }

    if (!prevConnected && connected) {
      InfluxDbConfig cfg;
      xSemaphoreTake(mutex, portMAX_DELAY);
      cfg = g_influxConfig;
      xSemaphoreGive(mutex);
      timeSync(cfg.tzInfo.c_str(), "pool.ntp.org", "time.nis.gov");
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    bool loggingActive = g_loggingActive;
    xSemaphoreGive(mutex);

    // Only write to InfluxDB during an active test run — that's the whole
    // point of the auto start/stop trigger above.
    if (connected && client != nullptr && loggingActive) {
      sensor.clearFields();

      xSemaphoreTake(mutex, portMAX_DELAY);
      for (int i = 0; i < MAX_ZONES; i++) {
        if (!g_zones[i].active) continue;
        String prefix = "Zone" + String(i + 1) + " ";
        sensor.addField(prefix + "temperature", g_zones[i].pv);
        sensor.addField(prefix + "fault", g_zones[i].fault);
      }
      sensor.addField("Ambient", g_ambientBaseline);
      xSemaphoreGive(mutex);

      published = client->writePoint(sensor);
      if (!published) {
        log_e("InfluxDB write failed (status %d): %s | point: %s",
              client->getLastStatusCode(),
              client->getLastErrorMessage().c_str(),
              sensor.toLineProtocol().c_str());
      } else {
        // Logged on every successful write, not just failure/recovery
        // transitions — verbose (one line/10s during a run) but requested
        // for confirming write cadence during debugging.
        log_i("InfluxDB write OK: %s", sensor.toLineProtocol().c_str());
      }
    } else {
      published = false;
    }

    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

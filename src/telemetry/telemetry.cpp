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

// Rolling history of the max active-zone temperature, sampled roughly once a
// minute, kept entirely in RAM so ramp-rate math never has to touch
// InfluxDB — see getRampRate()'s comment for why that matters on this
// project. 240 samples at 1/min covers 4 hours; a plain circular buffer
// (write pointer + count) rather than a timestamp-indexed structure, since
// scanning <=240 entries is trivial on this hardware and avoids any
// wraparound bookkeeping beyond what unsigned millis() subtraction already
// handles for free.
namespace {
  const int TEMP_HISTORY_CAPACITY = 240;
  const unsigned long TEMP_HISTORY_INTERVAL_MS = 60000; // 1 sample/minute

  struct TempSample { unsigned long timeMs; double value; };
  TempSample tempHistory[TEMP_HISTORY_CAPACITY];
  int tempHistoryCount = 0;      // valid entries so far, caps at CAPACITY
  int tempHistoryWriteIndex = 0; // next slot to write; once full, also the oldest entry
  unsigned long lastHistorySampleMs = 0;

  // Appends a sample if enough time has passed since the last one. Tracks
  // regardless of g_loggingActive — watching the ramp climb during preheat,
  // before the auto-start threshold trips, is exactly the point.
  void recordTempHistorySample(double maxPV, unsigned long nowMs) {
    if (lastHistorySampleMs != 0 && (nowMs - lastHistorySampleMs) < TEMP_HISTORY_INTERVAL_MS) return;
    lastHistorySampleMs = nowMs;
    tempHistory[tempHistoryWriteIndex] = {nowMs, maxPV};
    tempHistoryWriteIndex = (tempHistoryWriteIndex + 1) % TEMP_HISTORY_CAPACITY;
    if (tempHistoryCount < TEMP_HISTORY_CAPACITY) tempHistoryCount++;
  }
}

double getRampRate(uint32_t windowMinutes) {
  xSemaphoreTake(mutex, portMAX_DELAY);

  // "Now" is the freshest live reading, not the most recent history sample
  // (which can lag by up to ~1 minute) — keeps the rate current even
  // between samples.
  double currentMax = -1000;
  bool haveCurrent = false;
  for (int i = 0; i < MAX_ZONES; i++) {
    if (!g_zones[i].active || g_zones[i].fault) continue;
    if (g_zones[i].pv > currentMax) currentMax = g_zones[i].pv;
    haveCurrent = true;
  }

  if (!haveCurrent || tempHistoryCount == 0) {
    xSemaphoreGive(mutex);
    return NAN;
  }

  unsigned long now = millis();
  unsigned long targetAgeMs = (unsigned long)windowMinutes * 60000UL;
  int startIndex = (tempHistoryCount < TEMP_HISTORY_CAPACITY) ? 0 : tempHistoryWriteIndex;

  // Not enough history yet to cover this window — e.g. a 4-hour rate asked
  // for 10 minutes after boot.
  if ((now - tempHistory[startIndex].timeMs) < targetAgeMs) {
    xSemaphoreGive(mutex);
    return NAN;
  }

  // Find the recorded sample closest to (now - targetAgeMs).
  unsigned long bestDiff = 0xFFFFFFFFUL;
  double refValue = 0;
  unsigned long refTime = 0;
  for (int i = 0; i < tempHistoryCount; i++) {
    int idx = (startIndex + i) % TEMP_HISTORY_CAPACITY;
    unsigned long age = now - tempHistory[idx].timeMs;
    unsigned long diff = (age > targetAgeMs) ? (age - targetAgeMs) : (targetAgeMs - age);
    if (diff < bestDiff) {
      bestDiff = diff;
      refValue = tempHistory[idx].value;
      refTime = tempHistory[idx].timeMs;
    }
  }
  xSemaphoreGive(mutex);

  double elapsedHours = (double)(now - refTime) / 3600000.0;
  if (elapsedHours <= 0.0001) return NAN;
  return (currentMax - refValue) / elapsedHours;
}

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
      g_maxTemperature = maxPV; // seed this run's peak — see below for why it isn't reset elsewhere
      log_i("Logging started: zone at %.1f, ambient %.1f (session %u)", maxPV, ambient, g_sessionId);
    } else if (loggingActive && count > 0 && allReturned) {
      g_loggingActive = false;
      g_ambientBaseline = avg; // re-baseline so a repeat test doesn't need a reboot
      log_i("Logging stopped: zones back to ambient (%.1f)", avg);
    }

    // Tracks the running peak for the display (status page: degrees + Orton
    // cone equivalent). Deliberately not reset when logging stops — it's
    // meant to keep showing the just-completed run's peak until the next
    // session starts and re-seeds it above, not blank out the moment a
    // firing ends.
    if (loggingActive && maxPV > g_maxTemperature) {
      g_maxTemperature = maxPV;
    }

    recordTempHistorySample(maxPV, millis());
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
  unsigned long lastDiagLog = 0;

  while (1) {

    // Heap trend, logged unconditionally (ahead of every early `continue`
    // below, including the WiFi-down path) so it keeps running through
    // whatever state the device is in. Added to chase a report of the
    // device refusing new HTTP connections after a few days of uptime with
    // no trace captured from the actual failure — this is deliberately
    // simple (free/max-alloc heap + uptime) rather than trying to walk
    // lwIP's internal TCP PCB list, which isn't exposed through a stable
    // Arduino-layer API. A steadily falling free-heap trend over days would
    // confirm a heap leak; a flat trend alongside the same symptom would
    // point instead at TCP connection/PCB exhaustion, which doesn't show up
    // in heap stats at all. Either result narrows the next step.
    if (millis() - lastDiagLog >= 300000) {
      lastDiagLog = millis();
      log_i("Diag: uptime=%lus freeHeap=%u maxAlloc=%u",
            (unsigned long)(millis() / 1000), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }

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

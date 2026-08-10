#include "network.h"
#include <ESPmDNS.h>
#include <InfluxDbClient.h> // for timeSync() — NTP sync independent of InfluxDB configuration

#ifndef OTA_VERSION
  #define OTA_VERSION "local_development"
#endif

#define MDNS_HOSTNAME "kilnmonitor" // reachable at http://kilnmonitor.local

namespace {
const char FALLBACK_INDEX_HTML[] = R"rawl(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>KilnMonitor</title>
  <style>body{font-family:Arial,sans-serif;margin:2rem;line-height:1.5;}code{background:#eee;padding:0.2rem 0.4rem;border-radius:0.25rem;}</style>
</head>
<body>
  <h1>KilnMonitor Web Interface</h1>
  <p>The device is responding, but the full UI files were not found on LittleFS.</p>
  <p>Try uploading the web assets with <code>pio run --target uploadfs</code> for the complete interface.</p>
  <p>For now, the device is reachable at <code>/wifi-manager</code> and <code>/getWifiStatus</code>.</p>
</body>
</html>
)rawl";

const char FALLBACK_WIFI_HTML[] = R"rawl(
<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Wi-Fi Manager</title></head>
<body><h1>Wi-Fi Manager</h1><p>The web interface is running.</p></body>
</html>
)rawl";

void sendFileOrFallback(AsyncWebServerRequest* request, fs::FS& fs, const char* path, const char* contentType, const char* fallbackHtml) {
  if (fs.exists(path)) {
    request->send(fs, path, contentType);
    return;
  }

  log_w("File %s not found on LittleFS; serving fallback content", path);
  request->send(200, contentType, fallbackHtml);
}
}  // namespace

MyNetwork::MyNetwork(SemaphoreHandle_t& mutex, fs::FS& fileSystem)
: server(80), sharedMutex(mutex), fileSystem(fileSystem) {

}

//******************************************************************************************************************************
// WiFi related functions
//******************************************************************************************************************************

// Checks WiFi connection, connects and updates global variable
bool MyNetwork::checkWiFi() {
  static bool firstConnection = true;
  bool connected = false;

  if (!captive_mode)
  {
    if (receivedCredentials) {
      log_i("Credentials changed. Initializing WiFi again.\n");
      receivedCredentials = false;
      loadWifiCredentials();
      log_i("WiFi credentials loaded. Attempting to connect...\n");
      log_i("SSID &s, Password &s\n", ssid.c_str(), password.c_str());

      if (pendingCaptiveExit) {
        // Switch to combined AP+STA so we can attempt STA while AP stays alive
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("ESP32 Kiln Monitor", NULL);
      }
    }

    // Attempt WiFi connection
    xSemaphoreTake(mutex, portMAX_DELAY);
    g_connecting = true;
    xSemaphoreGive(mutex);

    for (uint8_t attempt = 0; attempt < 3; attempt++) {
      if (wifiMulti.run() != WL_CONNECTED) {
        log_i(".");
        firstConnection = true;
        delay(300);
      }
      else {
        connected = true;
        if (firstConnection) {
          firstConnection = false;
          log_i("Connected to: %s\nIP address: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
          if (!server_started) {
            setupServer();
            log_i("Web server started at http://%s\n", WiFi.localIP().toString().c_str());
          }
          // Re-run on every reconnect (not gated by server_started) since the
          // ESP32 mDNS responder is known to stop answering after a Wi-Fi
          // drop/reconnect cycle unless MDNS.begin() is called again.
          if (MDNS.begin(MDNS_HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            log_i("mDNS responder started: http://%s.local\n", MDNS_HOSTNAME);
          } else {
            log_w("mDNS responder failed to start\n");
          }

          // NTP sync on every (re)connect, independent of InfluxDB setup —
          // otherwise the clock never syncs until InfluxDB is configured.
          // Falls back to UTC if no InfluxDB timezone has been set yet.
          xSemaphoreTake(mutex, portMAX_DELAY);
          String tzInfo = g_influxConfig.configured ? g_influxConfig.tzInfo : String("UTC0");
          xSemaphoreGive(mutex);
          timeSync(tzInfo.c_str(), "pool.ntp.org", "time.nis.gov");
        }
        break;
      }
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    g_connecting = false;
    xSemaphoreGive(mutex);

    //log_i("WiFi connection check complete. Connected: %s\n", connected ? "Yes" : "No");
    if (pendingCaptiveExit) {
      pendingCaptiveExit = false;
      if (connected) {
        // Successfully connected — shut down the AP and DNS
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        log_i("Captive portal exited — connected to %s at %s\n",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      } else {
        // Failed — go back to captive mode
        captive_mode = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32 Kiln Monitor", NULL);
        log_w("WiFi connection failed — returning to captive mode\n");
      }
    } else if (connected) {
      consecutiveFailures_ = 0;
    } else {
      // No pending reconfiguration in progress — count normal connection failures
      // (no saved credentials, AP out of range, wrong password, etc.) and fall
      // back to AP mode so the device stays reachable for configuration.
      consecutiveFailures_++;
      if (consecutiveFailures_ >= AUTO_AP_FAILURE_THRESHOLD) {
        consecutiveFailures_ = 0;
        captive_mode = true;
        log_w("WiFi connection failed %d times in a row — starting AP mode\n", AUTO_AP_FAILURE_THRESHOLD);
        StartCaptivePortal();
      }
    }

  }  // end if (!captive_mode)

  xSemaphoreTake(mutex, portMAX_DELAY);
  g_connected = connected;
  xSemaphoreGive(mutex);

  return connected;
}

// loadWifiCredentials() - loads WiFi credentials from JSON array to wifiMulti
void MyNetwork::loadWifiCredentials() {
  WiFi.mode(WIFI_STA);

  // Clear existing WiFiMulti access points
  wifiMulti.~WiFiMulti(); // Call the destructor to clear the internal list
  new (&wifiMulti) WiFiMulti(); // Reconstruct the WiFiMulti object

  // Read WiFi credentials from JSON file
  JsonDocument json;
  log_i("Loading WiFi credentials from /wifi_credentials.json\n");
  parseJson(json, "/wifi_credentials.json");

  // Loop through each credential set
  JsonArray array = json.as<JsonArray>();
  for (JsonObject cred : array) {
    const char* ssid = cred["ssid"];
    const char* password = cred["password"];
    wifiMulti.addAP(ssid, password);
  }
}

//  getWifiQuality() - Returns the WiFi quality in percentage
int8_t MyNetwork::getWifiQuality() {
  int32_t dbm = WiFi.RSSI();
  if (dbm <= -100) {
    return 0;
  } else if (dbm >= -50) {
    return 100;
  } else {
    return 2 * (dbm + 100);
  }
}

//******************************************************************************************************************************
// Server related functions
//******************************************************************************************************************************

// HTTP handlers (get and post requests)
void MyNetwork::setupServer() {
  if (server_started) {
    return;
  }

  log_i("Starting AsyncWebServer on port 80\n");

  // Explicit root and health handlers so the device answers immediately on HTTP.
  server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/index.html", "text/html", FALLBACK_INDEX_HTML);
  });

  server.on("/health", HTTP_GET, [this](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "KilnMonitor HTTP OK");
  });

  // Route for WiFi manager config page
  server.on("/wifi-manager", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/wifimanager.html", "text/html", FALLBACK_WIFI_HTML);
  });

  // Returns current WiFi connection status and IP address for the captive portal connect flow
  server.on("/getWifiStatus", HTTP_GET, [this](AsyncWebServerRequest* request) {
    bool connected = (WiFi.status() == WL_CONNECTED);
    JsonDocument json;
    json["connected"] = connected;
    if (connected) json["ip"] = WiFi.localIP().toString();
    String output;
    serializeJson(json, output);
    request->send(200, "application/json", output);
  });

  // Route for the InfluxDB manager config page
  server.on("/influxdb-manager", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/influxdb-manager.html", "text/html", FALLBACK_INDEX_HTML);
  });

  // Returns current InfluxDB credentials as JSON for form pre-fill.
  // Token is intentionally omitted to avoid exposing it over the open AP.
  // Kiln name isn't actually an InfluxDB credential — it's stored separately
  // in NVS — but it's bundled into this response since it's shown on the
  // same form.
  server.on("/getInfluxCredentials", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument json;
    parseJson(json, "/influxdb_credentials.json");
    json.remove("token"); // never send the token back to the browser
    xSemaphoreTake(sharedMutex, portMAX_DELAY);
    json["kilnName"] = g_kilnName;
    xSemaphoreGive(sharedMutex);
    String output;
    serializeJson(json, output);
    request->send(200, "application/json", output);
  });

  // Route for the firmware update page
  server.on("/firmware-update", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/firmware-update.html", "text/html", FALLBACK_INDEX_HTML);
  });

  // Returns current OTA status as JSON for the firmware update page to poll
  server.on("/getFirmwareStatus", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument json;
    json["currentVersion"] = String(OTA_VERSION);
    xSemaphoreTake(sharedMutex, portMAX_DELAY);
    json["latestVersion"] = g_ota_latest_version;
    json["latestTag"]     = g_ota_latest_tag;
    OtaStatus s = g_ota_status;
    xSemaphoreGive(sharedMutex);

    const char* statusStr = "idle";
    if      (s == OtaStatus::CHECKING)          statusStr = "checking";
    else if (s == OtaStatus::UPDATE_AVAILABLE)  statusStr = "update_available";
    else if (s == OtaStatus::UP_TO_DATE)        statusStr = "up_to_date";
    else if (s == OtaStatus::UPDATING)          statusStr = "updating";
    else if (s == OtaStatus::ERROR)             statusStr = "error";
    json["status"] = statusStr;

    String output;
    serializeJson(json, output);
    request->send(200, "application/json", output);
  });

  // Triggers a firmware update check (OTA task polls g_ota_status)
  server.on("/checkFirmwareUpdate", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!g_connected) {
      request->send(400, "application/json", "{\"error\":\"Not connected to internet\"}");
      return;
    }
    xSemaphoreTake(sharedMutex, portMAX_DELAY);
    g_ota_status = OtaStatus::CHECKING;
    xSemaphoreGive(sharedMutex);
    request->send(200, "application/json", "{\"status\":\"checking\"}");
  });

  // Triggers OTA installation when an update is available
  server.on("/performOTA", HTTP_POST, [this](AsyncWebServerRequest* request) {
    xSemaphoreTake(sharedMutex, portMAX_DELAY);
    bool ready = (g_ota_status == OtaStatus::UPDATE_AVAILABLE);
    if (ready) g_ota_status = OtaStatus::UPDATING;
    xSemaphoreGive(sharedMutex);
    if (!ready) {
      request->send(400, "application/json", "{\"error\":\"No update available\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"updating\"}");
  });

  // Live status: zone readings + logging state, for a status web page
  server.on("/getStatus", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument json;
    xSemaphoreTake(sharedMutex, portMAX_DELAY);
    JsonArray zones = json["zones"].to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
      JsonObject z = zones.add<JsonObject>();
      z["active"] = g_zones[i].active;
      z["fault"]  = g_zones[i].fault;
      z["pv"]     = g_zones[i].pv;
    }
    json["loggingActive"]   = g_loggingActive;
    json["ambientBaseline"] = g_ambientBaseline;
    json["tcType"]          = String(g_tcType);
    xSemaphoreGive(sharedMutex);
    String output;
    serializeJson(json, output);
    request->send(200, "application/json", output);
  });

// Retrieving available SSIDs
  server.on("/getSSIDList", HTTP_GET, [this](AsyncWebServerRequest *request) {
    log_i("getting SSIDs \n");
    request->send(200, "application/json", ssidList); // sent as a JSON array
  });

  // Retrieving WiFi credentials
  server.on("/wifi-manager", HTTP_POST, [this](AsyncWebServerRequest* request) {
    int params = request->params();
    for (int i = 0; i < params; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (p->isPost()) {
        // HTTP POST ssid value
        if (p->name() == "ssid") {
          ssid = p->value().c_str();
          log_i("Received SSID: %s\n", ssid);
        }
        // HTTP POST pass value
        if (p->name() == "password") {
          password = p->value().c_str();
          log_i("Received Password: %s\n", password);
        }
      }
    }

    // Add the new credentials to the JSON array
    if (!ssid.isEmpty() && !password.isEmpty()) {
        addWifiCredentials(ssid, password);
        receivedCredentials = true;
        captive_mode = false;
        pendingCaptiveExit = true;
    }

    request->send(200, "application/json", "{\"status\":\"connecting\"}");
  });

  // Saving InfluxDB credentials + kiln name
  server.on("/influxdb-manager", HTTP_POST, [this](AsyncWebServerRequest* request) {
    String url, token, org, bucket, tzInfo, kilnName;
    bool hasKilnName = false;
    int params = request->params();
    for (int i = 0; i < params; i++) {
      const AsyncWebParameter* p = request->getParam(i);
      if (p->isPost()) {
        if (p->name() == "url")      url      = p->value();
        if (p->name() == "token")    token    = p->value();
        if (p->name() == "org")      org      = p->value();
        if (p->name() == "bucket")   bucket   = p->value();
        if (p->name() == "tzInfo")   tzInfo   = p->value();
        if (p->name() == "kilnName") { kilnName = p->value(); hasKilnName = true; }
      }
    }
    if (!url.isEmpty() && !token.isEmpty()) {
      saveInfluxDbCredentials(url, token, org, bucket, tzInfo);
    }
    // Independent of the InfluxDB credentials above — a kiln name can be
    // set/changed on its own without touching the rest of this form.
    if (hasKilnName) {
      saveKilnName(kilnName);
    }
    sendFileOrFallback(request, fileSystem, "/index.html", "text/html", FALLBACK_INDEX_HTML);
  });

  // Redirect any request to the root to the configuration page (catch all route)
  server.onNotFound([this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/index.html", "text/html", FALLBACK_INDEX_HTML);
  });

  server.serveStatic("/", fileSystem, "/");

  if (!captiveHandlerAdded) {
    server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
    captiveHandlerAdded = true;
  }

  server.begin();
  server_started = true;
  log_i("Async web server started\n");
}

// Starts captive portal in AP mode
void MyNetwork::StartCaptivePortal() {
  log_i("Setting up AP Mode\n");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("ESP32 Kiln Monitor", NULL);
  log_i("AP IP address: %s\n", WiFi.softAPIP().toString().c_str());

  log_i("Starting DNS Server\n");
  dnsServer.start(53, "*", WiFi.softAPIP());

  if (!server_started) {
    log_i("Setting up Async WebServer\n");
    setupServer();
  }

  log_i("Done!\n");
}

// Generates a JSON array with the first 5 unique SSIDs
void MyNetwork::getSSIDs() {
  // Provide JSON response with dynamic values
  int networks = WiFi.scanNetworks();
  std::set<String> uniqueSSIDs;

  // Get the first 5 unique SSIDs
  for (int i = 0; i < min(5, networks); ++i) {
    String ssid_i = WiFi.SSID(i);
    // Check if the SSID is not repeated
    if (uniqueSSIDs.find(ssid_i) == uniqueSSIDs.end()) {
      uniqueSSIDs.insert(ssid_i);
    }
  }

  // Construct JSON array with empty values
  ssidList = "{";
  for (int i = 1; i <= 5; i++) {
    String ssidProperty = "SSID" + String(i);
    if (i > 1) {
      ssidList += ", ";
    }
    ssidList += "\"" + ssidProperty + "\":\"";
    if (i <= uniqueSSIDs.size()) {
      ssidList += *std::next(uniqueSSIDs.begin(), i - 1);
    }
    ssidList += "\"";
  }
  ssidList += "}";

  // log_i("Updated SSID list:\n %s \n ", ssidList.c_str());
}

// Adds or updates WiFi credentials in the JSON file
void MyNetwork::addWifiCredentials(const String& ssid, const String& password) {
  JsonDocument json;
  String fileName = "/wifi_credentials.json";
  parseJson(json, fileName);

  // Check if the SSID already exists in the JSON array and update the password if it does
  bool ssidFound = false;
  JsonArray array = json.as<JsonArray>();
  for (JsonObject cred : array) {
    if (cred["ssid"].as<String>() == ssid) {
      cred["password"] = password;  // Update password
      ssidFound = true;
      log_i("Updated existing credentials\n");
      break;
    }
  }

  // If SSID not found, append new credentials
  if (!ssidFound) {
    JsonObject newCred = json.add<JsonObject>();
    newCred["ssid"] = ssid;
    newCred["password"] = password;
    log_i("Added new credentials\n");
  }

  // Save the updated credentials back to the file
  fs::File credentialsFile = fileSystem.open(fileName, FILE_WRITE);
  if (!credentialsFile) {
    log_i("Failed to open config file for writing\n");
  }

  if (!serializeJson(json, credentialsFile)) {
    log_i("Failed to write to file\n");
  }
  serializeJsonPretty(json, Serial);
  Serial.println();
  credentialsFile.close();
}

// Opens .json file and parses it to JSON document object
void MyNetwork::parseJson(JsonDocument& json, const String& path) {
  fs::File file = fileSystem.open(path, FILE_READ);
  if (!file) {
    log_i("- failed to open file for reading\n");
    return;
  }

  DeserializationError error = deserializeJson(json, file);
  if (error) {
    log_i("Failed to parse JSON, creating new JSON array\n");
    json.clear();
    return;
  }

  log_i("\n %s \n", path.c_str());
  serializeJsonPretty(json, Serial);
  Serial.println();
  file.close();
}

// loadInfluxDbCredentials() - loads InfluxDB credentials from JSON file into g_influxConfig
void MyNetwork::loadInfluxDbCredentials() {
  JsonDocument json;
  parseJson(json, "/influxdb_credentials.json");

  if (json.isNull() || !json["url"].is<const char*>()) {
    log_i("No InfluxDB credentials found. Publishing disabled until configured.\n");
    g_influxConfig.configured = false;
    return;
  }

  g_influxConfig.url      = json["url"]    | "";
  g_influxConfig.token    = json["token"]  | "";
  g_influxConfig.org      = json["org"]    | "";
  g_influxConfig.bucket   = json["bucket"] | "";
  g_influxConfig.tzInfo   = json["tzInfo"] | "UTC0";
  g_influxConfig.configured = !g_influxConfig.url.isEmpty() && !g_influxConfig.token.isEmpty();
  log_i("InfluxDB credentials loaded. URL: %s\n", g_influxConfig.url.c_str());
}

// saveInfluxDbCredentials() - saves InfluxDB credentials as JSON to LittleFS
void MyNetwork::saveInfluxDbCredentials(const String& url, const String& token, const String& org, const String& bucket, const String& tzInfo) {
  JsonDocument json;
  json["url"]    = url;
  json["token"]  = token;
  json["org"]    = org;
  json["bucket"] = bucket;
  json["tzInfo"] = tzInfo;

  fs::File file = fileSystem.open("/influxdb_credentials.json", FILE_WRITE);
  if (!file) {
    log_i("Failed to open influxdb_credentials.json for writing\n");
    return;
  }
  if (!serializeJson(json, file)) {
    log_i("Failed to write InfluxDB credentials\n");
  }
  serializeJsonPretty(json, Serial);
  Serial.println();
  file.close();

  // Update live config and signal the database task — guarded to prevent cross-core data race
  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  g_influxConfig.url      = url;
  g_influxConfig.token    = token;
  g_influxConfig.org      = org;
  g_influxConfig.bucket   = bucket;
  g_influxConfig.tzInfo   = tzInfo;
  g_influxConfig.configured = true;
  receivedInfluxCredentials = true;
  xSemaphoreGive(sharedMutex);
}

// saveKilnName() - persists the kiln's display name to NVS (same
// "kilnmonitor" namespace as TC type/temp scale) and updates the live
// value telemetry.cpp tags onto InfluxDB points. Falls back to the default
// device-derived name rather than allowing empty — see defaultKilnName()
// in EKcommon.h for why g_kilnName must never be empty.
void MyNetwork::saveKilnName(const String& name) {
  String resolved = name.isEmpty() ? defaultKilnName() : name;
  preferences.putString("kilnName", resolved);

  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  g_kilnName = resolved;
  xSemaphoreGive(sharedMutex);
}

bool MyNetwork::hasNewInfluxCredentials() const {
  return receivedInfluxCredentials;
}

void MyNetwork::clearInfluxCredentialsFlag() {
  receivedInfluxCredentials = false;
}

// Changes the captive mode, called from GUI
void MyNetwork::handleCaptiveModeToggle() {
  if (!captive_mode) {
    captive_mode = true;
    StartCaptivePortal();
    lastSSIDUpdate = millis() - 15000;
  }
  else {
    captive_mode = false;
    // Server continues running; accessible on STA IP once WiFi reconnects
  }
}

// Handles server when in captive mode
void MyNetwork::handleCaptiveMode() {
  dnsServer.processNextRequest();
  delay(10);
  if (millis() - lastSSIDUpdate > 15000) {
    getSSIDs();
    lastSSIDUpdate = millis();
  }
}

// Returns captive mode status
bool MyNetwork::get_captive_mode() const {
  return captive_mode;
}

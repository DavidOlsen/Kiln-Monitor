#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFiMulti.h>
#include <FS.h>
#include <LittleFS.h>
#include <set>

#include "EKcommon.h"
#include "userSetup.h"

class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request) { return true; }

  void handleRequest(AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
    Serial.println("Client connected");
  }
};

class MyNetwork {
public:

  MyNetwork(SemaphoreHandle_t& mutex, fs::FS& fileSystem);

  void setupServer();
  void StartCaptivePortal();
  void getSSIDs();
  void handleCaptiveMode();
  void handleCaptiveModeToggle();
  void loadWifiCredentials();
  void addWifiCredentials(const String& ssid, const String& password);
  void loadInfluxDbCredentials();
  void saveInfluxDbCredentials(const String& url, const String& token, const String& org, const String& bucket, const String& tzInfo);
  void saveKilnName(const String& name);
  void parseJson(JsonDocument& json, const String& path);

  int8_t getWifiQuality();

  bool checkWiFi();
  bool get_captive_mode() const;
  bool hasNewInfluxCredentials() const;
  void clearInfluxCredentialsFlag();

  // Releases the query-in-progress guard taken by handleGetSessions()/
  // handleDownloadSession(). Public because the CSV download's streaming
  // state (network.cpp, anonymous namespace) releases it from its own
  // destructor, well after the handler function that started it has returned.
  void releaseQueryLock();

private:
  void handleGetSessions(AsyncWebServerRequest* request);
  void handleDownloadSession(AsyncWebServerRequest* request);

  // Tries to take the query-in-progress guard. Returns false if genuinely
  // busy. Also self-heals: if the existing hold is older than a query could
  // plausibly still be legitimately running, it's treated as leaked (e.g.
  // from a response object that never got torn down) and forced clear rather
  // than wedging the whole feature until reboot.
  bool tryAcquireQueryLock();

  SemaphoreHandle_t& sharedMutex;
  fs::FS& fileSystem;

  AsyncWebServer server;
  DNSServer dnsServer;
  WiFiMulti wifiMulti;
  JsonDocument jsonDocument;

  CaptiveRequestHandler captiveRequestHandler;

  String ssidList;
  String ssid;
  String password;
  bool captive_mode = false;
  bool server_started = false;
  bool captiveHandlerAdded = false;
  bool receivedCredentials = false;
  bool receivedInfluxCredentials = false;
  bool pendingCaptiveExit = false;
  unsigned long lastSSIDUpdate;

  // Guards against overlapping InfluxDB query requests (session list/download) —
  // each one spins up its own TLS client, and two concurrent TLS sessions on
  // top of the write-path client is more heap than this device can spare.
  bool queryInProgress_ = false;
  unsigned long queryLockAcquiredAt_ = 0;

  // Number of consecutive failed connection attempts before auto-falling back to AP mode
  static constexpr uint8_t AUTO_AP_FAILURE_THRESHOLD = 3;
  uint8_t consecutiveFailures_ = 0;
};

#endif

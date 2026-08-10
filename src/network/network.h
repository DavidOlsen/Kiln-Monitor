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

private:
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

  // Number of consecutive failed connection attempts before auto-falling back to AP mode
  static constexpr uint8_t AUTO_AP_FAILURE_THRESHOLD = 3;
  uint8_t consecutiveFailures_ = 0;
};

#endif

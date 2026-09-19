#include "network.h"
#include <ESPmDNS.h>
#include <InfluxDbClient.h> // for timeSync() — NTP sync independent of InfluxDB configuration
#include <InfluxDbCloud.h>  // InfluxDbCloud2CACert, used by the session-list query client below
#include <HTTPClient.h>     // used directly (not via the InfluxDB client lib) for the CSV download — see SessionCsvStream
#include <memory>
#include <vector>

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

// Escapes a value for interpolation inside a Flux double-quoted string
// literal. Used instead of the InfluxDB client library's `params` query
// binding, which this device's InfluxDB instance rejects server-side with
// "undefined identifier params" — apparently unsupported on this deployment.
// Safe here because the only values ever passed through this are KilnName
// and bucket (from this device's own stored config, never the HTTP request)
// and a digit-only session id validated before it ever reaches this function.
String fluxEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

// Builds a Flux query for exactly one field's series within one session.
// Deliberately as simple as possible: a single already-time-ordered series,
// no pivot(), no group(), no cross-field merge/sort — see SessionCsvStream's
// comment for why every one of those turned out to cost something on this
// project's InfluxDB instance.
// Minimal JSON-string escaper for embedding the Flux query into the POST
// body InfluxDB's /api/v2/query expects. Distinct from fluxEscape() above —
// that one escapes for a Flux string *literal*; this one escapes for the
// *outer* JSON envelope the whole query string sits inside.
String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
    }
  }
  return out;
}

// Minimal percent-encoding for the org name in the query URL's query string.
// KilnName/bucket go through fluxEscape() instead since those are embedded
// in the Flux query body, not the URL.
String urlEncode(const String& value) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// Reads an HTTP response body directly off a WiFiClient, decoding chunked
// transfer-encoding itself, byte-counted throughout — never relying on
// Stream::readStringUntil()'s ambiguous "" return (which means both "hit a
// blank line" and "genuinely timed out", indistinguishably). That exact
// ambiguity, inside the InfluxDB client library's own HttpStreamScanner, is
// what every earlier version of this download's Flux query tripped over one
// way or another (see git history / the InfluxDB deployment notes for the
// three variants that were tried) — a blank line is completely routine in
// Flux's CSV format (it's the separator between result tables), so a reader
// that can't tell it apart from a stalled socket will eventually misfire on
// any large or multi-table response. Framing (chunk-size lines, the CRLF
// between chunks) is read with WiFiClient::readBytes(), which reports
// exactly how many bytes it actually got — fewer than requested is an
// unambiguous, genuine failure, unlike an empty string from readStringUntil.
class ChunkedBodyReader {
public:
  ChunkedBodyReader(WiFiClient* stream, bool chunked, long contentLength)
      : _stream(stream), _chunked(chunked), _remaining(contentLength) {}

  // Returns the next logical line of the (de-chunked) body, or an empty
  // string once nothing more will ever come — check error()/finished() to
  // tell a genuine read failure apart from a clean end of body.
  String readLine() {
    while (true) {
      int nl = _buffer.indexOf('\n');
      if (nl >= 0) {
        String line = _buffer.substring(0, nl);
        _buffer.remove(0, nl + 1);
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return line;
      }
      if (_finished || _error) return String();
      if (!fillMore()) return String(); // fillMore() sets _finished or _error
    }
  }

  bool finished() const { return _finished; }
  bool error() const { return _error; }

private:
  WiFiClient* _stream;
  bool _chunked;
  long _remaining;             // bytes left: in the current chunk (chunked) or whole body (not)
  bool _needChunkSizeLine = true;
  String _buffer;
  bool _finished = false;
  bool _error = false;

  // Reads one CRLF- or LF-terminated line straight off the socket, byte by
  // byte. Only used for chunk framing (chunk-size lines, the empty line
  // between chunks) — always short, so byte-at-a-time cost is negligible.
  bool readRawLine(String& out) {
    out = "";
    uint8_t c;
    while (true) {
      if (_stream->readBytes(&c, 1) != 1) return false;
      if (c == '\n') return true;
      if (c != '\r') out += (char)c;
    }
  }

  bool fillMore() {
    static const size_t READ_BUF = 256;
    uint8_t tmp[READ_BUF];

    if (_chunked) {
      if (_needChunkSizeLine) {
        String sizeLine;
        if (!readRawLine(sizeLine)) { _error = true; return false; }
        int semi = sizeLine.indexOf(';'); // chunk extensions, if any — ignored
        if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
        sizeLine.trim();
        long size = strtol(sizeLine.c_str(), nullptr, 16);
        if (size <= 0) { _finished = true; return false; } // the terminating 0-length chunk
        _remaining = size;
        _needChunkSizeLine = false;
      }
      size_t want = (_remaining < (long)READ_BUF) ? (size_t)_remaining : READ_BUF;
      size_t got = _stream->readBytes(tmp, want);
      if (got == 0) { _error = true; return false; }
      for (size_t i = 0; i < got; i++) _buffer += (char)tmp[i];
      _remaining -= got;
      if (_remaining == 0) {
        String trailer;
        if (!readRawLine(trailer)) { _error = true; return false; } // chunk's trailing CRLF
        _needChunkSizeLine = true;
      }
      return true;
    }

    // Not chunked — a plain Content-Length-bounded body.
    if (_remaining <= 0) { _finished = true; return false; }
    size_t want = (_remaining < (long)READ_BUF) ? (size_t)_remaining : READ_BUF;
    size_t got = _stream->readBytes(tmp, want);
    if (got == 0) { _error = true; return false; }
    for (size_t i = 0; i < got; i++) _buffer += (char)tmp[i];
    _remaining -= got;
    return true;
  }
};

// Drives one CSV download as a genuinely incremental HTTP chunked response
// back to the browser. Talks to InfluxDB directly via ESP32's own HTTPClient
// rather than the InfluxDB client library's query()/FluxQueryResult — see
// ChunkedBodyReader's comment for why. Emits "long" format (time,field,value
// — one row per raw point, grouped by field rather than interleaved by
// time), parsing Flux's annotated CSV format itself: lines starting with '#'
// are annotations (skipped), the next line is that table's column header
// (parsed to find the _time/_field/_value column positions), a blank line
// marks the boundary to the next table, and everything else is a data row.
//
// The first cut of this (AsyncResponseStream) built the entire CSV as one
// in-memory String before sending a byte — fine for the small JSON session
// list, but a multi-hour firing at 1 sample/sec is 100k+ rows, and that
// String's growth just silently stopped once the ESP32's ~300KB heap ran
// out, shipping a truncated-but-valid-looking file with no error anywhere.
// This still pulls exactly one row at a time and copies it into whatever
// buffer size ESPAsyncWebServer's chunked response hands us on each call,
// carrying any leftover across calls in `pending`, same as that fix.
//
// Owned via std::shared_ptr from the AwsResponseFiller lambda in
// handleDownloadSession() — that lambda outlives the handler function call
// (ESPAsyncWebServer invokes it repeatedly as the client is ready for more),
// so the HTTPClient/WiFiClient/queryInProgress_ lock all have to be released
// from this struct's destructor, once the response object itself is torn
// down, not from the end of the handler function.
struct SessionCsvStream {
  HTTPClient http;
  WiFiClient wifiClient;
  MyNetwork* network;
  ChunkedBodyReader* reader = nullptr;
  int timeCol = -1;
  std::vector<int> valueCols;    // indices (in the current table's header) of every column to emit, in header order
  std::vector<String> valueNames; // their names — becomes the CSV header once the first table's header line is seen
  bool inTable = false;   // whether the current table's header row has been seen
  bool headerWritten = false;
  bool done = false;
  bool hadError = false;
  String pending;
  size_t pendingOffset = 0;
  int rowCount = 0;

  SessionCsvStream(MyNetwork* n) : network(n) {}

  ~SessionCsvStream() {
    if (hadError) {
      log_e("handleDownloadSession: completed with errors, sent %d row(s) | free heap: %d, max alloc: %d",
            rowCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    } else {
      log_i("handleDownloadSession: sent %d row(s)", rowCount);
    }
    delete reader;
    http.end();
    network->releaseQueryLock();
  }

  // Issues the query and prepares the reader. Returns false on outright
  // request failure (bad response code) — the caller still needs to send
  // *some* HTTP response in that case, since no chunked response has been
  // started yet.
  //
  // pivot() reshapes the raw per-field points into one wide row per
  // timestamp, with a column per field (Zone1 temperature, Zone1 fault, ...)
  // — this is safe to use now in a way it wasn't earlier in this feature's
  // history: the failures previously blamed on pivot() being too expensive
  // for InfluxDB to compute were chased before two client-side bugs got
  // fixed (the InfluxDB library's setHTTPOptions() silently not applying our
  // configured timeout, and its HttpStreamScanner misreading a legitimate
  // blank line as a fatal timeout — see the InfluxDB deployment notes for
  // the full history). Both are gone now that this reads the response
  // itself via ChunkedBodyReader, so it's worth trusting pivot() again
  // rather than assuming it's still the problem.
  bool start(const InfluxDbConfig& cfg, const String& kilnName, const String& sessionId) {
    String url = cfg.url + "/api/v2/query?org=" + urlEncode(cfg.org);
    String flux =
      "from(bucket: \"" + fluxEscape(cfg.bucket) + "\")"
      " |> range(start: -5y)"
      " |> filter(fn: (r) => r._measurement == \"KILN MONITOR\" and r.KilnName == \"" + fluxEscape(kilnName) +
      "\" and r.SessionId == \"" + sessionId + "\")"
      " |> pivot(rowKey: [\"_time\"], columnKey: [\"_field\"], valueColumn: \"_value\")"
      " |> sort(columns: [\"_time\"])";
    String body =
      "{\"type\":\"flux\",\"query\":\"" + jsonEscape(flux) + "\","
      "\"dialect\":{\"annotations\":[\"datatype\"],\"dateTimeFormat\":\"RFC3339\","
      "\"header\":true,\"delimiter\":\",\",\"commentPrefix\":\"#\"}}";

    const char* headerKeys[] = {"Transfer-Encoding"};
    http.collectHeaders(headerKeys, 1);
    http.setTimeout(30000);
    http.setConnectTimeout(10000);
    if (!http.begin(wifiClient, url)) {
      log_e("handleDownloadSession: HTTPClient::begin failed for %s", url.c_str());
      hadError = true;
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + cfg.token);

    int code = http.POST(body);
    if (code != 200) {
      log_e("handleDownloadSession: query POST failed, status %d: %s", code, http.getString().c_str());
      hadError = true;
      return false;
    }

    bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    reader = new ChunkedBodyReader(http.getStreamPtr(), chunked, http.getSize());
    return true;
  }

  static std::vector<String> splitCsvLine(const String& line) {
    std::vector<String> cols;
    int start = 0;
    while (start <= (int)line.length()) {
      int comma = line.indexOf(',', start);
      cols.push_back((comma < 0) ? line.substring(start) : line.substring(start, comma));
      if (comma < 0) break;
      start = comma + 1;
    }
    return cols;
  }

  // Finds _time plus every other real column (temperature/fault/Ambient —
  // whichever pivot() produced for this session) in a table's header line,
  // skipping Flux's own bookkeeping columns and the tags, which aren't
  // useful in the CSV (constant for the whole file, or redundant with it).
  void parseHeaderLine(const String& line) {
    static const std::set<String> skip = {
      "result", "table", "_start", "_stop", "_measurement", "KilnName", "SessionId"
    };
    timeCol = -1;
    valueCols.clear();
    valueNames.clear();
    std::vector<String> cols = splitCsvLine(line);
    for (size_t i = 0; i < cols.size(); i++) {
      if (cols[i] == "_time") {
        timeCol = (int)i;
      } else if (!cols[i].isEmpty() && skip.find(cols[i]) == skip.end()) {
        valueCols.push_back((int)i);
        valueNames.push_back(cols[i]);
      }
    }
  }

  // Builds one wide CSV row (time + every value column) from a data line, by
  // position, using the most recently parsed header line. Returns false if
  // the row is shorter than expected (skipped as malformed rather than
  // treated as fatal).
  bool parseDataLine(const String& line, String& rowOut) {
    if (timeCol < 0) return false;
    std::vector<String> cols = splitCsvLine(line);
    if (timeCol >= (int)cols.size()) return false;
    rowOut = cols[timeCol];
    for (size_t i = 0; i < valueCols.size(); i++) {
      rowOut += ",";
      if (valueCols[i] < (int)cols.size()) rowOut += cols[valueCols[i]];
    }
    return true;
  }

  // Advances through the response until the next real data row, updating
  // column positions whenever a new table's header line is encountered.
  // Returns false only once the body is genuinely exhausted (clean end or a
  // real read error — either way, the destructor logs which).
  bool nextRow(String& rowOut) {
    while (true) {
      String line = reader->readLine();
      if (line.isEmpty()) {
        // readLine() returns "" for two different reasons that must not be
        // conflated: a genuinely empty *logical* line (Flux's separator
        // between result tables) versus true end-of-body. Only the reader's
        // own finished()/error() flags can tell them apart — treating every
        // "" as end-of-data here silently stopped the whole download after
        // the first table.
        if (reader->finished() || reader->error()) {
          if (reader->error()) hadError = true;
          return false; // genuinely nothing left
        }
        inTable = false; // a real blank line — boundary to the next table
        continue;
      }
      if (line.startsWith("#")) { inTable = false; continue; } // annotation line — a table is starting
      if (!inTable) { parseHeaderLine(line); inTable = true; continue; } // this table's header row
      if (parseDataLine(line, rowOut)) {
        rowCount++;
        return true;
      }
      // Malformed row — skip.
    }
  }

  void advance() {
    if (!headerWritten) {
      headerWritten = true;
      // The CSV header depends on which fields pivot() actually produced —
      // only known once the first table's header line has been parsed,
      // which happens as a side effect of pulling the first data row. So
      // pull that row now and emit both lines together.
      String firstRow;
      if (nextRow(firstRow)) {
        String header = "time";
        for (size_t i = 0; i < valueNames.size(); i++) { header += ","; header += valueNames[i]; }
        header += "\n";
        pending = header + firstRow + "\n";
      } else {
        pending = "time\n"; // no rows matched — still a minimal valid CSV
      }
      return;
    }
    String row;
    if (nextRow(row)) {
      pending = row + "\n";
    } else {
      pending = "";
      done = true;
    }
  }

  size_t fill(uint8_t* buf, size_t maxLen) {
    size_t written = 0;
    while (written < maxLen) {
      if (pendingOffset < (size_t)pending.length()) {
        size_t avail = pending.length() - pendingOffset;
        size_t toCopy = (maxLen - written < avail) ? (maxLen - written) : avail;
        memcpy(buf + written, pending.c_str() + pendingOffset, toCopy);
        written += toCopy;
        pendingOffset += toCopy;
        continue;
      }
      if (done) break;
      pendingOffset = 0;
      advance();
    }
    return written;
  }
};
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

  // Route for the firing sessions page
  server.on("/sessions", HTTP_GET, [this](AsyncWebServerRequest* request) {
    sendFileOrFallback(request, fileSystem, "/sessions.html", "text/html", FALLBACK_INDEX_HTML);
  });

  // Lists this device's firing sessions (start/end time, point count) pulled from
  // InfluxDB. Always scoped server-side to this device's own KilnName tag — the
  // request cannot influence which kiln's data is queried.
  server.on("/getSessions", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleGetSessions(request);
  });

  // Streams one firing session's data back as a CSV download, again scoped to
  // this device's own KilnName tag; the only client-supplied value is the
  // session id, validated as digits-only before it's used.
  server.on("/downloadSession", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleDownloadSession(request);
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
    json["tempScale"]       = String(g_tempScale);
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

  // This project never ships pre-gzipped assets, so the library's default
  // gzip-first probe just costs a failed LittleFS open() on every static
  // request — which vfs_api.cpp itself logs at [E], drowning out real
  // errors in the serial monitor for no benefit.
  server.serveStatic("/", fileSystem, "/").setTryGzipFirst(false);

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

// Lists this device's firing sessions: one row per SessionId tag value seen
// under this device's own KilnName, with first/last timestamp and point count.
// Grouped/aggregated in Flux rather than pulled point-by-point, since a run can
// be tens of thousands of points and this device doesn't have room to buffer that.
void MyNetwork::handleGetSessions(AsyncWebServerRequest* request) {
  log_i("handleGetSessions: request received");

  InfluxDbConfig cfg;
  String kilnName;
  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  cfg = g_influxConfig;
  kilnName = g_kilnName;
  xSemaphoreGive(sharedMutex);

  if (!cfg.configured) {
    request->send(400, "application/json", "{\"error\":\"InfluxDB not configured\"}");
    return;
  }

  if (!tryAcquireQueryLock()) {
    request->send(503, "application/json", "{\"error\":\"Another query is already in progress\"}");
    return;
  }

  InfluxDBClient client(cfg.url.c_str(), cfg.org.c_str(), cfg.bucket.c_str(), cfg.token.c_str(), InfluxDbCloud2CACert);
  // validateConnection() forces the client's internal _service/HTTPClient to
  // exist (a cheap /health GET) before we touch options — the library's
  // setHTTPOptions() silently no-ops until that object exists, so calling it
  // any earlier than this would have configured nothing. Default read
  // timeout (5s) is tuned for small write/status calls — an aggregate over
  // every point this kiln has ever logged is a bigger ask.
  client.validateConnection();
  client.setHTTPOptions(HTTPOptions().httpReadTimeout(30000));

  // KilnName and bucket come from this device's own stored config, never from
  // the request, so a client can't point the query at another kiln's data.
  // fluxEscape() makes it safe to inline them even though a kiln name is
  // free-text the user could type a quote character into.
  String flux =
    "from(bucket: \"" + fluxEscape(cfg.bucket) + "\")"
    " |> range(start: -5y)"
    " |> filter(fn: (r) => r._measurement == \"KILN MONITOR\" and r.KilnName == \"" + fluxEscape(kilnName) + "\" and r._field == \"Ambient\")"
    " |> group(columns: [\"SessionId\"])"
    " |> sort(columns: [\"_time\"])"
    " |> reduce("
    "     fn: (r, accumulator) => ({"
    "       count: accumulator.count + 1,"
    "       first: if accumulator.count == 0 then r._time else accumulator.first,"
    "       last: r._time"
    "     }),"
    "     identity: {count: 0, first: time(v: 0), last: time(v: 0)}"
    "   )";

  log_i("handleGetSessions: querying %s (org=%s bucket=%s)", cfg.url.c_str(), cfg.org.c_str(), cfg.bucket.c_str());
  FluxQueryResult result = client.query(flux);
  log_i("handleGetSessions: query() returned, reading rows. Free heap: %d", ESP.getFreeHeap());

  JsonDocument json;
  JsonArray sessions = json["sessions"].to<JsonArray>();
  int rowCount = 0;
  while (result.next()) {
    JsonObject s = sessions.add<JsonObject>();
    s["id"]        = result.getValueByName("SessionId").getString().toInt();
    s["startTime"] = result.getValueByName("first").getRawValue();
    s["endTime"]   = result.getValueByName("last").getRawValue();
    s["points"]    = result.getValueByName("count").getLong();
    rowCount++;
  }
  String err = result.getError();
  result.close();
  releaseQueryLock();

  if (!err.isEmpty()) {
    log_e("handleGetSessions: query failed after %d row(s): %s", rowCount, err.c_str());
    request->send(502, "application/json", "{\"error\":\"Query failed\"}");
    return;
  }

  log_i("handleGetSessions: sending %d session(s)", rowCount);
  String output;
  serializeJson(json, output);
  request->send(200, "application/json", output);
}

// Streams one firing session back as a CSV download. Scoped to this device's
// own KilnName the same way handleGetSessions() is; the session id is the only
// value the request contributes to the query, and it's validated as digits-only
// up front (see fluxEscape() above for why that's the safety net here rather
// than the InfluxDB client's `params` binding).
void MyNetwork::handleDownloadSession(AsyncWebServerRequest* request) {
  if (!request->hasParam("id")) {
    request->send(400, "application/json", "{\"error\":\"Missing id parameter\"}");
    return;
  }
  String idParam = request->getParam("id")->value();
  if (idParam.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"Invalid id\"}");
    return;
  }
  for (size_t i = 0; i < idParam.length(); i++) {
    if (!isDigit(idParam[i])) {
      request->send(400, "application/json", "{\"error\":\"Invalid id\"}");
      return;
    }
  }

  InfluxDbConfig cfg;
  String kilnName;
  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  cfg = g_influxConfig;
  kilnName = g_kilnName;
  xSemaphoreGive(sharedMutex);

  if (!cfg.configured) {
    request->send(400, "application/json", "{\"error\":\"InfluxDB not configured\"}");
    return;
  }

  if (!tryAcquireQueryLock()) {
    request->send(503, "application/json", "{\"error\":\"Another query is already in progress\"}");
    return;
  }

  // idParam was validated as digits-only above, so it's safe to inline
  // unescaped in SessionCsvStream::start() — it can't contain a quote
  // character to break out of the Flux string.
  log_i("handleDownloadSession: querying session %s", idParam.c_str());

  // Ownership: shared between every copy std::function makes of this lambda,
  // and released only in ~SessionCsvStream() — see the struct's comment above
  // for why cleanup can't live at the end of this function.
  auto stream = std::make_shared<SessionCsvStream>(this);
  if (!stream->start(cfg, kilnName, idParam)) {
    request->send(502, "application/json", "{\"error\":\"Query failed\"}");
    return; // stream's shared_ptr refcount drops to 0 here, running its cleanup
  }

  AsyncWebServerResponse* response = request->beginChunkedResponse(
    "text/csv",
    [stream](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
      return stream->fill(buf, maxLen);
    });
  if (response == nullptr) {
    request->send(500, "application/json", "{\"error\":\"Out of memory\"}");
    return; // stream's shared_ptr refcount drops to 0 here, running its cleanup
  }
  response->addHeader("Content-Disposition", "attachment; filename=\"session-" + idParam + ".csv\"");
  request->send(response);
}

bool MyNetwork::hasNewInfluxCredentials() const {
  return receivedInfluxCredentials;
}

void MyNetwork::clearInfluxCredentialsFlag() {
  receivedInfluxCredentials = false;
}

void MyNetwork::releaseQueryLock() {
  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  queryInProgress_ = false;
  xSemaphoreGive(sharedMutex);
}

bool MyNetwork::tryAcquireQueryLock() {
  // Generously longer than any legitimate session query should take — this
  // exists purely so a leaked lock (a response object ESPAsyncWebServer
  // never tore down, an unhandled edge case, etc.) self-heals instead of
  // wedging the whole feature until reboot.
  const unsigned long STALE_LOCK_MS = 3UL * 60UL * 1000UL;

  xSemaphoreTake(sharedMutex, portMAX_DELAY);
  bool busy = queryInProgress_;
  if (busy && (millis() - queryLockAcquiredAt_) > STALE_LOCK_MS) {
    log_w("Query lock held for over %lu ms with no release — treating as leaked, forcing it clear",
          (unsigned long)(millis() - queryLockAcquiredAt_));
    busy = false;
  }
  if (!busy) {
    queryInProgress_ = true;
    queryLockAcquiredAt_ = millis();
  }
  xSemaphoreGive(sharedMutex);
  return !busy;
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

#include <Preferences.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <time.h>

#include "userSetup.h"
#include "EKcommon.h"
#include "touch.h"

#include "../network/network.h"
#include "gui.h"

// Color Definitions
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_ORANGE  0xFD20
#define COLOR_BLUE    0x001F

// Palette matching the web UI (data/style.css --color-* variables), converted
// to RGB565. "_DIM" variants are pre-mixed dark tints (since this display has
// no alpha blending) used as badge/card backgrounds on the black canvas —
// the on-screen equivalent of the web's translucent badge fills.
#define COLOR_ACCENT      0x1414  // teal — primary buttons, matches --color-accent
#define COLOR_SUCCESS     0x1C49  // softer green — matches --color-success
#define COLOR_SUCCESS_DIM 0x0942  // dark green tint — "LOGGING ACTIVE" badge fill
#define COLOR_DANGER_DIM  0x3881  // dark red tint — faulted zone card fill
#define COLOR_CARD        0x39E9  // neutral dark gray — zone cards, WAITING badge, DONE button
#define COLOR_MUTED       0x6B90  // secondary/muted text

extern MyNetwork network;

static const char *TAG = "gui";

namespace {
  bool showingConfig = false; // false = status (home) screen, true = config screen

  bool touchDown = false;
  bool touchReleased = false;
  bool touchHandled = false;
  unsigned long touchHandledTime = 0;
  bool touchStartOnConfig = false;
  int16_t touchX = -1;
  int16_t touchY = -1;

  unsigned long topBar_start;
  uint32_t contentGeneration = 0;
}

Preferences preferences;
#define GFX_BL 27
// E32R40T Hardware Pins — same board as ElectricKiln
#define TFT_DC   2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_RST  -1 // Tied to ESP32 Reset (EN) pin

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *tft = new Arduino_ST7796(bus, TFT_RST, 1/* rotation */, false /* ips */);

int getTftTextWidth(String text, int size) {
    return text.length() * (6 * size);
}

static bool touchInRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    return touchX >= x && touchX < x + w && touchY >= y && touchY < y + h;
}

static constexpr int16_t GUI_TOP_BAR_HEIGHT = 20;
static constexpr int16_t GUI_ERROR_BAR_HEIGHT = 18;
static constexpr int16_t GUI_SIDE_MARGIN = 16;
static constexpr int16_t GUI_TOUCH_BUTTON_MIN_WIDTH = 220;
static constexpr int16_t GUI_TOUCH_BUTTON_MAX_WIDTH = 360;
static constexpr int16_t GUI_TOUCH_BUTTON_HEIGHT = 44;

static inline int16_t guiWidth() { return tft->width(); }
static inline int16_t guiHeight() { return tft->height(); }
static inline int16_t guiContentHeight() { return guiHeight() - GUI_TOP_BAR_HEIGHT - GUI_ERROR_BAR_HEIGHT; }
static inline int16_t guiContentTop() { return GUI_TOP_BAR_HEIGHT; }
static inline int16_t guiErrorBarY() { return guiHeight() - GUI_ERROR_BAR_HEIGHT; }
static inline int16_t guiLeftX() { return GUI_SIDE_MARGIN; }
static inline int16_t guiRightX(int16_t extra = 0) { return guiWidth() - GUI_SIDE_MARGIN - extra; }
static inline int16_t guiCenterX() { return guiWidth() / 2; }
static inline int16_t guiButtonWidth() {
    return min<int16_t>(max<int16_t>(GUI_TOUCH_BUTTON_MIN_WIDTH, guiWidth() / 3), GUI_TOUCH_BUTTON_MAX_WIDTH);
}
static inline int16_t guiButtonHeight() {
    return max<int16_t>(GUI_TOUCH_BUTTON_HEIGHT, guiHeight() / 12);
}

static inline int16_t guiMenuButtonY(int16_t row, int16_t spacing = 16) {
    return guiContentTop() + 40 + row * (guiButtonHeight() + spacing);
}
static inline int16_t guiMenuButtonX() {
    return guiCenterX() - guiButtonWidth() / 2;
}
static inline bool touchInMenuButton(int16_t row) {
    return touchInRect(guiMenuButtonX(), guiMenuButtonY(row), guiButtonWidth(), guiButtonHeight());
}
static int16_t touchMenuButtonIndex(int16_t maxRows) {
    for (int16_t row = 0; row < maxRows; row++) {
        if (touchInMenuButton(row)) return row;
    }
    return -1;
}

static inline void clearContentArea(uint16_t color) {
    tft->fillRect(0, GUI_TOP_BAR_HEIGHT, guiWidth(), guiContentHeight(), color);
    contentGeneration++;
}

// Direct-tap menu button (no highlight state — every button executes
// immediately on tap, there's no up/down cursor to reflect). Redraws only
// when its content actually changes since the last clearContentArea(), so
// it doesn't flicker while polled every gui_run() pass. Filled rounded
// button matches the web UI's solid-color button style; pass bg=COLOR_CARD
// for a secondary/neutral action (e.g. DONE) vs. the default accent fill.
static void drawMenuButton(const String &label, int16_t row, uint16_t bg = COLOR_ACCENT) {
    static constexpr int16_t MAX_CACHED_ROWS = 6;
    static uint32_t cacheGen[MAX_CACHED_ROWS] = {0};
    static String cacheLabel[MAX_CACHED_ROWS];
    static uint16_t cacheBg[MAX_CACHED_ROWS] = {0};
    static bool cacheValid[MAX_CACHED_ROWS] = {false};

    bool cacheable = row >= 0 && row < MAX_CACHED_ROWS;
    if (cacheable && cacheValid[row] && cacheGen[row] == contentGeneration &&
        cacheLabel[row] == label && cacheBg[row] == bg) {
      return;
    }

    int16_t buttonWidth = guiButtonWidth();
    int16_t buttonHeight = guiButtonHeight();
    int16_t x = guiMenuButtonX();
    int16_t y = guiMenuButtonY(row);

    tft->fillRoundRect(x, y, buttonWidth, buttonHeight, 8, bg);
    tft->setTextColor(COLOR_WHITE, bg);

    int16_t textWidth = getTftTextWidth(label, 2);
    tft->setTextSize(2);
    tft->setCursor(x + (buttonWidth - textWidth) / 2, y + (buttonHeight - 16) / 2);
    tft->print(label);

    if (cacheable) {
      cacheGen[row] = contentGeneration;
      cacheLabel[row] = label;
      cacheBg[row] = bg;
      cacheValid[row] = true;
    }
}

// --- Bottom action bar: 1-3 equal-width labeled buttons, tap to act directly ---
static constexpr int16_t GUI_ACTION_BAR_HEIGHT = 50;
static constexpr int16_t GUI_ACTION_BAR_GAP = 6;
static constexpr int16_t GUI_ACTION_BAR_BOTTOM_MARGIN = 10;

static inline int16_t guiActionBarY() { return guiErrorBarY() - GUI_ACTION_BAR_HEIGHT - GUI_ACTION_BAR_BOTTOM_MARGIN; }
static inline int16_t guiActionButtonWidth(int16_t slots) {
    return (guiWidth() - GUI_SIDE_MARGIN * 2 - GUI_ACTION_BAR_GAP * (slots - 1)) / slots;
}
static inline int16_t guiActionButtonX(int16_t slot, int16_t slots) {
    return guiLeftX() + slot * (guiActionButtonWidth(slots) + GUI_ACTION_BAR_GAP);
}
static bool touchInActionButton(int16_t slot, int16_t slots) {
    return touchInRect(guiActionButtonX(slot, slots), guiActionBarY(), guiActionButtonWidth(slots), GUI_ACTION_BAR_HEIGHT);
}
static void drawActionButton(const String &label, int16_t slot, int16_t slots, uint16_t bg = COLOR_ACCENT, uint16_t fg = COLOR_WHITE) {
    static constexpr int16_t MAX_CACHED_SLOTS = 4;
    static uint32_t cacheGen[MAX_CACHED_SLOTS] = {0};
    static String cacheLabel[MAX_CACHED_SLOTS];
    static uint16_t cacheBg[MAX_CACHED_SLOTS] = {0};
    static uint16_t cacheFg[MAX_CACHED_SLOTS] = {0};
    static bool cacheValid[MAX_CACHED_SLOTS] = {false};

    bool cacheable = slot >= 0 && slot < MAX_CACHED_SLOTS;
    if (cacheable && cacheValid[slot] && cacheGen[slot] == contentGeneration &&
        cacheLabel[slot] == label && cacheBg[slot] == bg && cacheFg[slot] == fg) {
      return;
    }

    int16_t x = guiActionButtonX(slot, slots);
    int16_t y = guiActionBarY();
    int16_t w = guiActionButtonWidth(slots);
    int16_t h = GUI_ACTION_BAR_HEIGHT;

    tft->fillRoundRect(x, y, w, h, 8, bg);

    tft->setTextSize(2);
    tft->setTextColor(fg, bg);
    int16_t textWidth = getTftTextWidth(label, 2);
    tft->setCursor(x + max<int16_t>(0, (w - textWidth) / 2), y + (h - 16) / 2);
    tft->print(label);

    if (cacheable) {
      cacheGen[slot] = contentGeneration;
      cacheLabel[slot] = label;
      cacheBg[slot] = bg;
      cacheFg[slot] = fg;
      cacheValid[slot] = true;
    }
}

// --- Bottom error bar: persistent reserved strip, shows which zone(s) are faulted ---
void disp_error_bar(const String &message) {
    static String lastMessage = "\x01";
    if (message == lastMessage) return;
    lastMessage = message;

    int16_t y = guiErrorBarY();
    uint16_t bg = message.isEmpty() ? COLOR_BLACK : COLOR_RED;
    tft->fillRect(0, y, guiWidth(), GUI_ERROR_BAR_HEIGHT, bg);

    if (!message.isEmpty()) {
        tft->setTextSize(1);
        tft->setTextColor(COLOR_WHITE, COLOR_RED);
        tft->setCursor(guiLeftX(), y + (GUI_ERROR_BAR_HEIGHT - 8) / 2);
        tft->print(message);
    }
}

// --- Multi-zone PV display helpers ---
static int countActiveZones() {
    int n = 0;
    for (int i = 0; i < MAX_ZONES; i++) if (g_zones[i].active) n++;
    return n;
}
static int firstActiveZone() {
    for (int i = 0; i < MAX_ZONES; i++) if (g_zones[i].active) return i;
    return 0;
}
// One "Z<n> nnnC" row per active zone. Returns the Y just below the last
// row drawn, so the caller can stack more content under it.
static char getDisplayTempScale() {
  char scale = 'C';
  xSemaphoreTake(mutex, portMAX_DELAY);
  scale = g_tempScale;
  xSemaphoreGive(mutex);
  return scale;
}

static int displayTemperature(double rawValue, char scale) {
  if (scale == 'F') {
    return (int)(rawValue * 9.0 / 5.0 + 32.0 + 0.5);
  }
  return (int)(rawValue + 0.5);
}

// One rounded card per active zone (matches the web status page's
// .zone-list rows) instead of bare text. gui_run() polls in a tight loop
// with no delay, so — unlike a cheap text-only redraw — repainting a filled
// rounded rect on every single pass is heavy enough to visibly flicker.
// Cache per-row so a card only actually redraws when its text/fault state
// changes (temps update ~2x/sec) or the content area was cleared.
static int16_t drawCompactZonePVs(int16_t top, uint8_t textSize, int16_t rowPitch) {
    static uint32_t cacheGen[MAX_ZONES] = {0};
    static String cacheLabel[MAX_ZONES];
    static bool cacheFault[MAX_ZONES] = {false};
    static bool cacheValid[MAX_ZONES] = {false};

    char displayScale = getDisplayTempScale();
    tft->setTextSize(textSize);
    int16_t rowY = top;
    int16_t rowW = guiWidth() - guiLeftX() * 2;
    int16_t rowH = rowPitch - 6;
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!g_zones[i].active) continue;
        bool fault = g_zones[i].fault;
        char buf[16];
        snprintf(buf, sizeof(buf), "Z%d %04d%c", i + 1, displayTemperature(g_zones[i].pv, displayScale), displayScale);

        bool unchanged = cacheValid[i] && cacheGen[i] == contentGeneration &&
                          cacheFault[i] == fault && cacheLabel[i] == buf;
        if (!unchanged) {
          uint16_t cardBg = fault ? COLOR_DANGER_DIM : COLOR_CARD;
          uint16_t textColor = fault ? COLOR_RED : COLOR_WHITE;
          tft->fillRoundRect(guiLeftX(), rowY, rowW, rowH, 6, cardBg);
          tft->setTextColor(textColor, cardBg);
          tft->setCursor(guiLeftX() + 10, rowY + (rowH - 8 * textSize) / 2);
          tft->print(buf);
          cacheGen[i] = contentGeneration;
          cacheLabel[i] = buf;
          cacheFault[i] = fault;
          cacheValid[i] = true;
        }
        rowY += rowPitch;
    }
    return rowY;
}

void gui_start() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);  /* enable backlight*/

  if (!tft->begin()) {
    log_i("begin failed");
  }

  tft->fillScreen(COLOR_BLACK);
  touch_init(tft->width(), tft->height(), 2);

  preferences.begin("kilnmonitor", false);
  g_tcType = (char)preferences.getInt("tcType", (int)TC_DEFAULT_TYPE);
  g_tempScale = (char)preferences.getInt("tempScale", (int)'C');
  g_sessionId = preferences.getUInt("sessionId", 0);
  g_kilnName = preferences.getString("kilnName", "");
  if (g_kilnName.isEmpty()) {
    g_kilnName = defaultKilnName();
    preferences.putString("kilnName", g_kilnName);
  }
}

void gui_run() {

  if (network.get_captive_mode()) {
    network.handleCaptiveMode();
  }

  xSemaphoreTake(disp_mutex, portMAX_DELAY);

  // Fault bar: which zone(s), if any, are faulted; or a "no probes" warning
  // once the initial scan has run and found nothing.
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool scanned = g_zonesScanned;
  bool anyActive = false;
  String faultMsg = "";
  int faultCount = 0;
  for (int i = 0; i < MAX_ZONES; i++) {
    if (g_zones[i].active) anyActive = true;
    if (g_zones[i].active && g_zones[i].fault) {
      if (faultCount > 0) faultMsg += ", ";
      faultMsg += "Z" + String(i + 1) + ": " + g_zones[i].errMsg;
      faultCount++;
    }
  }
  xSemaphoreGive(mutex);

  String barMsg = "";
  if (scanned && !anyActive) barMsg = "NO THERMOCOUPLES DETECTED";
  else if (faultCount > 0) barMsg = "TC FAULT " + faultMsg;
  disp_error_bar(barMsg);

  disp_top_bar();

  readButtons();

  if (showingConfig) {
    configScreen();
  } else {
    statusScreen();
  }

  xSemaphoreGive(disp_mutex);
}

//  STATUSSCREEN: main/home screen — zone readouts + logging state
void statusScreen() {
  char displayScale = getDisplayTempScale();
  int activeZones = countActiveZones();

  // The single-zone "pv" layout and the multi-zone compact rows occupy
  // different, overlapping screen regions. Redrawing only fills the glyph
  // cells each call touches, so switching layouts (or losing/gaining a row
  // within the compact view) leaves stale pixels from the previous layout
  // on screen unless we wipe the content area on the transition.
  static int lastActiveZones = -1;
  if (activeZones != lastActiveZones) {
    clearContentArea(COLOR_BLACK);
    lastActiveZones = activeZones;
  }

  if (activeZones <= 1) {
    int zone = firstActiveZone();
    tft->setCursor(guiLeftX(), guiContentTop() + 20);
    tft->setTextColor(COLOR_MUTED, COLOR_BLACK);
    tft->setTextSize(2);
    tft->print(F("Z1"));
    tft->setCursor(guiLeftX() + 50, guiContentTop() + 10);
    tft->setTextColor(COLOR_WHITE, COLOR_BLACK);
    tft->setTextSize(8);
    tft->printf("%04d%c", displayTemperature(g_zones[zone].pv, displayScale), displayScale);
  } else {
    drawCompactZonePVs(guiContentTop() + 10, 3, 36);
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  bool active = g_loggingActive;
  double ambient = g_ambientBaseline;
  char tcType = g_tcType;
  xSemaphoreGive(mutex);

  // Pill-shaped status badge, matching the web status page's badge style —
  // background is a pre-mixed dark tint standing in for the translucent
  // fill CSS uses there, since this display has no alpha blending.
  // gui_run() polls in a tight loop with no delay, so this only actually
  // redraws when `active` flips or the content area was cleared — otherwise
  // a filled rounded rect repainted every single pass visibly flickers
  // (a plain-text redraw was cheap enough not to, a filled shape isn't).
  static uint32_t badgeGen = 0xFFFFFFFF;
  static int badgeCachedActive = -1;
  int16_t badgeY = guiContentTop() + 126;
  bool badgeStateChanged = ((int)active != badgeCachedActive);
  if (badgeStateChanged) {
    tft->fillRect(0, badgeY - 2, guiWidth(), 40, COLOR_BLACK);
  }
  if (badgeStateChanged || badgeGen != contentGeneration) {
    const char* badgeText = active ? "LOGGING ACTIVE" : "WAITING";
    uint16_t badgeBg = active ? COLOR_SUCCESS_DIM : COLOR_CARD;
    uint16_t badgeFg = active ? COLOR_SUCCESS : COLOR_WHITE;
    tft->setTextSize(3);
    int16_t textW = getTftTextWidth(badgeText, 3);
    int16_t hPad = 16;
    int16_t badgeW = textW + hPad * 2;
    int16_t badgeH = 36;
    int16_t badgeX = (guiWidth() - badgeW) / 2;
    tft->fillRoundRect(badgeX, badgeY, badgeW, badgeH, badgeH / 2, badgeBg);
    tft->setTextColor(badgeFg, badgeBg);
    tft->setCursor(badgeX + hPad, badgeY + (badgeH - 24) / 2);
    tft->print(badgeText);
    badgeGen = contentGeneration;
    badgeCachedActive = (int)active;
  }

  tft->setTextSize(2);
  tft->setTextColor(COLOR_MUTED, COLOR_BLACK);
  tft->setCursor(guiLeftX(), guiContentTop() + 170);
  tft->printf("Ambient %04d%c   TC:%c", displayTemperature(ambient, displayScale), displayScale, tcType);

  drawActionButton("SETTINGS >", 0, 1);

  if (touchReleased) {
    if (touchInActionButton(0, 1)) {
      showingConfig = true;
      clearContentArea(COLOR_BLACK);
    }
    touchHandled = true;
    touchHandledTime = millis();
    touchReleased = false;
  }
}

//  CONFIGSCREEN: TC type (K/S) + WiFi captive portal + done
void configScreen() {
  tft->setTextColor(COLOR_WHITE, COLOR_BLACK);
  tft->setTextSize(2);
  tftPrint("CONFIG", guiLeftX(), guiContentTop() + 20);

  xSemaphoreTake(mutex, portMAX_DELAY);
  char tcType = g_tcType;
  xSemaphoreGive(mutex);

  String text1 = String("TC TYPE: ") + tcType;
  String text2 = (g_tempScale == 'F') ? "TEMP: F" : "TEMP: C";
  String text3 = network.get_captive_mode() ? "EXIT WIFI" : "RESET WIFI";
  String text4 = "DONE";

  drawMenuButton(text1, 0);
  drawMenuButton(text2, 1);
  drawMenuButton(text3, 2);
  drawMenuButton(text4, 3, COLOR_CARD); // secondary style — just navigates back, no change to commit

  if (touchReleased) {
    int16_t row = touchMenuButtonIndex(4);
    if (row == 0) {
      char newTcType = (tcType == 'K') ? 'S' : 'K';
      xSemaphoreTake(mutex, portMAX_DELAY);
      g_tcType = newTcType;
      xSemaphoreGive(mutex);
      preferences.putInt("tcType", (int)newTcType);
      clearContentArea(COLOR_BLACK);
    } else if (row == 1) {
      xSemaphoreTake(mutex, portMAX_DELAY);
      g_tempScale = (g_tempScale == 'F') ? 'C' : 'F';
      xSemaphoreGive(mutex);
      preferences.putInt("tempScale", (int)g_tempScale);
      clearContentArea(COLOR_BLACK);
    } else if (row == 2) {
      network.handleCaptiveModeToggle();
      clearContentArea(COLOR_BLACK);
    } else if (row == 3) {
      showingConfig = false;
      clearContentArea(COLOR_BLACK);
    }
    touchHandled = true;
    touchHandledTime = millis();
    touchReleased = false;
  }
}

//  Updates top bar of TFT with WiFi and Influx info
void disp_top_bar() {

  if ( (millis() - topBar_start < topBarCycle)) {
    return;
  }
  int centerY = 10;
  tft->fillRect(0, 0, guiWidth(), GUI_TOP_BAR_HEIGHT, BAR_COLOR); // clear top notch
  tft->setTextSize(1);

  xSemaphoreTake(mutex, portMAX_DELAY);
  bool connecting = g_connecting;
  bool connected = g_connected;
  xSemaphoreGive(mutex);

  if (connected) {
    // Right-aligned as one block: bars immediately left of the %, both
    // anchored off the text width so they stay adjacent regardless of
    // whether quality is 1-3 digits (was previously a fixed pixel offset
    // for the bars vs. a width-based offset for the text, which left a
    // large gap between them).
    int8_t quality = network.getWifiQuality();
    String pctText = String(quality) + "%";
    int16_t pctWidth = getTftTextWidth(pctText, 1);
    static constexpr int16_t WIFI_ICON_WIDTH = 8;  // 4 bars, 2px apart
    static constexpr int16_t WIFI_ICON_GAP = 4;    // gap between bars and %
    int16_t pctX = guiRightX(pctWidth);
    int16_t iconX = pctX - WIFI_ICON_GAP - WIFI_ICON_WIDTH;

    tft->setTextColor(COLOR_WHITE, BAR_COLOR);
    tft->setCursor(pctX, centerY);
    tft->print(pctText);
    for (int8_t i = 0; i < 4; i++) {
      for (int8_t j = 0; j < 2 * (i + 1); j++) {
        if (quality > i * 25) tft->drawPixel(iconX + 2 * i, (centerY + 7) - j, COLOR_GREEN);
      }
    }
  } else if (connecting) {
    disp_connecting();
  } else {
    tft->setTextColor(COLOR_RED, BAR_COLOR);
    tft->setCursor(guiRightX(90), centerY);
    tft->print("OFFLINE");
  }

  // IP address in the middle of the bar (station IP once connected, AP IP
  // while the captive portal is up), followed by the date/time once NTP
  // has synced (network.cpp syncs on WiFi connect, independent of whether
  // InfluxDB is configured).
  int16_t dateTimeX = 100;
  {
    String ipText;
    if (connected) {
      ipText = WiFi.localIP().toString();
    } else if (network.get_captive_mode()) {
      ipText = WiFi.softAPIP().toString();
    }
    if (!ipText.isEmpty()) {
      tft->setTextColor(COLOR_WHITE, BAR_COLOR);
      tft->setCursor(100, centerY);
      tft->print(ipText);
      dateTimeX = 100 + getTftTextWidth(ipText, 1) + 12;
    }
  }

  {
    time_t now = time(nullptr);
    if (now > 1000000000l) { // synced — same sanity threshold timeSync() itself uses
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char buf[16];
      strftime(buf, sizeof(buf), "%m/%d %H:%M", &timeinfo);
      tft->setTextColor(COLOR_WHITE, BAR_COLOR);
      tft->setCursor(dateTimeX, centerY);
      tft->print(buf);
    }
  }

  // Draw publish status
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool published = g_published;
  xSemaphoreGive(mutex);

  {
    const int iconLeft = guiLeftX() + 4;
    if (published) {
      tft->drawLine(iconLeft, centerY + 1, iconLeft + 3, centerY + 4, COLOR_GREEN);
      tft->drawLine(iconLeft + 3, centerY + 3, iconLeft + 7, centerY + 0, COLOR_GREEN);
      tft->drawLine(iconLeft, centerY + 2, iconLeft + 3, centerY + 5, COLOR_GREEN);
      tft->drawLine(iconLeft + 3, centerY + 4, iconLeft + 7, centerY + 1, COLOR_GREEN);
      tft->drawLine(iconLeft + 1, centerY + 1, iconLeft + 4, centerY + 4, COLOR_GREEN);
      tft->drawLine(iconLeft + 4, centerY + 3, iconLeft + 8, centerY + 0, COLOR_GREEN);
    } else {
      tft->drawLine(iconLeft + 1, centerY - 1, iconLeft + 7, centerY + 5, COLOR_RED);
      tft->drawLine(iconLeft + 7, centerY - 1, iconLeft + 1, centerY + 5, COLOR_RED);
    }
  }

  topBar_start = millis();
}

//  disp_connecting: DISPLAYS CONNECTING MESSAGE
void disp_connecting() {
  tft->setTextColor(COLOR_WHITE, BAR_COLOR);
  tft->setTextSize(1);
  tft->fillRect(0, 0, guiWidth(), GUI_TOP_BAR_HEIGHT, BAR_COLOR);
  tft->setCursor(guiRightX(80), 8);
  tft->print("Connecting...");
}

//  DISPLAYERRORMESSAGE: PRINT AN ERROR ON TFT (full-screen, blocking) —
//  only used for startup failures before the GUI loop is running.
void disp_error_msg(String title, String message1, String message2) {
  xSemaphoreTake(disp_mutex, portMAX_DELAY);
  tft->fillScreen(COLOR_BLACK);
  tft->setTextColor(COLOR_RED, COLOR_BLACK);
  tft->setTextSize(4);
  tftPrintCenterWidth("ERROR", 80);
  tft->setTextColor(COLOR_WHITE, COLOR_BLACK);
  tft->setTextSize(2);
  tftPrintCenterWidth(title, 150);
  tftPrintCenterWidth(message1, 180);
  tftPrintCenterWidth(message2, 210);
  xSemaphoreGive(disp_mutex);
}

//  RESETTFT: RESETS TFT
void resetTFT() {
  tft->begin();
  tft->setRotation(3);
  tft->fillScreen(COLOR_BLACK);
}

//  TFTPRINTCENTERWIDTH: CENTERS CURSOR ON WIDTH AND PRINTS
void tftPrintCenterWidth(String text, int y) {
  tft->setCursor((guiWidth() - getTftTextWidth(text,2)) / 2, y);
  tft->print(text);
}

//  TFTPRINT: SETS CURSOR ON (X,Y) AND PRINTS
void tftPrint(String text, int x, int y) {
  tft->setCursor(x, y);
  tft->print(text);
}

//  READBUTTONS: touch state machine (touchDown / touchReleased / touchX / touchY).
//  Each screen hit-tests its own on-screen buttons once touchReleased is set.
void readButtons() {
  bool touched = false;
  if (touch_has_signal()) {
    touched = touch_touched();
  }

  if (touched) {
    if (!touchDown && millis() - touchHandledTime >= 250) {
      touchDown = true;
      touchReleased = false;
      touchHandled = false;
      touchStartOnConfig = showingConfig;
      touchX = get_touch_x();
      touchY = get_touch_y();
    }
  } else {
    if (touchDown) {
      touchDown = false;
      touchReleased = true;
      log_i("Touch released at (%d, %d) on %s screen", touchX, touchY, showingConfig ? "config" : "status");
    }
  }

  // Guard against acting on a touch that started on a different screen
  // (e.g. a screen transition happened mid-touch).
  if (touchReleased && touchStartOnConfig != showingConfig) {
    touchReleased = false;
  }
}


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
// <vector> and <algorithm> removed from main — replaced with fixed-size buffers
#include <cmath>       // for floor(), fmod(), isnan()
#include <esp_task_wdt.h> // hardware watchdog

#include "config.h"
#include "secrets.h"

#include "ui.h"
#include "touch.h"
#include "net.h"
#include "storage.h"
#include <LittleFS.h>
#include "sensors.h"
#include "weather.h"
#include "quotes.h"
#include "news.h"
#include "games.h"
#include "joystick.h"
#include "alarm.h"

// Forward declarations for UI functions
void uiPomodoroDrawFull(uint32_t remainMs, bool running, bool isWork, int cycles);
void uiPomodoroUpdate(uint32_t remainMs, bool running, bool isWork, int cycles);
void uiSketchDrawFull(uint16_t currentColor);
void uiSketchDrawFooter(uint16_t currentColor);
void uiSketchRedrawCurrent(); // Redraw current sketch from buffer
void uiSketchClear();         // Clear sketch buffer
void uiSketchDrawLine(int x1, int y1, int x2, int y2, uint16_t color, int z);
void uiSketchDrawBezier(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color, int z);
// uiSketchGalleryDrawFull, uiSketchView, uiSketchDelete now declared in ui.h
void uiSettingsDrawFull(int sel, int top, const char* v0, const char* v1, const char* v2, const char* v3, const char* v4, const char* v5, const char* v6, const char* v7, const char* v8, const char* v9);
void uiFormatOptionsDrawFull(int sel, const char** items, int count);
void uiFormatOptionsRedrawSelection(int sel, const char** items, int count);
void gamesStartByIndex(int index); // Forward declaration for games startup
void quotesNext(); // Forward decl for quotes cycling

// =================== Interrupt-driven encoder ===================
// Using IRAM_ATTR so ISRs run from IRAM even during flash cache misses.
static volatile int  encAccum    = 0;   // accumulated delta, read+clear in loop
static volatile int  encLastA    = 0;
static volatile int  encLastB    = 0;
static bool          encBtnLast  = false;
static uint32_t      encBtnDownAt = 0;

// PRO UPGRADE: Sub-millisecond interrupt debouncing variable
static volatile uint32_t encLastMicros = 0;

static void IRAM_ATTR encoderISR_A() {
  uint32_t nowUs = micros();
  // PRO UPGRADE: Hardware Debounce. Ignore any physical contact static 
  // faster than 1.5ms (1500us) to completely eliminate phantom scrolls
  if ((nowUs - encLastMicros) < 1500) return;
  
  int a = digitalRead(ENC_A);
  int b = digitalRead(ENC_B);
  if (a != encLastA) {
    encAccum += (b != a) ? +1 : -1;
    encLastA = a;
    encLastB = b;
    encLastMicros = nowUs;
  }
}

static void encoderBegin() {
  pinMode(ENC_A,  INPUT_PULLUP);
  pinMode(ENC_B,  INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encLastA = digitalRead(ENC_A);
  encLastB = digitalRead(ENC_B);
  encBtnLast = (digitalRead(ENC_SW) == LOW);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR_A, CHANGE);
}

// Call once per loop(); returns accumulated ticks since last call.
static int encoderReadDelta() {
  portDISABLE_INTERRUPTS();
  int d = encAccum;
  encAccum = 0;
  portENABLE_INTERRUPTS();
  return d;
}

static void encoderPoll(bool &clicked, bool &longPressed) {
  const bool nowDown = (digitalRead(ENC_SW) == LOW);
  clicked     = false;
  longPressed = false;
  static bool longSent = false;

  if (nowDown && !encBtnLast) {
    encBtnDownAt = millis();
    longSent = false;
  } else if (nowDown && !longSent) {
    if (millis() - encBtnDownAt >= 600) {
      longPressed = true;
      longSent    = true;
    }
  } else if (!nowDown && encBtnLast) {
    uint32_t held = millis() - encBtnDownAt;
    if (held > 20 && held < 600) clicked = true;
  }
  encBtnLast = nowDown;
}

// =================== Gadget state ===================
enum Screen : uint8_t {
  SCR_TITLE = 0,
  SCR_HOME,
  SCR_TIME,         // Normal time
  SCR_TIME_STUDY,   // Full screen time, no timeout
  SCR_WEATHER,
  SCR_NEWS,
  SCR_NEWS_CHANNELS,   // NEW: BBC, Times, GNews
  SCR_NEWS_CATEGORIES, // NEW: Tech, Business, etc.
  SCR_NEWS_STUDY,      // Full screen news headline + description
  SCR_QUOTES,
  SCR_QUOTES_STUDY, // NEW: Full screen quote
  SCR_GAMES,
  SCR_TOOLS,
  SCR_ALARM,
  SCR_CALC,
  SCR_TEMP_PRESSURE,
  SCR_STOPWATCH,
  SCR_TIMER,
  SCR_POMODORO,     // NEW: Pomodoro Timer
  SCR_SKETCH,       // NEW: Sketch Pad
  SCR_SKETCH_GALLERY, // NEW: Gallery
  SCR_DOCS,         // NEW: Document Reader
  SCR_UNITCONV,
  SCR_SETTINGS,
  SCR_FORMAT_OPTIONS, // NEW: Granular format options
  SCR_INFO,
  SCR_WIFI,
};

static Screen screen = SCR_TITLE;
static Screen lastActiveScreen = SCR_HOME;
static bool qsOpen = false; // NEW: Quick settings overlay state
static uint32_t lastNightCheck = 0; // NEW: Night mode check timer

// Idle Return logic
static uint32_t lastInteractionMs = 0;
static uint32_t displayTimeoutMs = 60000UL; // Default to 60s

// HOME list
extern const char* HOME_ITEMS[] = { "TIME", "WEATHER", "NEWS", "QUOTES", "GAMES", "TEMP & PRESSURE", "POMODORO", "SKETCH PAD", "DOCUMENTS" };
extern const int HOME_COUNT = sizeof(HOME_ITEMS) / sizeof(HOME_ITEMS[0]);

static int homeSel = 0;
static int homeTop = 0;

// NEWS list
static const char* NEWS_CHAN_ITEMS[] = { "INDIA NEWS", "GLOBAL NEWS" };
static const int NEWS_CHAN_COUNT = 2;
static int newsChanSel = 0;

static const char* NEWS_CAT_ITEMS[] = { "TOP HEADLINES", "TECHNOLOGY", "BUSINESS", "SPORTS", "ENTERTAINMENT", "WORLD", "SCIENCE", "HEALTH" };
static const int NEWS_CAT_COUNT = 8;
static int newsCatSel = 0;
static int newsStudyScrollOffset = 0; // Line scroll position in the article reader

// UI geometry (must match ui.cpp)
static const int UI_HEADER_H = 30;
static const int UI_FOOTER_H = 34;
static const int HOME_LIST_Y = UI_HEADER_H;
static const int HOME_LIST_H = TFT_H - UI_HEADER_H - UI_FOOTER_H;
static const int HOME_ROW_H  = 28;
static const int HOME_ROWS   = HOME_LIST_H / HOME_ROW_H;

static uint32_t titleShownAt = 0;
static bool titleReadyToExit = false;

static inline bool wifiOk() { return netIsConnected(); }

static void clampHomeViewport() {
  if (homeSel < 0) homeSel = 0;
  if (homeSel >= HOME_COUNT) homeSel = HOME_COUNT - 1;

  if (homeTop < 0) homeTop = 0;
  if (homeTop > HOME_COUNT - 1) homeTop = HOME_COUNT - 1;

  if (homeSel < homeTop) homeTop = homeSel;
  if (homeSel >= homeTop + HOME_ROWS) homeTop = homeSel - (HOME_ROWS - 1);

  if (homeTop < 0) homeTop = 0;
  int maxTop = HOME_COUNT - HOME_ROWS;
  if (maxTop < 0) maxTop = 0;
  if (homeTop > maxTop) homeTop = maxTop;
}

static void homeRedrawSelectionOnly(int prevSel, int prevTop) {
  if (prevTop != homeTop) { uiHomeDrawFull(homeSel, homeTop); return; }
  const int prevRow = prevSel - homeTop;
  const int newRow  = homeSel - homeTop;
  if (prevRow >= 0 && prevRow < HOME_ROWS) uiHomeDrawRow(prevRow, HOME_ITEMS[prevSel], false);
  if (newRow  >= 0 && newRow  < HOME_ROWS) uiHomeDrawRow(newRow,  HOME_ITEMS[homeSel], true);
}

static void goTitle() {
  if (screen != SCR_TITLE) {
    lastActiveScreen = screen;
  }
  uiFadeOut(150);
  screen = SCR_TITLE;
  uiSetMascotExpression(EXPR_SLEEPING);
  uiTitleScreenDraw();
  uiFadeIn(150);
  titleShownAt = millis();
  titleReadyToExit = false;
  lastInteractionMs = millis();
}

static void goHome() {
  // Leaving games? cleanly disable joystick so it doesn't interfere
  if (screen == SCR_GAMES) gamesLeave();
  
  // If leaving Temp & Pressure screen, put BMP back to sleep
  if (screen == SCR_TEMP_PRESSURE) sensorsIdleBMP();

  // Reset touch resilience
  touchSetResilience(false);

  screen = SCR_HOME;
  uiSetMascotExpression(EXPR_SMILE);
  
  // Update viewports to ensure newest items (DOCUMENTS) are visible
  clampHomeViewport();
  
  uiHomeDrawFull(homeSel, homeTop);
  lastInteractionMs = millis();
}

// ===== TIME =====
static bool timeShowSeconds = true;
static uint32_t lastTimeDraw = 0;

static void timeFormat(DateTime dt, char* outTime, size_t outTimeSz, bool showSec,
                       char* outDate, size_t outDateSz) {
  int hh = dt.hour();
  bool pm = (hh >= 12);
  int hh12 = hh % 12;
  if (hh12 == 0) hh12 = 12;

  if (showSec) snprintf(outTime, outTimeSz, "%02d:%02d:%02d %s", hh12, dt.minute(), dt.second(), pm ? "PM" : "AM");
  else         snprintf(outTime, outTimeSz, "%02d:%02d %s",       hh12, dt.minute(),              pm ? "PM" : "AM");

  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  int dow = dt.dayOfTheWeek();
  if (dow < 0 || dow > 6) dow = 0;

  snprintf(outDate, outDateSz, "%s, %02d-%02d-%04d", days[dow], dt.day(), dt.month(), dt.year());
}

static void enterTimeScreen() {
  screen = SCR_TIME;
  uiTimeScreenDrawFull(sensorsRtcOk());
}

static void enterStudyMode() {
  screen = SCR_TIME_STUDY;
  uiTimeScreenDrawStudy(sensorsRtcOk());
}

static void timeScreenLoop(uint32_t now) {
  if (!sensorsRtcOk()) return;
  if (now - lastTimeDraw < 250) return;
  lastTimeDraw = now;

  DateTime dt = sensorsNow();
  char tbuf[24], dbuf[24];
  timeFormat(dt, tbuf, sizeof(tbuf), timeShowSeconds, dbuf, sizeof(dbuf));
  if (screen == SCR_TIME_STUDY) {
    uiTimeScreenUpdateStudy(tbuf, dbuf);
  } else {
    uiTimeScreenUpdate(tbuf, dbuf);
  }
}

// ===== WEATHER =====
static int wxCitySel = 0;
static WeatherCache wxAll[4];

static bool weatherCacheLooksValid(const WeatherCache& w) {
  if (isnan(w.tempC)) return false;
  if (!w.updated[0]) return false;
  if (strcmp(w.updated, "None") == 0) return false;
  if (strcmp(w.updated, "NO_TIME") == 0) return false;
  return true;
}

static void weatherPrevCity() {
  wxCitySel--;
  if (wxCitySel < 0) wxCitySel = CITY_COUNT - 1;
  uiWeatherScreenDrawFull(wxCitySel, wxAll[wxCitySel], wifiOk(), weatherIsBusy());
}

static void weatherNextCity() {
  wxCitySel++;
  if (wxCitySel >= CITY_COUNT) wxCitySel = 0;
  uiWeatherScreenDrawFull(wxCitySel, wxAll[wxCitySel], wifiOk(), weatherIsBusy());
}

static void weatherRequestRefresh() {
  if (!wifiOk()) return;
  if (weatherIsBusy()) return;
  uiSetMascotExpression(EXPR_BUSY);
  weatherRequestFetch(wxCitySel);
  uiWeatherScreenSetBusy(true);
}

static void enterWeatherScreen() {
  screen = SCR_WEATHER;
  uiWeatherScreenDrawFull(wxCitySel, wxAll[wxCitySel], wifiOk(), weatherIsBusy());

  if (wifiOk() && !weatherIsBusy() && !weatherCacheLooksValid(wxAll[wxCitySel])) {
    weatherRequestRefresh();
  }
}

static inline bool inFooter(int y) { return y >= (TFT_H - UI_FOOTER_H); }

// WEATHER footer hitboxes must match ui.cpp
static inline bool inRefreshBtnWx(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  // FETCH (right button): expanded hitbox for smooth touch, split at 138
  return (x >= 138 && x < TFT_W && y >= FOOT_Y);
}
static inline bool inCityBtnWx(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  // CITY (left button): expanded hitbox for smooth touch, split at 138
  return (x >= 0 && x < 138 && y >= FOOT_Y);
}

static inline bool inQuotesNewBtn(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  // Expanded hitbox for "NEW" button in Quotes (covers whole button and gap)
  return (x >= 0 && x < 110 && y >= FOOT_Y);
}

static inline bool inNewsRefreshBtn(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  // Expanded hitbox for "FETCH" button in News (covers whole button and gap)
  return (x >= 0 && x < 130 && y >= FOOT_Y);
}

// HOME footer nav
static int homeFooterIndex(int x, int y) {
  if (!inFooter(y)) return -1;
  int w = TFT_W / 4;
  int idx = x / w;
  if (idx < 0) idx = 0;
  if (idx > 3) idx = 3;
  return idx; // 0 HOME, 1 INFO, 2 WIFI, 3 TOOLS
}

static void showInfo() {
  char l1[40]; snprintf(l1, sizeof(l1), "Firmware: NAA TV");
  char l2[40]; snprintf(l2, sizeof(l2), "Uptime: %lus", (unsigned long)(millis() / 1000));
  char l3[40]; snprintf(l3, sizeof(l3), "Free heap: %u", (unsigned)ESP.getFreeHeap());
  screen = SCR_INFO;
  uiInfoScreenDraw(wifiOk(), l1, l2, l3);
}

static void showWifi() {
  screen = SCR_WIFI;
  String ssid = WiFi.SSID();
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("-");
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
  uiWifiScreenDraw(wifiOk(), ssid.c_str(), ip.c_str(), rssi);
}

static int toolsSel = 0;
static int toolsTop = 0;
static const char* TOOLS_ITEMS[] = { "ALARM CLOCK", "CALCULATOR", "STOPWATCH", "TIMER", "UNIT CONVERTER", "SKETCHES", "SETTINGS", "FORMAT OPTIONS", "ABOUT" };
static const int TOOLS_COUNT = sizeof(TOOLS_ITEMS) / sizeof(TOOLS_ITEMS[0]);

static int gamesSel = 0;
static int gamesTop = 0;
static const char* GAMES_ITEMS[] = { "DICE", "REACTION", "PONG", "BREAKOUT", "DINO", "CAR", "SNAKE", "MAZE", "TETRIS", "SPACE INVADERS", "PIXEL ART" };
static const int GAMES_COUNT = sizeof(GAMES_ITEMS) / sizeof(GAMES_ITEMS[0]);

static void clampGamesViewport() {
  if (gamesSel < 0) gamesSel = 0;
  if (gamesSel >= GAMES_COUNT) gamesSel = GAMES_COUNT - 1;
  if (gamesTop < 0) gamesTop = 0;
  int rows = (TFT_H - UI_HEADER_H - UI_FOOTER_H) / 28;
  if (rows < 1) rows = 1;
  int maxTop = (GAMES_COUNT > rows) ? (GAMES_COUNT - rows) : 0;
  if (gamesTop > maxTop) gamesTop = maxTop;
  if (gamesSel < gamesTop) gamesTop = gamesSel;
  if (gamesSel >= gamesTop + rows) gamesTop = gamesSel - (rows - 1);
}

static void showGamesMenu() {
  screen = SCR_GAMES;
  clampGamesViewport();
  uiGamesMenuDrawFull(gamesSel, gamesTop, GAMES_ITEMS, GAMES_COUNT);
}

static void gamesRedrawSelectionOnly(int prevSel, int prevTop) {
  if (prevTop != gamesTop) { 
    uiGamesMenuDrawFull(gamesSel, gamesTop, GAMES_ITEMS, GAMES_COUNT); 
    return; 
  }
  const int rows = (TFT_H - UI_HEADER_H - UI_FOOTER_H) / 28;
  const int prevRow = prevSel - gamesTop;
  const int newRow  = gamesSel - gamesTop;
  if (prevRow >= 0 && prevRow < rows) uiGamesMenuDrawRow(prevRow, GAMES_ITEMS[prevSel], false);
  if (newRow  >= 0 && newRow  < rows) uiGamesMenuDrawRow(newRow,  GAMES_ITEMS[gamesSel], true);
}

static void clampToolsViewport() {
  if (toolsSel < 0) toolsSel = 0;
  if (toolsSel >= TOOLS_COUNT) toolsSel = TOOLS_COUNT - 1;
  if (toolsTop < 0) toolsTop = 0;
  int rows = (TFT_H - UI_HEADER_H - UI_FOOTER_H) / 28;
  if (rows < 1) rows = 1;
  int maxTop = (TOOLS_COUNT > rows) ? (TOOLS_COUNT - rows) : 0;
  if (toolsTop > maxTop) toolsTop = maxTop;
  if (toolsSel < toolsTop) toolsTop = toolsSel;
  if (toolsSel >= toolsTop + rows) toolsTop = toolsSel - (rows - 1);
}

static void showTools() {
  screen = SCR_TOOLS;
  clampToolsViewport();
  uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT);
}

static void toolsRedrawSelectionOnly(int prevSel, int prevTop) {
  if (prevTop != toolsTop) { 
    uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT); 
    return; 
  }
  const int rows = (TFT_H - UI_HEADER_H - UI_FOOTER_H) / 28;
  const int prevRow = prevSel - toolsTop;
  const int newRow  = toolsSel - toolsTop;
  if (prevRow >= 0 && prevRow < rows) uiToolsMenuDrawRow(prevRow, TOOLS_ITEMS[prevSel], false);
  if (newRow  >= 0 && newRow  < rows) uiToolsMenuDrawRow(newRow,  TOOLS_ITEMS[toolsSel], true);
}

static int settingsSel = 0;
static int settingsTop = 0;

static void clampSettingsViewport() {
  if (settingsSel < 0) settingsSel = 0;
  if (settingsSel >= 10) settingsSel = 9;
  if (settingsTop < 0) settingsTop = 0;
  const int rowH = 28;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int maxRows = bodyH / rowH;
  int maxTop = (10 > maxRows) ? (10 - maxRows) : 0;
  if (settingsTop > maxTop) settingsTop = maxTop;
  if (settingsSel < settingsTop) settingsTop = settingsSel;
  if (settingsSel >= settingsTop + maxRows) settingsTop = settingsSel - (maxRows - 1);
}

static bool setUseF = false;
static bool setUseInHg = false;
static bool setUseMph = false;
static int setFontSize = 1;
static int setBrightness = 1;
static bool setThemeDark = false;
static int setAccentIdx = 0; // 0: Cyber Blue, 1: Emerald Green, 2: Sunset Orange, 3: Royal Purple
static int setTimeoutIdx = 3; // Default to 60s
static int setPomoWorkMin = 25;
static int setPomoBreakMin = 5;
static float setTempOffset = 0.0f;

uint32_t pomoWorkMs = 25 * 60 * 1000UL;
uint32_t pomoBreakMs = 5 * 60 * 1000UL;

static const uint32_t TIMEOUT_VALS[] = { TIMEOUT_15S, TIMEOUT_30S, TIMEOUT_45S, TIMEOUT_60S, TIMEOUT_120S, TIMEOUT_NEVER };
static const char* TIMEOUT_LABELS[] = { "15s", "30s", "45s", "60s", "120s", "Never" };

static void settingsLoad() {
  setUseF = storageGetBool("set_use_f", false);
  setUseInHg = storageGetBool("set_use_inhg", false);
  setUseMph = storageGetBool("set_use_mph", false);
  setFontSize = storageGetI32("set_font_sz", 1);
  setBrightness = storageGetI32("set_bl_lvl", 1);
  if (setBrightness < 0) setBrightness = 0;
  if (setBrightness > 4) setBrightness = 4;
  setThemeDark = storageGetBool("set_theme_dark", true);
  setAccentIdx = storageGetI32("set_accent_idx", 0);
  if (setAccentIdx < 0 || setAccentIdx > 3) setAccentIdx = 0;
  setTimeoutIdx = storageGetI32("set_timeout_idx", 3);
  if (setTimeoutIdx < 0 || setTimeoutIdx > 5) setTimeoutIdx = 3;
  displayTimeoutMs = TIMEOUT_VALS[setTimeoutIdx];
  
  setPomoWorkMin = storageGetI32("set_pomo_w", 25);
  setPomoBreakMin = storageGetI32("set_pomo_b", 5);
  pomoWorkMs = setPomoWorkMin * 60 * 1000UL;
  pomoBreakMs = setPomoBreakMin * 60 * 1000UL;
  
  // Custom temp calibration offset
  setTempOffset = (float)storageGetI32("set_temp_off", 0) / 10.0f;
}

static void settingsSave() {
  storageSetBool("set_use_f", setUseF);
  storageSetBool("set_use_inhg", setUseInHg);
  storageSetBool("set_use_mph", setUseMph);
  storageSetI32("set_font_sz", setFontSize);
  storageSetI32("set_bl_lvl", setBrightness);
  storageSetBool("set_theme_dark", setThemeDark);
  storageSetI32("set_accent_idx", setAccentIdx);
  storageSetI32("set_timeout_idx", setTimeoutIdx);
  storageSetI32("set_pomo_w", setPomoWorkMin);
  storageSetI32("set_pomo_b", setPomoBreakMin);
  storageSetI32("set_temp_off", (int)(setTempOffset * 10.0f));
}

static int formatSel = 0;
static const char* FORMAT_ITEMS[] = { "QUOTES", "NEWS", "SKETCHES", "DOCUMENTS", "CALCULATOR" }; // ENHANCED: Added calculator formatting
static const int FORMAT_COUNT = sizeof(FORMAT_ITEMS) / sizeof(FORMAT_ITEMS[0]);

static void showFormatOptions() {
  screen = SCR_FORMAT_OPTIONS;
  formatSel = 0;
  uiFormatOptionsDrawFull(formatSel, FORMAT_ITEMS, FORMAT_COUNT);
}

static volatile bool formatBusy = false;
static TaskHandle_t formatTaskH = nullptr;

static void formatTask(void* arg) {
  int idx = (int)(intptr_t)arg;
  if (!LittleFS.begin(true)) {
    formatBusy = false;
    vTaskDelete(nullptr);
    return;
  }
  const char* type = FORMAT_ITEMS[idx];
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    const char* fname = f.name();
    bool del = false;
    if      (strcmp(type, "QUOTES")    == 0 && strstr(fname, "quote"))  del = true;
    else if (strcmp(type, "NEWS")      == 0 && strstr(fname, "news"))   del = true;
    else if (strcmp(type, "SKETCHES")  == 0 && strstr(fname, "sketch")) del = true;
    else if (strcmp(type, "DOCUMENTS") == 0 &&
             (strstr(fname, "/docs/") || strstr(fname, ".txt") || strstr(fname, ".pdf"))) del = true;
    if (del) {
      char path[64];
      snprintf(path, sizeof(path), "%s%s", fname[0]=='/' ? "" : "/", fname);
      f.close();
      LittleFS.remove(path);
      root = LittleFS.open("/");
    }
    f = root.openNextFile();
    // PRO UPGRADE: Yield slightly to prevent OS watchdog resets and keep UI fully responsive
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  formatBusy = false;
  vTaskDelete(nullptr);
}

static void granularFormat(int idx) {
  if (formatBusy) return;
  formatBusy = true;
  uiShowScreenTitle("FORMATTING... (BACKGROUND)");
  xTaskCreatePinnedToCore(formatTask, "formatTask", 4096, (void*)(intptr_t)idx, 1, &formatTaskH, 0);
}

static void showSettings() {
  screen = SCR_SETTINGS;
  clampSettingsViewport();
  char v0[24], v1[24], v2[24], v3[24], v4[24], v5[24], v6[24], v7[24], v8[24], v9[24];
  const char* blStr;
  if (setBrightness <= 0) blStr = "Off";
  else if (setBrightness == 1) blStr = "25%";
  else if (setBrightness == 2) blStr = "50%";
  else if (setBrightness == 3) blStr = "75%";
  else blStr = "100%";
  snprintf(v0, sizeof(v0), "%s", blStr);
  snprintf(v1, sizeof(v1), "%s", setThemeDark ? "Dark" : "Light");
  
  const char* accentLabels[] = { "Cyber Blue", "Emerald", "Sunset", "Royal" };
  snprintf(v2, sizeof(v2), "%s", accentLabels[setAccentIdx]);
  
  snprintf(v3, sizeof(v3), "%s", setFontSize == 0 ? "Small" : (setFontSize == 2 ? "Large" : "Default"));
  snprintf(v4, sizeof(v4), "%s", setUseF ? "F" : "C");
  snprintf(v5, sizeof(v5), "%s", setUseInHg ? "inHg" : "hPa");
  snprintf(v6, sizeof(v6), "%s", TIMEOUT_LABELS[setTimeoutIdx]);
  snprintf(v7, sizeof(v7), "%d m", setPomoWorkMin);
  snprintf(v8, sizeof(v8), "%d m", setPomoBreakMin);
  snprintf(v9, sizeof(v9), "%.1f C", setTempOffset);
  uiSettingsDrawFull(settingsSel, settingsTop, v0, v1, v2, v3, v4, v5, v6, v7, v8, v9);
}

static void applyBrightness() {
  uiSetBrightnessLevel(setBrightness);
  uiSetBacklight(setBrightness > 0);
}

static float readTempC() { 
  float t = sensorsTempC();
  if (isnan(t)) return t;
  return t + setTempOffset; 
}
static float readHum()   { return sensorsHumidity(); }
static float readPress() { return sensorsPressureHPa(); }

static uint32_t tpWakeMs = 0; // when BMP was last woken (for warm-up window)

static void enterTempPressureScreen() {
  screen = SCR_TEMP_PRESSURE;
  // Wake the BMP280 so it starts sampling the actual surrounding environment
  sensorsWakeBMP();
  tpWakeMs = millis();
  // Initial draw with NAN (sensor is warming up — screen shows '--.-')
  uiTempPressDrawFull(sensorsRtcOk(), NAN, NAN, NAN, setUseF, setUseInHg);
}

static inline bool inTempUnitsBtn(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  int bx = 8, by = FOOT_Y + 4, bw = 120, bh = 26;
  return (x >= bx && x < bx + bw && y >= by && y < by + bh);
}
static inline bool inTempRefreshBtn(int x, int y) {
  const int FOOT_Y = TFT_H - UI_FOOTER_H;
  int bx = TFT_W - 92, by = FOOT_Y + 4, bw = 84, bh = 26;
  return (x >= bx && x < bx + bw && y >= by && y < by + bh);
}

static int calcFocusR = 0;
static int calcFocusC = 0;
static char calcExpr[128] = {0};  // ENHANCED: Increased buffer size for longer expressions
static char calcResult[64] = {0}; // ENHANCED: Increased result buffer
static double calcAccum = 0.0;
static char calcOp = 0;
static bool calcFresh = true;
static bool calcScientificMode = false; // ENHANCED: Added scientific mode toggle
bool calcError = false; // ENHANCED: Added error state tracking (non-static for extern access)

static int alarmSel = 0;
static bool alarmEditMode = false;
static int alarmEditField = 0; // 0=H, 1=M, 2=AM/PM, 3=E
static void showAlarms() {
  screen = SCR_ALARM;
  uiAlarmsDrawFull(alarmSel, alarmEditMode, alarmEditField, alarmsGet(0), 3);
}

// ---- Calculation history: fixed ring buffer, zero heap allocation ----
static const int MAX_HISTORY = 10;
static CalcHistoryEntry calcHistoryBuf[MAX_HISTORY];  // static array, never malloc'd
static int   calcHistoryHead = 0;   // index where next entry goes
int   calcHistoryCount = 0;  // how many valid entries (0–MAX_HISTORY)
int historyViewIndex = -1;          // -1 = not viewing history

// Compatibility helpers used by the rest of the code
static inline int calcHistorySize() { return calcHistoryCount; }
static inline bool calcHistoryEmpty() { return calcHistoryCount == 0; }
static inline CalcHistoryEntry& calcHistoryAt(int i) {
  // i=0 is most-recent
  int idx = (calcHistoryHead - 1 - i + MAX_HISTORY * 2) % MAX_HISTORY;
  return calcHistoryBuf[idx];
}

static void calcReset() {
  calcExpr[0] = 0;
  calcResult[0] = 0;
  calcAccum = 0.0;
  calcOp = 0;
  calcFresh = true;
  calcError = false; // ENHANCED: Reset error state
  historyViewIndex = -1; // ENHANCED: Exit history view
}

// ENHANCED: Function to add calculation to history
static void calcAddToHistory(const char* expr, const char* result) {
  CalcHistoryEntry& e = calcHistoryBuf[calcHistoryHead];
  strncpy(e.expression, expr,   sizeof(e.expression) - 1);
  e.expression[sizeof(e.expression) - 1] = 0;
  strncpy(e.result,     result, sizeof(e.result)     - 1);
  e.result[sizeof(e.result) - 1] = 0;
  e.timestamp = millis();
  calcHistoryHead = (calcHistoryHead + 1) % MAX_HISTORY;
  if (calcHistoryCount < MAX_HISTORY) calcHistoryCount++;
}

// ENHANCED: Backspace function for editing numbers
static void calcBackspace() {
  if (calcError) {
    calcError = false;
    calcExpr[0] = 0;
    calcResult[0] = 0;
    return;
  }
  
  size_t l = strlen(calcExpr);
  if (l > 0) {
    calcExpr[l - 1] = 0;
  }
}

// ENHANCED: Mathematical functions for scientific calculator
static double calcFactorial(double n) {
  if (n < 0 || n != floor(n) || n > 170) return NAN; // Prevent overflow
  if (n <= 1) return 1;
  double result = 1;
  for (int i = 2; i <= (int)n; i++) {
    result *= i;
  }
  return result;
}

static void calcShowHistory() {
  if (calcHistoryEmpty()) return;
  historyViewIndex = 0;
  strncpy(calcExpr,   calcHistoryAt(0).expression, sizeof(calcExpr)   - 1);
  strncpy(calcResult, calcHistoryAt(0).result,     sizeof(calcResult) - 1);
  calcExpr[sizeof(calcExpr)-1] = 0;
  calcResult[sizeof(calcResult)-1] = 0;
  uiCalcDrawFull(calcExpr, calcResult);
}

static void calcHistoryNext() {
  if (calcHistoryEmpty() || historyViewIndex < 0) return;
  historyViewIndex++;
  if (historyViewIndex >= calcHistorySize()) {
    historyViewIndex = -1;
    calcReset();
  } else {
    strncpy(calcExpr,   calcHistoryAt(historyViewIndex).expression, sizeof(calcExpr)   - 1);
    strncpy(calcResult, calcHistoryAt(historyViewIndex).result,     sizeof(calcResult) - 1);
    calcExpr[sizeof(calcExpr)-1] = 0;
    calcResult[sizeof(calcResult)-1] = 0;
  }
  uiCalcDrawFull(calcExpr, calcResult);
}

static void calcHistoryPrev() {
  if (calcHistoryEmpty()) return;
  if (historyViewIndex < 0) historyViewIndex = 0;
  else if (historyViewIndex > 0) historyViewIndex--;
  strncpy(calcExpr,   calcHistoryAt(historyViewIndex).expression, sizeof(calcExpr)   - 1);
  strncpy(calcResult, calcHistoryAt(historyViewIndex).result,     sizeof(calcResult) - 1);
  calcExpr[sizeof(calcExpr)-1] = 0;
  calcResult[sizeof(calcResult)-1] = 0;
  uiCalcDrawFull(calcExpr, calcResult);
}

static void showCalc() {
  screen = SCR_CALC;
  uiCalcDrawFull(calcExpr, calcResult);
}

// ENHANCED: Updated calculator keys with Clear (C) and Backspace (<)
static const char* CALC_KEYS[5][4] = {
  { "C",  "<",  "%",  "/" },  // ENHANCED: Added Clear and Backspace (using < for visibility)
  { "7",  "8",  "9",  "*" },
  { "4",  "5",  "6",  "-" },
  { "1",  "2",  "3",  "+" },
  { "0",  ".",  "=",  "+/-" }   // ENHANCED: Added plus/minus toggle (using +/- for visibility)
};

static void calcAppend(const char* s) {
  if (calcError) return; // ENHANCED: Don't append if in error state
  
  size_t l = strlen(calcExpr);
  size_t sl = strlen(s);
  if (l + sl >= sizeof(calcExpr) - 1) return;
  strcat(calcExpr, s);
}

static bool isDigitOrDot(char c) { return (c >= '0' && c <= '9') || c == '.'; }

static void calcApplyOp(double v) {
  if (calcOp == 0) { calcAccum = v; return; }
  if (calcOp == '+') calcAccum += v;
  else if (calcOp == '-') calcAccum -= v;
  else if (calcOp == '*') calcAccum *= v;
  else if (calcOp == '/') { 
    if (v == 0.0) {
      calcError = true;
      strcpy(calcResult, "Error: Div by 0");
      return;
    }
    calcAccum /= v; 
  }
  else if (calcOp == '%') {
    if (v == 0.0) {
      calcError = true;
      strcpy(calcResult, "Error: Mod by 0");
      return;
    }
    calcAccum = fmod(calcAccum, v);
  }
}

static void calcHandleToken(const char* tok) {
  if (calcError && strcmp(tok, "C") != 0) return; // ENHANCED: Only allow Clear when in error
  
  // Clear button - ENHANCED: Complete reset
  if (strcmp(tok, "C") == 0) {
    calcReset();
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Backspace button - ENHANCED: New functionality
  if (strcmp(tok, "<") == 0) { // ENHANCED: Changed from "←" to "<"
    calcBackspace();
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Plus/minus toggle - ENHANCED: New functionality
  if (strcmp(tok, "+/-") == 0) { // ENHANCED: Changed from "±" to "+/-"
    if (strlen(calcExpr) > 0) {
      if (calcExpr[0] == '-') {
        // Remove negative sign
        memmove(calcExpr, calcExpr + 1, strlen(calcExpr));
      } else {
        // Add negative sign
        memmove(calcExpr + 1, calcExpr, strlen(calcExpr) + 1);
        calcExpr[0] = '-';
      }
    }
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Percentage - ENHANCED: New functionality
  if (strcmp(tok, "%") == 0) {
    double v = atof(calcExpr);
    calcApplyOp(v);
    if (!calcError) {
      snprintf(calcResult, sizeof(calcResult), "%.8g", calcAccum);
      // Add to history
      char fullExpr[128];
      snprintf(fullExpr, sizeof(fullExpr), "%s%%", calcExpr);
      calcAddToHistory(fullExpr, calcResult);
    }
    calcExpr[0] = 0;
    calcOp = 0;
    calcFresh = true;
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Equals - ENHANCED: Enhanced with history
  if (strcmp(tok, "=") == 0) {
    double v = atof(calcExpr);
    calcApplyOp(v);
    
    if (!calcError) {
      snprintf(calcResult, sizeof(calcResult), "%.8g", calcAccum);
      // Add to history - ENHANCED: New functionality
      char fullExpr[128];
      if (calcOp != 0) {
        // Try to reconstruct the full expression for history
        snprintf(fullExpr, sizeof(fullExpr), "%.8g %c %s", 
                 (calcOp == '+') ? calcAccum - v :
                 (calcOp == '-') ? calcAccum + v :
                 (calcOp == '*') ? calcAccum / v :
                 (calcOp == '/') ? calcAccum * v : calcAccum, 
                 calcOp, calcExpr);
      } else {
        strcpy(fullExpr, calcExpr);
      }
      calcAddToHistory(fullExpr, calcResult);
    }
    
    calcExpr[0] = 0;
    calcOp = 0;
    calcFresh = true;
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Operators - ENHANCED: Enhanced handling
  if (strlen(tok) == 1 && !isDigitOrDot(tok[0])) {
    double v = atof(calcExpr);
    calcApplyOp(v);
    
    if (!calcError) {
      calcOp = tok[0];
      calcExpr[0] = 0;
      calcFresh = true;
      snprintf(calcResult, sizeof(calcResult), "%.8g", calcAccum);
    }
    
    uiCalcDrawFull(calcExpr, calcResult);
    return;
  }
  
  // Numbers and decimal point - ENHANCED: Enhanced validation
  if (calcFresh) {
    calcExpr[0] = 0;
    calcFresh = false;
  }
  
  // Prevent multiple decimal points
  if (strcmp(tok, ".") == 0 && strchr(calcExpr, '.') != nullptr) {
    return;
  }
  
  calcAppend(tok);
  uiCalcDrawFull(calcExpr, calcResult);
}

// STOPWATCH
static bool swRunning = false;
static uint32_t swStartMs = 0;
static uint32_t swAccumMs = 0;

static inline bool inLeftFooterBtn(int x, int y) {
  // Left button: x=6, width=(TFT_W-30)/2, y=TFT_H-UI_FOOTER_H+4, height=26
  const int btnW = (TFT_W - 30) / 2;
  const int y0   = TFT_H - UI_FOOTER_H + 4;
  return (y >= y0 && y < y0 + 26 && x >= 6 && x < 6 + btnW);
}
static inline bool inRightFooterBtn(int x, int y) {
  // Right button: x=6+btnW+6, width=(TFT_W-30)/2
  const int btnW  = (TFT_W - 30) / 2;
  const int rightX = 6 + btnW + 6;
  const int y0    = TFT_H - UI_FOOTER_H + 4;
  return (y >= y0 && y < y0 + 26 && x >= rightX && x < rightX + btnW);
}

static void showStopwatch() {
  screen = SCR_STOPWATCH;
  uiStopwatchDrawFull(swAccumMs + (swRunning ? (millis() - swStartMs) : 0), swRunning);
}

// TIMER
static bool tmRunning = false;
static uint32_t tmRemainMs = 60000;
static uint32_t tmLastTick = 0;
static int tmPresetIdx = 0; // 0=1m,1=3m,2=5m

static void timerApplyPreset() {
  const uint32_t presets[] = { 60000UL, 180000UL, 300000UL };
  if (tmPresetIdx < 0) tmPresetIdx = 0;
  if (tmPresetIdx > 2) tmPresetIdx = 2;
  tmRemainMs = presets[tmPresetIdx];
}

static void showTimer() {
  screen = SCR_TIMER;
  uiTimerDrawFull(tmRemainMs, tmRunning, tmPresetIdx);
}

// UNIT CONVERTER
// mode: 0=C<->F, 1=hPa<->inHg
static int ucMode = 0;
static float ucInVal = 25.0f;
static float ucOutVal = 77.0f;

static void unitConvRecalc() {
  if (ucMode == 0) {
    ucOutVal = ucInVal * 9.0f / 5.0f + 32.0f;
  } else {
    ucOutVal = ucInVal * 0.0295299830714f;
  }
}

static inline bool inUcMinus(int x, int y) {
  // Left minus button: x=6, width=(TFT_W-30)/2
  const int btnW = (TFT_W - 30) / 2;
  const int y0   = TFT_H - UI_FOOTER_H + 4;
  return (y >= y0 && y < y0 + 26 && x >= 6 && x < 6 + btnW);
}
static inline bool inUcPlus(int x, int y) {
  // Right plus button: x=6+btnW+6, width=(TFT_W-30)/2
  const int btnW  = (TFT_W - 30) / 2;
  const int rightX = 6 + btnW + 6;
  const int y0    = TFT_H - UI_FOOTER_H + 4;
  return (y >= y0 && y < y0 + 26 && x >= rightX && x < rightX + btnW);
}

static void showUnitConv() {
  screen = SCR_UNITCONV;
  const char* inLabel = (ucMode == 0) ? "C" : "hPa";
  const char* outLabel = (ucMode == 0) ? "F" : "inHg";
  uiUnitConvDrawFull(ucMode, ucInVal, ucOutVal, inLabel, outLabel);
}

static void enterQuotesScreen() {
  screen = SCR_QUOTES;
  uiQuotesScreenDrawFull(wifiOk(), quotesIsBusy(), quotesGetCached());
}

static void enterQuotesStudyMode() {
  screen = SCR_QUOTES_STUDY;
  uiQuotesScreenDrawStudy(quotesGetCached());
}

static void enterNewsStudyMode() {
  newsStudyScrollOffset = 0;
  screen = SCR_NEWS_STUDY;
  uiNewsScreenDrawStudy(newsGetCached(), 0);
}

static bool newsCacheLooksValid(const NewsData& n) {
  if (n.count <= 0) return false;
  if (!n.updated[0]) return false;
  if (strcmp(n.updated, "None") == 0) return false;
  return true;
}

static void showNewsChannels() {
  screen = SCR_NEWS_CHANNELS;
  newsChanSel = 0;
  uiNewsChannelsMenuDrawFull(newsChanSel, 0, NEWS_CHAN_ITEMS, NEWS_CHAN_COUNT);
}

static void showNewsCategories() {
  screen = SCR_NEWS_CATEGORIES;
  newsCatSel = 0;
  const char* chanName = (newsChanSel == 1) ? "GLOBAL" : "INDIA";
  uiNewsCategoriesMenuDrawFull(newsCatSel, 0, NEWS_CAT_ITEMS, NEWS_CAT_COUNT, chanName);
}

static void enterNewsScreen() {
  screen = SCR_NEWS;
  newsSetChannel(newsChanSel);
  newsSetCategory(newsCatSel);
  
  const NewsData& n = newsGetCached();
  
  // If we only have the placeholder, try to fetch if online
  bool isPlaceholder = (n.count <= 1 && (strcmp(n.updated, "None") == 0 || n.updated[0] == 0));
  
  if (isPlaceholder && WiFi.status() == WL_CONNECTED && !newsIsBusy()) {
    newsRequestFetch();
    uiNewsScreenDrawFull(wifiOk(), true, n);
  } else {
    uiNewsScreenDrawFull(wifiOk(), newsIsBusy(), n);
  }
}

static void enterNewsStudyScreen() {
  newsStudyScrollOffset = 0;
  screen = SCR_NEWS_STUDY;
  uiNewsScreenDrawStudy(newsGetCached(), 0);
}

static void enterGamesScreen() {
  gamesEnter();
  showGamesMenu();
}

// POMODORO
static bool pomoRunning = false;
static bool pomoIsWork = true; // true=Work, false=Break
static uint32_t pomoRemainMs = 0;
static uint32_t pomoLastTick = 0;
static int pomoCycles = 0;

static uint16_t currentSketchColor = ILI9341_WHITE;
static int lastSketchX = -1, lastSketchY = -1;
static int prevSketchX = -1, prevSketchY = -1; // For curve smoothing

static void enterSketchScreen() {
  screen = SCR_SKETCH;
  lastSketchX = -1;
  lastSketchY = -1;
  // Initialize color based on theme
  currentSketchColor = (uiColorBg() == ILI9341_WHITE) ? ILI9341_BLACK : ILI9341_WHITE;
  touchSetResilience(true);
  
  // Clear buffer for a FRESH start from menu
  uiSketchDrawFull(currentSketchColor);
  uiSketchClear(); 
}

static int gallerySel = 0;
static int galleryTop = 0;

// ---------- Documents ----------
static WebServer docsServer(80);
static bool docsServerStarted = false;
static File uploadFile;
static int docsSel  = 0;
static int docsTop  = 0;

// Fixed-size docs state — no heap String objects
static char   currentDocName[64]  = {0};
static char   docContent[102400]  = {0};   // 100 KB max, stack in .bss (not heap)
static int    docPageOffsets[512] = {0};   // up to 512 pages
static int    docPageCount        = 0;
static int    docCurrentPage      = 0;
static bool   viewingDoc          = false;

static void saveDocProgress(const char* name, int page) {
  // Read existing entries into a temp buffer (no String)
  static char lineBuf[4096];
  lineBuf[0] = 0;
  bool found = false;

  File fr = LittleFS.open("/doc_progress.txt", FILE_READ);
  if (fr) {
    while (fr.available()) {
      char tmp[256];
      int len = fr.readBytesUntil('\n', tmp, sizeof(tmp) - 1);
      tmp[len] = 0;
      if (len == 0) continue;
      // find '=' separator
      char* sep = strchr(tmp, '=');
      if (!sep) continue;
      *sep = 0;  // split: tmp = key, sep+1 = value
      if (strcmp(tmp, name) == 0) {
        char entry[128];
        snprintf(entry, sizeof(entry), "%s=%d\n", name, page);
        strncat(lineBuf, entry, sizeof(lineBuf) - strlen(lineBuf) - 1);
        found = true;
      } else {
        *sep = '=';  // restore
        strncat(lineBuf, tmp, sizeof(lineBuf) - strlen(lineBuf) - 1);
        strncat(lineBuf, "\n", sizeof(lineBuf) - strlen(lineBuf) - 1);
      }
    }
    fr.close();
  }

  if (!found) {
    char entry[128];
    snprintf(entry, sizeof(entry), "%s=%d\n", name, page);
    strncat(lineBuf, entry, sizeof(lineBuf) - strlen(lineBuf) - 1);
  }

  File fw = LittleFS.open("/doc_progress.txt", FILE_WRITE);
  if (fw) { fw.print(lineBuf); fw.close(); }
}

static int loadDocProgress(const char* name) {
  File f = LittleFS.open("/doc_progress.txt", FILE_READ);
  if (!f) return 0;
  int lastPage = 0;
  while (f.available()) {
    char tmp[256];
    int len = f.readBytesUntil('\n', tmp, sizeof(tmp) - 1);
    tmp[len] = 0;
    char* sep = strchr(tmp, '=');
    if (sep) {
      *sep = 0;
      if (strcmp(tmp, name) == 0) lastPage = atoi(sep + 1);
    }
  }
  f.close();
  return lastPage;
}

static void startDocsServer() {
  if (docsServerStarted) return;
  
  if (MDNS.begin("smile")) {
    Serial.println("MDNS responder started: http://smile.local");
  }

  docsServer.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;background:#121212;color:#eee;text-align:center;padding:20px;}";
    html += ".card{background:#1e1e1e;padding:30px;border-radius:15px;box-shadow:0 10px 20px rgba(0,0,0,0.5);max-width:400px;margin:auto;}";
    html += "h1{color:#00bcd4;} .btn{background:#00bcd4;color:white;border:none;padding:15px 30px;border-radius:30px;font-weight:bold;cursor:pointer;margin-top:20px;width:100%;}";
    html += "#status{margin-top:20px;color:#888;}</style></head><body>";
    html += "<div class='card'><h1>Smile TV PRO</h1><p>Smart Document Reader</p>";
    html += "<input type='file' id='fileInput' accept='.txt' style='display:none'>";
    html += "<button class='btn' onclick='document.getElementById(\"fileInput\").click()'>Select .txt File</button>";
    html += "<div id='status'>Ready to upload</div></div>";
    html += "<script>";
    html += "document.getElementById('fileInput').onchange = function(e){";
    html += "  var file = e.target.files[0]; if(!file) return;";
    html += "  var status = document.getElementById('status'); status.innerText = 'Uploading ' + file.name + '...';";
    html += "  var reader = new FileReader();";
    html += "  reader.onload = function(e){ uploadText(file.name, e.target.result); };";
    html += "  reader.readAsText(file);";
    html += "};";
    html += "function uploadText(name, text){";
    html += "  var status = document.getElementById('status'); status.innerText = 'Uploading to Smile TV...';";
    html += "  var formData = new FormData();";
    html += "  var blob = new Blob([text], {type: 'text/plain'});";
    html += "  formData.append('upload', blob, name);";
    html += "  fetch('/upload', {method: 'POST', body: formData}).then(function(){";
    html += "    status.innerText = 'Upload Successful! Open on Gadget.'; status.style.color = '#4caf50';";
    html += "  }).catch(function(){ status.innerText = 'Upload Failed!'; status.style.color = '#f44336'; });";
    html += "}";
    html += "</script></body></html>";
    docsServer.send(200, "text/html", html);
  });



  docsServer.on("/upload", HTTP_POST, []() {
    docsServer.send(200, "text/plain", "Upload Successful! Check your Smile TV.");
  }, []() {
    HTTPUpload& upload = docsServer.upload();
    String filename = upload.filename;
    
    // Improved Filename Cleaning
    int lastS = filename.lastIndexOf('/');
    if (lastS != -1) filename = filename.substring(lastS + 1);
    lastS = filename.lastIndexOf('\\');
    if (lastS != -1) filename = filename.substring(lastS + 1);
    
    if (filename.length() == 0) filename = "unnamed.txt";
    if (!filename.startsWith("/")) filename = "/" + filename;

    if (upload.status == UPLOAD_FILE_START) {
      if (uploadFile) uploadFile.close();
      uploadFile = LittleFS.open(filename, FILE_WRITE);
      if (uploadFile) {
        Serial.printf("DOCS: UPLOAD START: %s\n", filename.c_str());
      } else {
        Serial.printf("DOCS: UPLOAD FAILED TO OPEN: %s\n", filename.c_str());
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) {
        uploadFile.write(upload.buf, upload.currentSize);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (uploadFile) {
        uploadFile.close();
        Serial.printf("DOCS: UPLOAD SUCCESS: %s, size: %u\n", filename.c_str(), upload.totalSize);
      }
    }
  });

  docsServer.begin();
  docsServerStarted = true;
  Serial.println("Docs Server started");
}

static std::vector<String> getDocFiles() {
  std::vector<String> files;
  
  Serial.println("DOCS: Starting LittleFS scan...");
  
  File root = LittleFS.open("/", "r");
  if (!root || !root.isDirectory()) {
    Serial.println("DOCS: FAILED to open root directory!");
    return files;
  }
  
  int totalEntries = 0;
  File file = root.openNextFile();
  while (file) {
    totalEntries++;
    String fullName = String(file.name());
    
    // Log every single entry found for high-precision debugging
    Serial.printf("DOCS: [%d] Entry: '%s', Size: %u, Dir: %s\n", 
                  totalEntries, fullName.c_str(), (unsigned int)file.size(), 
                  file.isDirectory() ? "YES" : "NO");

    if (!file.isDirectory()) {
      String lowerName = fullName;
      lowerName.toLowerCase();
      
      // Filter strictly for user .txt files (excluding internal progress and system files)
      if (lowerName.endsWith(".txt") && 
          !lowerName.equals("doc_progress.txt") && 
          !lowerName.equals("/doc_progress.txt") && 
          !lowerName.equals("settings.bin") &&
          !lowerName.equals("weather.bin")) {
        
        files.push_back(fullName);
        Serial.printf("DOCS: -> ACCEPTED (full path): '%s'\n", fullName.c_str());
      }
    }
    
    file.close(); // CRITICAL: Close handle to prevent descriptor exhaustion
    file = root.openNextFile();
  }
  
  root.close();
  std::sort(files.begin(), files.end());
  Serial.printf("DOCS: Scan finished. Total entries: %d, Accepted .txt: %d\n", totalEntries, files.size());
  return files;
}

// Fixed-size file list — max 32 documents, 64 chars each
static char   docFileList[32][64];
static int    docFileCount = 0;

static void refreshDocFiles() {
  docFileCount = 0;
  File root = LittleFS.open("/", "r");
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file && docFileCount < 32) {
    if (!file.isDirectory()) {
      char lname[64];
      strncpy(lname, file.name(), sizeof(lname)-1); lname[63]=0;
      // toLowerCase equivalent
      for (int i=0; lname[i]; i++) lname[i] = tolower((unsigned char)lname[i]);
      // accept only user .txt files, reject internals
      if (strstr(lname, ".txt") &&
          strcmp(lname, "doc_progress.txt") != 0 &&
          strcmp(lname, "/doc_progress.txt") != 0 &&
          strcmp(lname, "settings.bin") != 0 &&
          strcmp(lname, "weather.bin") != 0) {
        strncpy(docFileList[docFileCount], file.name(), 63);
        docFileList[docFileCount][63] = 0;
        docFileCount++;
      }
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  // sort alphabetically
  for (int i=0; i<docFileCount-1; i++)
    for (int j=i+1; j<docFileCount; j++)
      if (strcmp(docFileList[i], docFileList[j]) > 0) {
        char tmp[64]; memcpy(tmp, docFileList[i], 64);
        memcpy(docFileList[i], docFileList[j], 64);
        memcpy(docFileList[j], tmp, 64);
      }
}

static void docsRedrawRow(int i, int count, int rowH, int topH) {
  int idx = docsTop + i;
  if (idx < 0 || idx >= count) return;
  int y = uiBodyY0() + topH + i * rowH;
  bool selected = (idx == docsSel);
  uiTft().fillRect(0, y, uiTft().width(), rowH, selected ? uiColorSelBg() : uiColorBg());
  uiTft().setTextColor(selected ? uiColorAccent() : uiColorText());
  uiTft().setFont(&FreeSans9pt7b);
  uiTft().setCursor(10, y + 18);
  // Show only filename (after last '/')
  const char* full = docFileList[idx];
  const char* slash = strrchr(full, '/');
  uiTft().print(slash ? slash + 1 : full);
  uiTft().drawFastHLine(0, y + rowH - 1, uiTft().width(), uiColorDivider());
}

static void docsRedrawSelectionOnly(int prevSel, int prevTop) {
  const int rowH = 28;
  const int topH = 5;
  const int bodyH = uiBodyH() - 34 - topH;
  const int maxRows = bodyH / rowH;
  if (docsTop != prevTop) { showDocsMenu(); return; }
  int oldRow = prevSel - docsTop;
  if (oldRow >= 0 && oldRow < maxRows) docsRedrawRow(oldRow, docFileCount, rowH, topH);
  int newRow = docsSel - docsTop;
  if (newRow >= 0 && newRow < maxRows) docsRedrawRow(newRow, docFileCount, rowH, topH);
}

static void showDocsMenu() {
  refreshDocFiles();
  int count = docFileCount;
  if (docsSel < 0) docsSel = 0;
  if (docsSel >= count) docsSel = (count > 0 ? count - 1 : 0);
  if (docsTop < 0) docsTop = 0;
  const int rowH = 28;
  const int topH = 5;
  const int bodyH = uiBodyH() - 34 - topH;
  const int maxRows = bodyH / rowH;
  int maxTop = (count > maxRows) ? (count - maxRows) : 0;
  if (docsTop > maxTop) docsTop = maxTop;
  if (docsSel < docsTop) docsTop = docsSel;
  if (docsSel >= docsTop + maxRows) docsTop = docsSel - (maxRows - 1);
  uiTft().fillScreen(uiColorBg());
  uiShowScreenTitle("DOCUMENTS");
  if (count == 0) {
    uiTft().setFont(&FreeSans12pt7b);
    uiTft().setTextColor(uiColorSub());
    uiTft().setCursor(40, uiBodyY0() + 80);
    uiTft().print("No .txt files found");
    uiTft().setFont(&FreeSans9pt7b);
    uiTft().setCursor(40, uiBodyY0() + 110);
    uiTft().print("Upload from laptop.");
  } else {
    for (int i = 0; i < maxRows; i++) docsRedrawRow(i, count, rowH, topH);
  }
  const int fy = TFT_H - 34;
  uiTft().fillRect(0, fy, TFT_W, 34, uiColorBg());
  uiTft().drawFastHLine(0, fy, TFT_W, uiColorDivider());
  uiTft().setFont(nullptr);
  uiTft().setTextColor(uiColorSub());
  uiTft().setCursor(5, fy + 12);
  uiTft().print("IP:"); uiTft().print(WiFi.localIP().toString());
  uint32_t totalB = LittleFS.totalBytes(), usedB = LittleFS.usedBytes();
  char fsInfo[48];
  snprintf(fsInfo, sizeof(fsInfo), "%.1f/%.1f KB (%d Files)",
           usedB/1024.0f, totalB/1024.0f, count);
  uiTft().setCursor(5, fy + 24); uiTft().print(fsInfo);
  if (count > 0) {
    uiTft().fillRoundRect(TFT_W - 75, fy + 4, 70, 26, 4, 0x8800);
    uiTft().setTextColor(ILI9341_WHITE);
    uiTft().setFont(&FreeSans9pt7b);
    uiTft().setCursor(TFT_W - 70, fy + 22);
    uiTft().print("DEL");
  }
  uiTft().setTextColor(uiColorSub());
  uiTft().setFont(nullptr);
  uiTft().setCursor(TFT_W/2 - 25, fy + 12);
  uiTft().print("Long=Home");
}

static void openDoc(const char* name) {
  uiFadeOut(100);
  strncpy(currentDocName, name, sizeof(currentDocName)-1);
  currentDocName[sizeof(currentDocName)-1] = 0;
  viewingDoc = true;
  docPageCount = 0;
  docContent[0] = 0;

  char path[68];
  snprintf(path, sizeof(path), "%s%s", name[0]=='/' ? "" : "/", name);

  // Extract filename for title
  const char* slash = strrchr(path, '/');
  const char* displayTitle = slash ? slash+1 : path;

  File f = LittleFS.open(path, "r");
  if (f) {
    uint32_t sz = f.size();
    if (sz == 0) {
      strncpy(docContent, "[Empty File]", sizeof(docContent)-1);
    } else if (sz >= sizeof(docContent)-1) {
      snprintf(docContent, sizeof(docContent),
               "File too large (Max %dKB).\nYour file: %luKB",
               (int)(sizeof(docContent)/1024), (unsigned long)(sz/1024));
    } else {
      uint32_t idx = 0;
      while (f.available() && idx < sizeof(docContent)-1) {
        docContent[idx++] = (char)f.read();
        if ((idx & 0x3FF) == 0) yield();
      }
      docContent[idx] = 0;
    }
    f.close();
  } else {
    snprintf(docContent, sizeof(docContent), "Error: Could not open file.\nPath: %s", path);
  }

  // CRLF -> LF
  { char* p = docContent; char* w = docContent;
    while (*p) { if (*p=='\r' && *(p+1)=='\n') { p++; } *w++=*p++; } *w=0; }

  // Paginate into fixed docPageOffsets[]
  uiTft().setFont(&FreeSans9pt7b);
  const int maxWidth  = uiTft().width() - 20;
  const int lineH     = 22;
  const int maxHeight = uiBodyH() - 30;
  docPageOffsets[0] = 0;
  docPageCount = 1;
  int curY = 0;
  int si = 0;
  int contentLen = (int)strlen(docContent);
  while (si < contentLen) {
    if ((si & 0x1FF) == 0) yield();
    int nextNL = si;
    while (nextNL < contentLen && docContent[nextNL] != '\n') nextNL++;
    int pStart = si;
    while (pStart < nextNL) {
      char tmp[160]; int tlen = 0;
      int bi = pStart;
      int lastSp = -1;
      while (bi < nextNL) {
        tmp[tlen++] = docContent[bi];
        tmp[tlen]   = 0;
        int16_t x1,y1; uint16_t w,h;
        uiTft().getTextBounds(tmp, 0, 0, &x1, &y1, &w, &h);
        if ((int)w > maxWidth) break;
        if (docContent[bi] == ' ') lastSp = bi;
        bi++;
      }
      pStart = (bi < nextNL) ? (lastSp>=0 ? lastSp+1 : bi) : nextNL;
      curY += lineH;
      if (curY + lineH > maxHeight && docPageCount < 512) {
        docPageOffsets[docPageCount++] = pStart;
        curY = 0;
      }
    }
    si = nextNL + 1;
  }

  docCurrentPage = loadDocProgress(currentDocName);
  if (docCurrentPage >= docPageCount) docCurrentPage = 0;

  uiTft().fillScreen(uiColorBg());
  uiShowScreenTitle(displayTitle);
  drawDocContent();
  uiFadeIn(100);
}

static void drawDocContent() {
  uiTft().fillRect(0, uiBodyY0(), uiTft().width(), TFT_H - uiBodyY0(), uiColorBg());
  uiTft().setFont(&FreeSans9pt7b);
  uiTft().setTextColor(uiColorText());
  if (docPageCount == 0) return;
  int startIdx = docPageOffsets[docCurrentPage];
  int endIdx   = (docCurrentPage + 1 < docPageCount) ? docPageOffsets[docCurrentPage+1] : (int)strlen(docContent);
  const int maxWidth  = uiTft().width() - 20;
  const int lineH     = 22;
  int curX = 10, curY = uiBodyY0() + 20;
  int pStart = startIdx;
  int contentLen = (int)strlen(docContent);
  while (pStart < endIdx) {
    int nextNL = pStart;
    while (nextNL < endIdx && docContent[nextNL] != '\n') nextNL++;
    int lineStart = pStart;
    while (lineStart < nextNL) {
      char tmp[160]; int tlen = 0, bi = lineStart, lastSp = -1;
      while (bi < nextNL) {
        tmp[tlen++] = docContent[bi]; tmp[tlen] = 0;
        int16_t x1,y1; uint16_t w,h;
        uiTft().getTextBounds(tmp, 0, 0, &x1, &y1, &w, &h);
        if ((int)w > maxWidth) break;
        if (docContent[bi] == ' ') lastSp = bi;
        bi++;
      }
      int endOfLine = (bi < nextNL) ? (lastSp>=0 ? lastSp : bi-1) : nextNL;
      char lineBuf[160]; int l = endOfLine - lineStart;
      if (l > 159) l = 159;
      memcpy(lineBuf, docContent + lineStart, l); lineBuf[l] = 0;
      uiTft().setCursor(curX, curY);
      uiTft().print(lineBuf);
      curY += lineH;
      lineStart = (bi < nextNL) ? (lastSp>=0 ? lastSp+1 : bi) : nextNL;
      if (curY > uiBodyY0() + uiBodyH() - 5) break;
    }
    pStart = nextNL + 1;
  }
  char pageBuf[64];
  if (docPageCount > 1)
    snprintf(pageBuf, sizeof(pageBuf), "Page %d of %d (%dKB)", docCurrentPage+1, docPageCount, (int)strlen(docContent)/1024);
  else
    snprintf(pageBuf, sizeof(pageBuf), "%d Bytes", (int)strlen(docContent));
  uiTft().setFont(nullptr);
  uiTft().setTextColor(uiColorSub());
  uiTft().setCursor(5, uiTft().height()-10); uiTft().print(pageBuf);
  uiTft().setCursor(uiTft().width()-60, uiTft().height()-10); uiTft().print("Long=Home");
}


static void enterDocsScreen() {
  screen = SCR_DOCS;
  viewingDoc = false;
  showDocsMenu();
}

static void clampGalleryViewport() {
  int count = uiSketchGalleryGetCount();
  if (gallerySel < 0) gallerySel = 0;
  if (gallerySel >= count) gallerySel = (count > 0 ? count - 1 : 0);
  if (galleryTop < 0) galleryTop = 0;
  const int rowH = 28;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int maxRows = bodyH / rowH;
  int maxTop = (count > maxRows) ? (count - maxRows) : 0;
  if (galleryTop > maxTop) galleryTop = maxTop;
  if (gallerySel < galleryTop) galleryTop = gallerySel;
  if (gallerySel >= galleryTop + maxRows) galleryTop = gallerySel - (maxRows - 1);
}

static void enterSketchGallery() {
  screen = SCR_SKETCH_GALLERY;
  gallerySel = 0;
  galleryTop = 0;
  uiSketchGalleryDrawFull(gallerySel, galleryTop);
}

static void enterPomodoroScreen() {
  screen = SCR_POMODORO;
  if (pomoRemainMs == 0 && !pomoRunning) {
    pomoRemainMs = pomoIsWork ? pomoWorkMs : pomoBreakMs;
  }
  uiPomodoroDrawFull(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles);
}

static void openSelectedHome() {
  uiFadeOut(100);
  switch (homeSel) {
    case 0: enterTimeScreen(); break;
    case 1: enterWeatherScreen(); break;
    case 2: showNewsChannels(); break;
    case 3: enterQuotesScreen(); break;
    case 4: enterGamesScreen(); break;
    case 5: enterTempPressureScreen(); break;
    case 6: enterPomodoroScreen(); break;
    case 7: enterSketchScreen(); break;
    case 8: enterDocsScreen(); break;
  }
  uiFadeIn(100);
}

static void handleBack() {
  if (screen == SCR_DOCS && viewingDoc) {
    viewingDoc = false;
    showDocsMenu();
    return;
  }

  if (screen == SCR_NEWS_CHANNELS) {
    uiFadeOut(100);
    goHome();
    uiFadeIn(100);
    return;
  }
  if (screen == SCR_NEWS_CATEGORIES) {
    uiFadeOut(100);
    showNewsChannels();
    uiFadeIn(100);
    return;
  }
  if (screen == SCR_NEWS) {
    uiFadeOut(100);
    showNewsCategories();
    uiFadeIn(100);
    return;
  }
  if (screen == SCR_NEWS_STUDY) {
    uiFadeOut(100);
    enterNewsScreen();
    uiFadeIn(100);
    return;
  }

  if (screen == SCR_QUOTES_STUDY) {
    uiFadeOut(100);
    enterQuotesScreen();
    uiFadeIn(100);
    return;
  }

  if (screen == SCR_TIME_STUDY) {
    uiFadeOut(100);
    enterTimeScreen();
    uiFadeIn(100);
    return;
  }

  if (screen == SCR_GAMES && gamesInGame()) {
    gamesExitToMenu();
    showGamesMenu();
    return;
  }
  
  if (screen == SCR_SETTINGS || screen == SCR_FORMAT_OPTIONS || 
      screen == SCR_CALC || screen == SCR_STOPWATCH || screen == SCR_TIMER || 
      screen == SCR_UNITCONV || screen == SCR_SKETCH_GALLERY) {
    uiFadeOut(100);
    uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT);
    screen = SCR_TOOLS;
    uiFadeIn(100);
    return;
  }
  
  if (screen != SCR_HOME && screen != SCR_TITLE) {
    uiFadeOut(100);
    goHome();
    uiFadeIn(100);
  }
}

static void checkNightMode() {
  if (millis() - lastNightCheck < 60000) return; // Check once a minute
  lastNightCheck = millis();
  
  if (!sensorsRtcOk()) return;
  DateTime dt = sensorsNow();
  int hr = dt.hour();
  
  // Auto-Dark Theme between 10PM and 7AM
  bool shouldBeDark = (hr >= 22 || hr < 7);
  if (shouldBeDark && !setThemeDark) {
    setThemeDark = true;
    setBrightness = 1; // Minimum brightness at night
    uiSetTheme(true);
    applyBrightness();
    settingsSave();
    if (screen == SCR_HOME) goHome();
  } else if (!shouldBeDark && setThemeDark) {
    // Revert to light mode during daytime (only if auto-night set it dark)
    // Check stored preference — if user manually set dark, don't override
    // We use a simple heuristic: revert only if brightness is still at minimum (1)
    if (setBrightness <= 1) {
      setThemeDark = false;
      setBrightness = 2;
      uiSetTheme(false);
      applyBrightness();
      settingsSave();
      if (screen == SCR_HOME) goHome();
    }
  }
}

static bool isAlarmSnoozed = false;
static uint32_t snoozeUntilMs = 0;
static bool alarmIsRinging = false;
static uint32_t alarmRingStartMs = 0;

struct AlarmWave { float r; bool active; };
static AlarmWave awaves[3];

static void refreshCurrentScreen() {
    uiFadeOut(100);
    uiClearContent();
    uiTft().setTextSize(1);
    switch(screen) {
        case SCR_HOME: uiHomeDrawFull(homeSel, homeTop); break;
        case SCR_TIME: uiTimeScreenDrawFull(sensorsRtcOk()); break;
        case SCR_TIME_STUDY: uiTimeScreenDrawStudy(sensorsRtcOk()); break;
        case SCR_WEATHER: uiWeatherScreenDrawFull(wxCitySel, wxAll[wxCitySel], wifiOk(), weatherIsBusy()); break;
        case SCR_INFO: {
            char l1[40]; snprintf(l1, sizeof(l1), "Firmware: NAA TV");
            char l2[40]; snprintf(l2, sizeof(l2), "Uptime: %lus", (unsigned long)(millis() / 1000));
            char l3[40]; snprintf(l3, sizeof(l3), "Free heap: %u", (unsigned)ESP.getFreeHeap());
            uiInfoScreenDraw(wifiOk(), l1, l2, l3);
            break;
        }
        case SCR_WIFI: showWifi(); break;
        case SCR_ALARM: uiAlarmsDrawFull(alarmSel, alarmEditMode, alarmEditField, alarmsGet(0), 3); break;
        case SCR_CALC: uiCalcDrawFull(calcExpr, calcResult); break;
        case SCR_TEMP_PRESSURE: {
            float tC = readTempC(); float h = readHum(); float p = readPress();
            uiTempPressDrawFull(sensorsRtcOk(), tC, h, p, setUseF, setUseInHg);
            break;
        }
        case SCR_STOPWATCH: uiStopwatchDrawFull(swAccumMs + (swRunning ? (millis() - swStartMs) : 0), swRunning); break;
        case SCR_TIMER: uiTimerDrawFull(tmRemainMs, tmRunning, tmPresetIdx); break;
        case SCR_POMODORO: uiPomodoroDrawFull(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles); break;
        case SCR_UNITCONV: uiUnitConvDrawFull(ucMode, ucInVal, ucOutVal, (ucMode==0)?"C":"hPa", (ucMode==0)?"F":"inHg"); break;
        case SCR_SETTINGS: showSettings(); break;
        case SCR_DOCS: enterDocsScreen(); break;
        case SCR_SKETCH: uiSketchDrawFull(currentSketchColor); uiSketchRedrawCurrent(); break;
        case SCR_SKETCH_GALLERY: uiSketchGalleryDrawFull(gallerySel, galleryTop); break;
        case SCR_QUOTES: uiQuotesScreenDrawFull(wifiOk(), quotesIsBusy(), quotesGetCached()); break;
        case SCR_QUOTES_STUDY: uiQuotesScreenDrawStudy(quotesGetCached()); break;
        case SCR_NEWS: uiNewsScreenDrawFull(wifiOk(), newsIsBusy(), newsGetCached()); break;
        case SCR_NEWS_STUDY: uiNewsScreenDrawStudy(newsGetCached()); break;
        case SCR_NEWS_CHANNELS: uiNewsChannelsMenuDrawFull(newsChanSel, 0, NEWS_CHAN_ITEMS, NEWS_CHAN_COUNT); break;
        case SCR_NEWS_CATEGORIES: uiNewsCategoriesMenuDrawFull(newsCatSel, 0, NEWS_CAT_ITEMS, NEWS_CAT_COUNT, (newsChanSel==1)?"GLOBAL":"INDIA"); break;
        case SCR_TOOLS: uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT); break;
        case SCR_FORMAT_OPTIONS: uiFormatOptionsDrawFull(formatSel, FORMAT_ITEMS, FORMAT_COUNT); break;
        case SCR_GAMES: 
            if (gamesInGame()) { gamesExitToMenu(); }
            uiGamesMenuDrawFull(gamesSel, gamesTop, GAMES_ITEMS, GAMES_COUNT); 
            break;
        default: goHome(); break;
    }
    uiFadeIn(100);
}

static void startAlarmRing() {
    alarmIsRinging = true;
    alarmRingStartMs = millis();
    uiSetBacklight(true);
    
    uiTft().fillScreen(uiColorBg());
    
    uiTft().setFont(nullptr);
    uiTft().setTextSize(3);
    uiTft().setTextColor(uiColorText());
    uiTft().setCursor(160 - 45, 30);
    uiTft().print("ALARM");
    uiTft().setTextSize(1);
    
    int cx = 160, cy = 140;
    // Bell top ring
    uiTft().fillCircle(cx, cy - 25, 8, uiColorDivider());
    uiTft().fillCircle(cx, cy - 25, 4, uiColorBg());
    // Bell body
    uiTft().fillRoundRect(cx - 15, cy - 20, 30, 30, 8, uiColorAccent());
    uiTft().fillRect(cx - 20, cy + 5, 40, 5, uiColorAccent());
    
    for(int i=0; i<3; i++) awaves[i].active = false;
}

static void stopAlarmRing(bool snooze) {
    alarmIsRinging = false;
    
    refreshCurrentScreen();
    
    if (snooze) {
        isAlarmSnoozed = true;
        snoozeUntilMs = millis() + 3 * 60 * 1000UL;
    } else {
        isAlarmSnoozed = false;
    }
}

static uint32_t lastAWave = 0;
static uint32_t animTickMs = 0;

static void drawAlarmAnimation(uint32_t now) {
    if (now - animTickMs < 20) return;
    animTickMs = now;
    auto& tft = uiTft();
    int cx = 160, cy = 140;
    
    // Flash text
    tft.setFont(nullptr);
    tft.setTextSize(3);
    uint16_t txtColor = ((now / 500) % 2 == 0) ? uiColorAccent() : uiColorText();
    tft.setTextColor(txtColor, uiColorBg());
    tft.setCursor(160 - 45, 30);
    tft.print("ALARM");
    tft.setTextSize(1);
    
    // 1. Erase Clapper Area (safely below base)
    tft.fillRect(cx - 22, cy + 10, 44, 14, uiColorBg());
    
    // 2. Draw Clapper
    int swing = (int)(sin(now / 50.0f) * 12.0f);
    tft.fillCircle(cx + swing, cy + 16, 5, uiColorText());
    
    // 3. Spawner
    if (now - lastAWave > 700) {
        lastAWave = now;
        for(int i=0; i<3; i++) {
            if(!awaves[i].active) {
                awaves[i].active = true;
                awaves[i].r = 30.0f;
                break;
            }
        }
    }
    
    // 4. Update Waves
    for(int i=0; i<3; i++) {
        if(awaves[i].active) {
            // Erase old wave precisely
            tft.drawCircle(cx, cy, (int)awaves[i].r, uiColorBg());
            tft.drawCircle(cx, cy, (int)awaves[i].r - 1, uiColorBg());
            
            awaves[i].r += 1.5f;
            
            if(awaves[i].r > 95.0f) {
                awaves[i].active = false;
            } else {
                // Draw new wave
                tft.drawCircle(cx, cy, (int)awaves[i].r, uiColorDivider());
                tft.drawCircle(cx, cy, (int)awaves[i].r - 1, uiColorDivider());
            }
        }
    }
}

void setup() {
  Serial.begin(115200);
  delay(150);

  // Hardware watchdog: reboot if loop() stalls for > 10 seconds
  // ESP-IDF v5.x uses esp_task_wdt_config_t struct
  {
    esp_task_wdt_config_t wdt_cfg = {
      .timeout_ms    = 10000,
      .idle_core_mask = 0,
      .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(nullptr);
  }

  uiBegin();
  touchBegin();
  netBegin();
  sensorsBegin();
  encoderBegin();

  gamesBegin();
  joyBegin();

  weatherAttachRTC(sensorsRtcPtr(), sensorsRtcOk());

  weatherBegin();
  quotesBegin();
  newsBegin();

  storageBegin();
  alarmsBegin();
  refreshDocFiles();

  storageLoadWeatherAll(wxAll, CITY_COUNT, nullptr);
  settingsLoad();
  
  // Optional safety: if user somehow starts up with an uninitialized dark theme that isn't saved, 
  // ensure it becomes the new persisted default if needed.
  // Actually, setting Default to true in settingsLoad() already did this.
  
  uiSetTheme(setThemeDark);
  uiSetAccentTheme(setAccentIdx);
  uiSetFontSizeSetting(setFontSize);
  applyBrightness();

  startDocsServer();
  goTitle();
}

void loop() {
  esp_task_wdt_reset(); // Feed watchdog — proves loop() is alive
  const uint32_t now = millis();
  
  // High-accuracy encoder poll
  const int d = encoderReadDelta();
  bool eClick = false;
  bool eLong = false;
  encoderPoll(eClick, eLong);

  if (alarmIsRinging) {
      drawAlarmAnimation(now);
      TouchEvent ev;
      bool touched = touchPollEvent(ev);
      if (touched || d != 0 || eClick || eLong) {
          stopAlarmRing(false); // Dismiss alarm entirely
          return;
      }
      if (now - alarmRingStartMs > 60000UL) {
          stopAlarmRing(true); // Snooze for 3 minutes
      }
      return; // Block rest of loop processing
  } else if (sensorsRtcOk()) {
      DateTime dt = sensorsNow();
      if (isAlarmSnoozed && now >= snoozeUntilMs) {
          isAlarmSnoozed = false;
          startAlarmRing();
      } else if (!isAlarmSnoozed && alarmsCheckTrigger(dt.hour(), dt.minute(), dt.second())) {
          startAlarmRing();
      }
  }

  checkNightMode(); // NEW: Scheduled auto-theme check

  // Documents periodic handle
  if (docsServerStarted) {
    docsServer.handleClient();
  }

  sensorsLoop();
  netLoop();

  weatherAttachRTC(sensorsRtcPtr(), sensorsRtcOk());

  weatherLoop();
  quotesLoop();
  newsLoop();

  // Idle Return Logic
  if (displayTimeoutMs != TIMEOUT_NEVER && (now - lastInteractionMs > displayTimeoutMs)) {
    // Check exclusions
    bool exempt = (swRunning || tmRunning || pomoRunning || (screen == SCR_GAMES && gamesInGame()) || (screen == SCR_TIME_STUDY) || (screen == SCR_QUOTES_STUDY) || (screen == SCR_NEWS_STUDY) || (screen == SCR_DOCS && viewingDoc) || (screen == SCR_TEMP_PRESSURE));
    if (!exempt && screen != SCR_TITLE) {
      goTitle();
    }
  }

  // TITLE animation update
  if (screen == SCR_TITLE) {
    uiTitleScreenUpdate();
    if (!titleReadyToExit && (now - titleShownAt) > 1200) titleReadyToExit = true;
    
    // Tap to resume last screen if it wasn't the title screen
    TouchEvent ev;
    if (touchPollEvent(ev)) {
      if (ev.type == TE_TAP && titleReadyToExit) {
        lastInteractionMs = now;
        uiFadeOut(150);
        // Resume last screen
        screen = lastActiveScreen;
        if (screen == SCR_HOME) uiHomeDrawFull(homeSel, homeTop);
        else if (screen == SCR_TIME) enterTimeScreen();
        else if (screen == SCR_TIME_STUDY) enterStudyMode();
        else if (screen == SCR_WEATHER) enterWeatherScreen();
        else if (screen == SCR_NEWS) enterNewsScreen();
        else if (screen == SCR_NEWS_CHANNELS) showNewsChannels();
        else if (screen == SCR_NEWS_CATEGORIES) {
          const char* chanName = (newsChanSel == 1) ? "GLOBAL" : "INDIA";
          uiNewsCategoriesMenuDrawFull(newsCatSel, 0, NEWS_CAT_ITEMS, NEWS_CAT_COUNT, chanName);
        }
        else if (screen == SCR_NEWS_STUDY) enterNewsStudyScreen();
        else if (screen == SCR_QUOTES) enterQuotesScreen();
        else if (screen == SCR_QUOTES_STUDY) enterQuotesStudyMode();
        else if (screen == SCR_GAMES) enterGamesScreen();
        else if (screen == SCR_TOOLS) uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT);
        else if (screen == SCR_DOCS) enterDocsScreen();
        else if (screen == SCR_SETTINGS) showSettings();
        else if (screen == SCR_POMODORO) enterPomodoroScreen();
        else if (screen == SCR_TEMP_PRESSURE) uiTempPressDrawFull(sensorsRtcOk(), readTempC(), readHum(), readPress(), setUseF, setUseInHg);
        else if (screen == SCR_STOPWATCH) uiStopwatchDrawFull(swAccumMs, swRunning);
        else if (screen == SCR_TIMER) uiTimerDrawFull(tmRemainMs, tmRunning, tmPresetIdx);
        else if (screen == SCR_CALC) {
          // Resume calculator with preserved state (don't reset calculations)
          uiCalcDrawFull(calcExpr, calcResult);
        }
        else if (screen == SCR_SKETCH) {
          uiSketchDrawFull(currentSketchColor); // Draw header/footer
          uiSketchRedrawCurrent(); // Keep the drawing as it is
        }
        else goHome(); // Fallback
        uiFadeIn(150);
      }
    }
  }

  // Weather async consume
  {
    bool ok; WeatherCache out; int idx;
    if (weatherConsumeResult(ok, out, idx)) {
      uiWeatherScreenSetBusy(false);
      uiSetMascotExpression(ok ? EXPR_SMILE : EXPR_SAD);
      if (ok && idx >= 0 && idx < CITY_COUNT) {
        wxAll[idx] = out;
        storageSaveWeatherOne(idx, out);
        if (screen == SCR_WEATHER && idx == wxCitySel) {
          // Partial redraw — no full clear, no flicker
          uiWeatherScreenUpdateData(wxCitySel, wxAll[wxCitySel], wifiOk());
        }
      } else {
        // Fetch failed — full redraw to show '--.-' state clearly
        if (screen == SCR_WEATHER) {
          uiWeatherScreenDrawFull(wxCitySel, wxAll[wxCitySel], wifiOk(), false);
        }
      }
    }
  }

  // Alarm Trigger Logic
  static uint32_t lastAlarmCheck = 0;
  if (now - lastAlarmCheck >= 1000) {
    lastAlarmCheck = now;
    if (sensorsRtcOk()) {
      DateTime dt = sensorsNow();
      alarmsCheckTrigger(dt.hour(), dt.minute(), dt.second());
    }
  }

  if (alarmsIsTriggered()) {
    // Visual flash
    bool flash = (now / 500) % 2 == 0;
    uiTft().invertDisplay(flash);
    
    // Stop alarm on any interaction
    TouchEvent ev;
    if (touchPollEvent(ev) || eClick || eLong || d != 0) {
      alarmsResetTriggered();
      uiTft().invertDisplay(false);
      lastInteractionMs = now;
    }
    return; // Block normal operation while alarming
  }

  // Quotes async consume
  {
    bool ok; QuoteData out;
    if (quotesConsumeResult(ok, out)) {
      uiQuotesScreenSetBusy(false);
      if (screen == SCR_QUOTES) uiQuotesScreenUpdate(wifiOk(), quotesGetCached());
      else if (screen == SCR_QUOTES_STUDY) uiQuotesScreenUpdateStudy(quotesGetCached());
    }
  }

  // News async consume
  {
    bool ok; static NewsData out; // Static to prevent stack overflow (~11KB)
    if (newsConsumeResult(ok, out)) {
      uiNewsScreenSetBusy(false);
      if (screen == SCR_NEWS) uiNewsScreenUpdate(wifiOk(), newsGetCached());
      else if (screen == SCR_NEWS_STUDY) uiNewsScreenUpdateStudy(newsGetCached(), newsStudyScrollOffset);
    }
  }

  // Games periodic loop (non-blocking)
  if (screen == SCR_GAMES) {
    gamesLoop(now);
  }

  if (screen == SCR_DOCS && !viewingDoc) {
    static uint32_t lastRefreshCheck = 0;
    static int lastDocFileCount = -1;
    if (now - lastRefreshCheck > 4000) {
      lastRefreshCheck = now;
      refreshDocFiles();
      if (docFileCount != lastDocFileCount) {
        lastDocFileCount = docFileCount;
        showDocsMenu();
      }
    }
  }

  // Temp & Pressure: on-demand BMP280 — only reads while the screen is open
  static uint32_t tpLast = 0;
  if (screen == SCR_TEMP_PRESSURE) {
    // Wait 2 s after wake so sensor equilibrates to actual air (eliminates
    // residual heat from the ESP32 and the BMP280's own startup transient)
    uint32_t warmupMs = 2000UL;
    bool warmedUp = (now - tpWakeMs >= warmupMs);

    if (!warmedUp) {
      // Still warming — show countdown in the status area
      if (now - tpLast >= 250) {
        tpLast = now;
        uint32_t remainSec = (warmupMs - (now - tpWakeMs) + 999) / 1000;
        uiTempPressSetStatus("Warming up... %ds", (int)remainSec);
      }
    } else {
      // Poll every 2 s — slow polling prevents the sensor heating itself
      if (now - tpLast >= 2000) {
        tpLast = now;
        float tC = readTempC();
        float h  = readHum();
        float p  = readPress();
        uiTempPressUpdate(sensorsRtcOk(), tC, h, p, setUseF, setUseInHg);
      }
    }
  } else {
    // Screen changed away from Temp — sleep the sensor if still awake
    if (sensorsIsBMPAwake()) sensorsIdleBMP();
  }

  // Stopwatch periodic refresh
  if (screen == SCR_STOPWATCH && swRunning) {
    static uint32_t swLast = 0;
    if (now - swLast >= 50) {
      swLast = now;
      uiStopwatchUpdateTime(swAccumMs + (millis() - swStartMs), true);
    }
  }

  // Timer periodic countdown
  if (screen == SCR_TIMER && tmRunning) {
    uint32_t m = millis();
    if (m - tmLastTick >= 50) {
      uint32_t delta = m - tmLastTick;
      tmLastTick = m;
      if (tmRemainMs > delta) tmRemainMs -= delta;
      else { tmRemainMs = 0; tmRunning = false; }
      uiTimerUpdateTime(tmRemainMs, tmRunning, tmPresetIdx);
    }
  }

  // Pomodoro periodic countdown
  if (pomoRunning) {
    uint32_t m = millis();
    if (m - pomoLastTick >= 100) {
      uint32_t delta = m - pomoLastTick;
      pomoLastTick = m;
      if (pomoRemainMs > delta) pomoRemainMs -= delta;
      else {
        // Cycle switch
        pomoIsWork = !pomoIsWork;
        if (pomoIsWork) {
          pomoCycles++; // Completed a break, back to work
          uiSetMascotExpression(EXPR_SMILE);
          uiSetNotification(false); // Clear notification when back to work
        } else {
          uiSetMascotExpression(EXPR_WINK); // Break time!
          uiSetNotification(true); // Notification badge when work is done
        }
        pomoRemainMs = pomoIsWork ? pomoWorkMs : pomoBreakMs;
        // Optional: you could add a beep here if buzzer exists
      }
      if (screen == SCR_POMODORO) {
        uiPomodoroUpdate(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles);
      }
    }
  }

  // Touch
  TouchEvent ev;
  if (touchPollEvent(ev)) {
    lastInteractionMs = now; // Reset idle timer on any touch

    // Quick Settings Overlay handling
    if (qsOpen) {
      if (ev.type == TE_TAP) {
        if (ev.y > 130) { // Tap below QS area to close
          qsOpen = false;
          // Redraw current screen to clear overlay
          switch(screen) {
            case SCR_HOME: goHome(); break;
            case SCR_TIME: enterTimeScreen(); break;
            case SCR_WEATHER: enterWeatherScreen(); break;
            case SCR_NEWS: enterNewsScreen(); break;
            case SCR_QUOTES: enterQuotesScreen(); break;
            case SCR_GAMES: enterGamesScreen(); break;
            case SCR_TEMP_PRESSURE: enterTempPressureScreen(); break;
            case SCR_STOPWATCH: showStopwatch(); break;
            case SCR_TIMER: showTimer(); break;
            case SCR_POMODORO: enterPomodoroScreen(); break;
            case SCR_SETTINGS: showSettings(); break;
            default: goHome(); break;
          }
          return;
        }
        // Basic toggle logic inside QS
        if (ev.y > 40 && ev.y < 70) { // Brightness row
          setBrightness = (setBrightness + 1) % 5;
          applyBrightness();
          settingsSave();
          uiDrawQuickSettings(setBrightness, setThemeDark, wifiOk());
          return;
        }
        if (ev.y > 70 && ev.y < 100) { // Theme row
          setThemeDark = !setThemeDark;
          uiSetTheme(setThemeDark);
          settingsSave();
          uiDrawQuickSettings(setBrightness, setThemeDark, wifiOk());
          return;
        }
        if (ev.y > 100 && ev.y < 130) { // WiFi (just a toggle for demo, or info)
          // WiFi toggle logic if needed
          uiDrawQuickSettings(setBrightness, setThemeDark, wifiOk());
          return;
        }
      }
      return; // Block other touches when QS is open
    }

    // Swipe Down to open Quick Settings (Header section only)
    if (ev.type == TE_SWIPE_DOWN && screen != SCR_TITLE && screen != SCR_SKETCH) {
      if ((ev.y - ev.dy) < UI_HEADER_H) {
        qsOpen = true;
        uiDrawQuickSettings(setBrightness, setThemeDark, wifiOk());
        return;
      }
    }

    // Long-press behavior: Back/Home navigation
    if (ev.type == TE_LONGPRESS) {
      if (screen == SCR_SKETCH) return; // Disable long-press in Sketch Pad
      handleBack();
      return;
    }

    // Double-tap Study Mode (Butter-Smooth Activation)
    if (ev.type == TE_DBL_TAP) {
      if (screen == SCR_TIME) {
        uiFadeOut(100); enterStudyMode(); uiFadeIn(100); return;
      } else if (screen == SCR_QUOTES) {
        uiFadeOut(100); enterQuotesStudyMode(); uiFadeIn(100); return;
      } else if (screen == SCR_NEWS) {
        uiFadeOut(100); enterNewsStudyMode(); uiFadeIn(100); return;
      }
    }

    // Gesture control (Swipes)
    if (ev.type == TE_SWIPE_RIGHT || ev.type == TE_SWIPE_LEFT || ev.type == TE_SWIPE_UP || ev.type == TE_SWIPE_DOWN) {
      if (screen == SCR_SKETCH) return; // Disable swipes in Sketch Pad
    }

    // SKETCH DRAWING LOGIC
    if (screen == SCR_SKETCH) {
      if (ev.type == TE_DOWN) {
        if (ev.y >= UI_HEADER_H && ev.y < (TFT_H - UI_FOOTER_H)) {
          lastSketchX = ev.x;
          lastSketchY = ev.y;
          prevSketchX = ev.x; // Set initial points same
          prevSketchY = ev.y;
          // Draw a single point on DOWN so taps are visible as dots
          uiSketchDrawLine(ev.x, ev.y, ev.x, ev.y, currentSketchColor, ev.raw.z);
        }
      }
      else if (ev.type == TE_DRAG) {
        if (ev.y >= UI_HEADER_H && ev.y < (TFT_H - UI_FOOTER_H)) {
          if (lastSketchX != -1 && lastSketchY != -1) {
            // Raw segment drawing for maximum precision in symbols (division, minus, etc.)
            uiSketchDrawLine(lastSketchX, lastSketchY, ev.x, ev.y, currentSketchColor, ev.raw.z);
          }
          lastSketchX = ev.x;
          lastSketchY = ev.y;
        } else {
          lastSketchX = -1; lastSketchY = -1;
        }
      }
      else if (ev.type == TE_UP) {
        lastSketchX = -1; lastSketchY = -1;
        prevSketchX = -1; prevSketchY = -1;
      }
    }

    // Title tap -> Restore last screen
    if (screen == SCR_TITLE) {
      if ((ev.type == TE_TAP || eClick) && titleReadyToExit) {
        uiFadeOut(100);
        if (lastActiveScreen == SCR_TITLE) goHome();
        else {
          screen = lastActiveScreen;
          // Re-draw based on screen type
          switch (screen) {
            case SCR_HOME: goHome(); break;
            case SCR_TIME: enterTimeScreen(); break;
            case SCR_TIME_STUDY: enterStudyMode(); break;
            case SCR_WEATHER: enterWeatherScreen(); break;
            case SCR_NEWS: enterNewsScreen(); break;
            case SCR_QUOTES: enterQuotesScreen(); break;
            case SCR_QUOTES_STUDY: enterQuotesStudyMode(); break;
            case SCR_NEWS_STUDY: enterNewsStudyMode(); break;
            case SCR_SKETCH: 
              screen = SCR_SKETCH;
              uiSketchDrawFull(currentSketchColor); // Header/Footer
              uiSketchRedrawCurrent(); // Redraw lines from buffer
              break;
            case SCR_GAMES: enterGamesScreen(); break;
            case SCR_TEMP_PRESSURE: enterTempPressureScreen(); break;
            case SCR_STOPWATCH: showStopwatch(); break;
            case SCR_TIMER: showTimer(); break;
            case SCR_POMODORO: enterPomodoroScreen(); break;
            case SCR_UNITCONV: showUnitConv(); break;
            case SCR_CALC: showCalc(); break;
            case SCR_TOOLS: showTools(); break;
            case SCR_SETTINGS: showSettings(); break;
            case SCR_INFO: showInfo(); break;
            case SCR_WIFI: showWifi(); break;
            case SCR_DOCS: enterDocsScreen(); break;
            case SCR_SKETCH_GALLERY: uiSketchGalleryDrawFull(gallerySel, galleryTop); break;
            case SCR_NEWS_CHANNELS: showNewsChannels(); break;
            case SCR_NEWS_CATEGORIES: showNewsCategories(); break;
            case SCR_FORMAT_OPTIONS: showFormatOptions(); break;
            default: goHome(); break;
          }
        }
        uiFadeIn(100);
      }
      return;
    }

    // swipe right = back (global)
    if (ev.type == TE_SWIPE_RIGHT) { handleBack(); return; }

    // swipe left behaviors
    if (ev.type == TE_SWIPE_LEFT) {
      if (screen == SCR_WEATHER) { weatherNextCity(); return; }
      if (screen == SCR_NEWS)    { newsNext(); uiNewsScreenUpdate(wifiOk(), newsGetCached()); return; }
      if (screen == SCR_NEWS_STUDY) { 
        newsStudyScrollOffset = 0;
        newsNext(); 
        uiNewsScreenUpdateStudy(newsGetCached(), 0); 
        return; 
      }
    }

    // vertical swipe behaviors for scrolling
    if (ev.type == TE_SWIPE_UP || ev.type == TE_SWIPE_DOWN) {
      const int d = (ev.type == TE_SWIPE_UP) ? 1 : -1;
      if (screen == SCR_HOME) {
        const int prevSel = homeSel;
        const int prevTop = homeTop;
        homeSel += d;
        clampHomeViewport();
        homeRedrawSelectionOnly(prevSel, prevTop);
        return;
      }
      if (screen == SCR_TOOLS) {
        int prevSel = toolsSel;
        toolsSel += d;
        clampToolsViewport();
        if (prevSel != toolsSel) uiToolsMenuDrawFull(toolsSel, toolsTop, TOOLS_ITEMS, TOOLS_COUNT);
        return;
      }
      if (screen == SCR_GAMES && !gamesInGame()) {
        int prevSel = gamesSel;
        gamesSel += d;
        clampGamesViewport();
        if (prevSel != gamesSel) uiGamesMenuDrawFull(gamesSel, gamesTop, GAMES_ITEMS, GAMES_COUNT);
        return;
      }
      if (screen == SCR_SETTINGS) {
        int prevSel = settingsSel;
        settingsSel += d;
        clampSettingsViewport();
        if (prevSel != settingsSel) showSettings();
        return;
      }
      if (screen == SCR_SKETCH_GALLERY) {
        int prevSel = gallerySel;
        gallerySel += d;
        clampGalleryViewport();
        if (prevSel != gallerySel) uiSketchGalleryDrawFull(gallerySel, galleryTop);
        return;
      }
      if (screen == SCR_DOCS && !viewingDoc) {
        int prevSel = docsSel;
        int prevTop = docsTop;
        docsSel += d;
        // Use fixed-size docFileList instead of heap-allocating getDocFiles()
        refreshDocFiles();
        int count = docFileCount;
        if (docsSel < 0) docsSel = 0;
        if (docsSel >= count) docsSel = (count > 0 ? count - 1 : 0);
        
        const int rowH = 28;
        const int topH = 5;
        const int bodyH = uiBodyH() - 34 - topH;
        const int maxRows = bodyH / rowH;
        int maxTop = (count > maxRows) ? (count - maxRows) : 0;
        if (docsTop > maxTop) docsTop = maxTop;
        if (docsSel < docsTop) docsTop = docsSel;
        if (docsSel >= docsTop + maxRows) docsTop = docsSel - (maxRows - 1);
        
        if (prevSel != docsSel) docsRedrawSelectionOnly(prevSel, prevTop);
        return;
      }
      if (screen == SCR_NEWS_STUDY) {
        // Swipe UP = scroll description DOWN (read more), Swipe DOWN = scroll up
        newsStudyScrollOffset += d * 3;
        if (newsStudyScrollOffset < 0) newsStudyScrollOffset = 0;
        uiNewsScreenUpdateStudy(newsGetCached(), newsStudyScrollOffset);
        return;
      }
    }

    if (ev.type == TE_TAP) {
      if (screen == SCR_DOCS) {
        if (viewingDoc) {
          // Tap bottom 1/3 to page down, top 1/3 to page up
          if (ev.y > (TFT_H * 2 / 3)) {
            if (docCurrentPage + 1 < docPageCount) {
              docCurrentPage++;
              saveDocProgress(currentDocName, docCurrentPage);
              drawDocContent();
            }
          }
          else if (ev.y < (TFT_H / 3)) { 
            if (docCurrentPage > 0) { 
              docCurrentPage--; 
              saveDocProgress(currentDocName, docCurrentPage);
              drawDocContent(); 
            }
          }
        } else {
          // Footer check (DELETE button on Right)
          if (ev.y > TFT_H - 34) {
            if (ev.x > TFT_W - 80) {
              refreshDocFiles();
              if (docsSel >= 0 && docsSel < docFileCount) {
                char toDelete[68];
                const char* fn = docFileList[docsSel];
                snprintf(toDelete, sizeof(toDelete), "%s%s", fn[0]=='/' ? "" : "/", fn);
                LittleFS.remove(toDelete);
                showDocsMenu();
              }
              return;
            }
          }

          // Check which file was tapped in the list
          const int rowH = 28;
          int relativeY = ev.y - (uiBodyY0() + 5);
          if (relativeY >= 0 && ev.y < TFT_H - 34) {
            int tappedIdx = docsTop + (relativeY / rowH);
            if (tappedIdx >= 0 && tappedIdx < docFileCount) {
              docsSel = tappedIdx;
              openDoc(docFileList[docsSel]);
            }
          }
        }
        return;
      }

      // Mascot Nose Easter Egg (Mascot is in Header at x=4, y=4, size=22)
      if (ev.y < UI_HEADER_H && ev.x < 40) {
        FaceExpression eggs[] = { EXPR_WINK, EXPR_BUSY, EXPR_LOOK_LEFT, EXPR_SMILE };
        uiSetMascotExpression(eggs[esp_random() % 4]);
        // Visual feedback by redrawing ONLY the mascot area
        uiRedrawHeaderMascot(); 
        return;
      }

      if (screen == SCR_TIME_STUDY) return;
      if (screen == SCR_QUOTES_STUDY) return;

      if (screen == SCR_NEWS_STUDY && ev.type == TE_TAP) {
        // Tap = advance to next article (reset scroll)
        newsStudyScrollOffset = 0;
        newsNext();
        uiNewsScreenUpdateStudy(newsGetCached(), 0);
        return;
      }

      if (screen == SCR_HOME) {
        int f = homeFooterIndex(ev.x, ev.y);
        if (f == 1) { showInfo(); return; }
        if (f == 2) { showWifi(); return; }
        if (f == 3) { showTools(); return; }

        if (ev.y >= HOME_LIST_Y && ev.y < (HOME_LIST_Y + HOME_LIST_H)) {
          const int row = (ev.y - HOME_LIST_Y) / HOME_ROW_H;
          const int idx = homeTop + row;
          if (idx >= 0 && idx < HOME_COUNT) { homeSel = idx; openSelectedHome(); return; }
        }
      }

      else if (screen == SCR_TIME && ev.type == TE_TAP) {
        // Double-tap is now the priority; single-tap toggle removed for "butter-smooth" feel
        // lastTimeDraw = 0;
      }

      else if (screen == SCR_WEATHER && ev.type == TE_TAP) {
        if (inFooter(ev.y)) {
          if (inRefreshBtnWx(ev.x, ev.y)) { weatherRequestRefresh(); return; }
          if (inCityBtnWx(ev.x, ev.y)) { weatherNextCity(); return; }
        }
      }

      else if (screen == SCR_QUOTES && ev.type == TE_TAP) {
        if (inFooter(ev.y) && inQuotesNewBtn(ev.x, ev.y)) {
          if (wifiOk() && !quotesIsBusy()) { 
            uiSetMascotExpression(EXPR_BUSY);
            quotesRequestFetch(); 
            uiQuotesScreenSetBusy(true); 
          }
          return;
        }
      }

      else if (screen == SCR_NEWS && ev.type == TE_TAP) {
        if (inFooter(ev.y) && inNewsRefreshBtn(ev.x, ev.y)) {
          if (wifiOk() && !newsIsBusy()) { 
            uiSetMascotExpression(EXPR_BUSY);
            newsRequestFetch(); 
            uiNewsScreenSetBusy(true); 
          }
          return;
        }

        // READ MORE pill hit-test (same coordinates as drawn in drawNewsBody)
        {
          const NewsData& rn = newsGetCached();
          if (rn.count > 0 && rn.desc[rn.index][0]) {
            const int boxX = 10;
            const int boxY = UI_HEADER_H + 36;
            const int boxW = TFT_W - 20;
            const int boxH = (TFT_H - UI_FOOTER_H - 10) - boxY;
            const int pillW = 108, pillH = 24;
            const int pillX = boxX + boxW - pillW - 3;
            const int pillY = boxY + boxH - pillH - 3;
            if (ev.x >= pillX && ev.x <= pillX + pillW &&
                ev.y >= pillY && ev.y <= pillY + pillH) {
              enterNewsStudyScreen();
              return;
            }
          }
        }
        // Single tap outside pill does nothing (double-tap = study mode via other path)
      }

      else if (screen == SCR_NEWS_CHANNELS) {
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int tileH = (bh / 2) - 10;
        
        for (int i = 0; i < 2; i++) {
          int yStart = by0 + 5 + i * (tileH + 10);
          if (ev.y >= yStart && ev.y <= yStart + tileH) {
            uiFadeOut(100);
            newsChanSel = i;
            showNewsCategories();
            uiFadeIn(100);
            return;
          }
        }
      }

      else if (screen == SCR_NEWS_CATEGORIES) {
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int cols = 2;
        const int rows = 4;
        const int tileW = (TFT_W / cols) - 10;
        const int tileH = (bh / rows) - 8;

        for (int i = 0; i < NEWS_CAT_COUNT; i++) {
          int r = i / cols;
          int c = i % cols;
          int xStart = 5 + c * (tileW + 10);
          int yStart = by0 + 4 + r * (tileH + 8);
          
          if (ev.x >= xStart && ev.x <= xStart + tileW &&
              ev.y >= yStart && ev.y <= yStart + tileH) {
            uiFadeOut(100);
            newsCatSel = i;
            enterNewsScreen();
            uiFadeIn(100);
            return;
          }
        }
      }

      else if (screen == SCR_NEWS_STUDY) {
        // Tap anywhere in study = advance to next article (reset scroll)
        newsStudyScrollOffset = 0;
        newsNext();
        uiNewsScreenUpdateStudy(newsGetCached(), 0);
      }

      else if (screen == SCR_GAMES) {
        if (!gamesInGame()) {
          const int by0 = uiBodyY0();
          const int bh = uiBodyH();
          const int rowH = 28;
          if (ev.y >= by0 && ev.y < by0 + bh) {
            int visIdx = (ev.y - by0) / rowH;
            int idx = gamesTop + visIdx;
            if (idx >= 0 && idx < GAMES_COUNT) {
              gamesSel = idx;
              uiGamesMenuDrawFull(gamesSel, gamesTop, GAMES_ITEMS, GAMES_COUNT);
              delay(100); // Visual feedback
              gamesStartByIndex(gamesSel);
              return;
            }
          }
        } else {
          gamesOnTouch(ev);
        }
      }

      else if (screen == SCR_TOOLS) {
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int rowH = 28;
        const int rows = bh / rowH;
        if (ev.y >= by0 && ev.y < by0 + bh) {
          int visIdx = (ev.y - by0) / rowH;
          int idx = toolsTop + visIdx;
          if (idx >= 0 && idx < TOOLS_COUNT) {
            toolsSel = idx;
            const char* it = TOOLS_ITEMS[toolsSel];
            if (strcmp(it, "ALARM CLOCK") == 0) { showAlarms(); return; }
            if (strcmp(it, "CALCULATOR") == 0) { calcReset(); showCalc(); return; }
            if (strcmp(it, "STOPWATCH") == 0) { showStopwatch(); return; }
            if (strcmp(it, "TIMER") == 0) { showTimer(); return; }
            if (strcmp(it, "UNIT CONVERTER") == 0) { showUnitConv(); return; }
            if (strcmp(it, "SKETCHES") == 0) { enterSketchGallery(); return; }
            if (strcmp(it, "SETTINGS") == 0) { showSettings(); return; }
            if (strcmp(it, "FORMAT OPTIONS") == 0) { showFormatOptions(); return; }
            if (strcmp(it, "ABOUT") == 0) { uiShowScreenTitle("ABOUT"); uiShowPlaceholderBody("NAA TV Gadget", "ESP32-S3 multi-tool"); return; }
          }
        }
      }

      else if (screen == SCR_FORMAT_OPTIONS) {
        const int bodyY = UI_HEADER_H + 10;
        const int rowH = 34;
        if (ev.y >= bodyY && ev.y < bodyY + FORMAT_COUNT * rowH) {
          int idx = (ev.y - bodyY) / rowH;
          if (idx >= 0 && idx < FORMAT_COUNT) {
            formatSel = idx;
            uiFormatOptionsRedrawSelection(formatSel, FORMAT_ITEMS, FORMAT_COUNT);
            delay(150);
            granularFormat(formatSel);
            return;
          }
        }
      }

      else if (screen == SCR_ALARM && ev.type == TE_TAP) {
        if (!alarmEditMode) {
          const int by0 = UI_HEADER_H + 10;
          const int rowH = 46;
          if (ev.y >= by0 && ev.y < by0 + 3 * rowH) {
            int idx = (ev.y - by0) / rowH;
            if (idx >= 0 && idx < 3) {
              alarmSel = idx;
              alarmEditMode = true;
              alarmEditField = 0;
              showAlarms();
            }
          }
        }
      }

      else if (screen == SCR_CALC) {
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int gridY = by0 + 80;
        const int cellW = TFT_W / 4;
        const int availH = bh - 80;
        const int cellH = availH / 5; // ENHANCED: Changed from 4 to 5 rows
        if (ev.y >= gridY) {
          int r = (ev.y - gridY) / cellH;
          int c = ev.x / cellW;
          if (r >= 0 && r < 5 && c >= 0 && c < 4) { // ENHANCED: Changed from 4 to 5 rows
            const char* tok = CALC_KEYS[r][c];
            calcHandleToken(tok);
          }
        }
      }

      else if (screen == SCR_STOPWATCH) {
        if (inLeftFooterBtn(ev.x, ev.y)) {
          if (!swRunning) { swRunning = true; swStartMs = millis(); }
          else { swRunning = false; swAccumMs += (millis() - swStartMs); }
          showStopwatch();
          return;
        }
        if (inRightFooterBtn(ev.x, ev.y)) {
          swRunning = false; swAccumMs = 0; swStartMs = 0;
          showStopwatch();
          return;
        }
      }

      else if (screen == SCR_TIMER) {
        // Body taps cycle preset
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        if (ev.y >= by0 && ev.y < by0 + bh - 40) {
          tmPresetIdx = (tmPresetIdx + 1) % 3;
          timerApplyPreset();
          showTimer();
          return;
        }
        if (inLeftFooterBtn(ev.x, ev.y)) {
          if (!tmRunning) { tmRunning = true; tmLastTick = millis(); }
          else { tmRunning = false; }
          showTimer();
          return;
        }
        if (inRightFooterBtn(ev.x, ev.y)) {
          tmRunning = false; timerApplyPreset();
          showTimer();
          return;
        }
      }

      else if (screen == SCR_POMODORO) {
        if (inLeftFooterBtn(ev.x, ev.y)) {
          if (!pomoRunning) { 
            pomoRunning = true; 
            pomoLastTick = millis(); 
            if (pomoRemainMs == 0) pomoRemainMs = pomoIsWork ? pomoWorkMs : pomoBreakMs;
          }
          else pomoRunning = false;
          uiPomodoroDrawFull(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles);
          return;
        }
        if (inRightFooterBtn(ev.x, ev.y)) {
          pomoRunning = false;
          pomoRemainMs = pomoIsWork ? pomoWorkMs : pomoBreakMs;
          uiPomodoroDrawFull(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles);
          return;
        }
        // Body tap = toggle Work/Break manually
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        if (ev.y >= by0 && ev.y < by0 + bh - 40) {
          pomoIsWork = !pomoIsWork;
          pomoRemainMs = pomoIsWork ? pomoWorkMs : pomoBreakMs;
          uiPomodoroDrawFull(pomoRemainMs, pomoRunning, pomoIsWork, pomoCycles);
          return;
        }
      }

      else if (screen == SCR_SKETCH_GALLERY && ev.type == TE_TAP) {
        // Footer check (DELETE button)
        if (ev.y > TFT_H - 38) { // Larger hit area for the footer
          if (ev.x > 230) { // DELETE button area (320 - 75 - 8 = 237)
            int count = uiSketchGalleryGetCount();
            if (gallerySel >= 0 && gallerySel < count) {
              uiSketchDelete(gallerySel);
              // Auto-clamp viewport after delete
              int newCount = uiSketchGalleryGetCount();
              if (gallerySel >= newCount) gallerySel = (newCount > 0) ? newCount - 1 : 0;
              uiSketchGalleryDrawFull(gallerySel, galleryTop);
            }
            return;
          }
        }
        
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int rowH = 28;
        if (ev.y >= by0 && ev.y < by0 + bh) {
          int visIdx = (ev.y - by0) / rowH;
          int idx = galleryTop + visIdx;
          int count = uiSketchGalleryGetCount();
          if (idx >= 0 && idx < count) {
            gallerySel = idx;
            uiSketchGalleryDrawFull(gallerySel, galleryTop);
            delay(100);
            uiSketchView(gallerySel, gallerySel, galleryTop);
            return;
          }
        }
      }

      else if (screen == SCR_UNITCONV) {
        // Tap body toggles mode
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        if (ev.y >= by0 && ev.y < by0 + bh - 40) {
          ucMode = (ucMode == 0) ? 1 : 0;
          unitConvRecalc();
          showUnitConv();
          return;
        }
        if (inUcMinus(ev.x, ev.y)) { ucInVal -= (ucMode == 0 ? 0.5f : 10.0f); unitConvRecalc(); showUnitConv(); return; }
        if (inUcPlus(ev.x, ev.y))  { ucInVal += (ucMode == 0 ? 0.5f : 10.0f); unitConvRecalc(); showUnitConv(); return; }
      }

      else if (screen == SCR_SKETCH) {
        if (inFooter(ev.y)) {
          // EXIT button (6 -> 44)
          if (ev.x >= 6 && ev.x <= 44) {
            goHome();
            return;
          }
          // SAVE button (48 -> 86)
          if (ev.x >= 48 && ev.x <= 86) {
            uiSketchSave();
            return;
          }
          // CLR button (90 -> 124)
          if (ev.x >= 90 && ev.x <= 124) {
            uiSketchClear();
            uiSketchDrawFull(currentSketchColor);
            return;
          }
          // Color dots + Eraser: startX=150, dotW=21
          const uint16_t colors[] = { ILI9341_WHITE, ILI9341_BLACK, ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE, ILI9341_YELLOW, ILI9341_CYAN, 0x0001 };
          const int startX = 320 - (8 * 21) - 2; // 150
          if (ev.x >= startX) {
            int dotIdx = (ev.x - startX) / 21;
            if (dotIdx >= 0 && dotIdx < 8) {
              currentSketchColor = colors[dotIdx];
              uiSketchDrawFooter(currentSketchColor);
            }
          }
        }
      }

      else if (screen == SCR_SETTINGS) {
        const int by0 = uiBodyY0();
        const int bh = uiBodyH();
        const int rowH = 28;
        if (ev.y >= by0 && ev.y < by0 + bh) {
          int visIdx = (ev.y - by0) / rowH;
          int idx = settingsTop + visIdx;
          if (idx >= 0 && idx < 10) {
            settingsSel = idx;
            if (settingsSel == 0) { setBrightness = (setBrightness + 1) % 5; applyBrightness(); settingsSave(); }
            else if (settingsSel == 1) { setThemeDark = !setThemeDark; settingsSave(); uiSetTheme(setThemeDark); }
            else if (settingsSel == 2) { setAccentIdx = (setAccentIdx + 1) % 4; uiSetAccentTheme(setAccentIdx); settingsSave(); }
            else if (settingsSel == 3) { setFontSize = (setFontSize + 1) % 3; settingsSave(); uiSetFontSizeSetting(setFontSize); }
            else if (settingsSel == 4) { setUseF = !setUseF; settingsSave(); }
            else if (settingsSel == 5) { setUseInHg = !setUseInHg; settingsSave(); }
            else if (settingsSel == 6) { setTimeoutIdx = (setTimeoutIdx + 1) % 6; displayTimeoutMs = TIMEOUT_VALS[setTimeoutIdx]; settingsSave(); }
            else if (settingsSel == 7) { setPomoWorkMin = (setPomoWorkMin % 60) + 5; pomoWorkMs = setPomoWorkMin * 60 * 1000UL; settingsSave(); }
            else if (settingsSel == 8) { setPomoBreakMin = (setPomoBreakMin % 30) + 1; pomoBreakMs = setPomoBreakMin * 60 * 1000UL; settingsSave(); }
            else if (settingsSel == 9) { 
              setTempOffset += 0.5f; 
              if (setTempOffset > 5.0f) setTempOffset = -5.0f; 
              settingsSave(); 
            }
            showSettings();
          }
        }
      }

      else if (screen == SCR_TEMP_PRESSURE) {
        if (inFooter(ev.y)) {
          if (inTempUnitsBtn(ev.x, ev.y)) {
            setUseF = !setUseF;
            settingsSave();
            // Redraw with current readings in new unit
            uiTempPressUpdate(sensorsRtcOk(), readTempC(), readHum(), readPress(), setUseF, setUseInHg);
          }
          if (inTempRefreshBtn(ev.x, ev.y)) {
            // Re-sleep then re-wake: flushes residual heat, restarts warm-up
            sensorsIdleBMP();
            sensorsWakeBMP();
            tpWakeMs = millis();
            uiTempPressDrawFull(sensorsRtcOk(), NAN, NAN, NAN, setUseF, setUseInHg);
          }
        }
      }
    }

    // also pass DOWN into games (for pong drag)
    if (screen == SCR_GAMES && ev.type == TE_DOWN) {
      gamesOnTouch(ev);
    }
  }

  if (eLong) {
    lastInteractionMs = now;
    handleBack();
    return;
  }

  if (d != 0) {
    lastInteractionMs = now; // Reset idle timer on encoder
    if (screen == SCR_HOME) {
      const int prevSel = homeSel;
      const int prevTop = homeTop;
      homeSel += d;
      clampHomeViewport();
      homeRedrawSelectionOnly(prevSel, prevTop);
    }
    else if (screen == SCR_WEATHER) {
      if (d > 0) weatherNextCity();
      else       weatherPrevCity();
    }
    else if (screen == SCR_NEWS) {
      if (d > 0) newsNext();
      else       newsPrev();
      uiNewsScreenUpdate(wifiOk(), newsGetCached());
    }
    else if (screen == SCR_NEWS_CHANNELS) {
      newsChanSel += d;
      if (newsChanSel < 0) newsChanSel = NEWS_CHAN_COUNT - 1;
      if (newsChanSel >= NEWS_CHAN_COUNT) newsChanSel = 0;
      uiNewsChannelsMenuDrawFull(newsChanSel, 0, NEWS_CHAN_ITEMS, NEWS_CHAN_COUNT);
    }
    else if (screen == SCR_NEWS_CATEGORIES) {
      newsCatSel += d;
      if (newsCatSel < 0) newsCatSel = NEWS_CAT_COUNT - 1;
      if (newsCatSel >= NEWS_CAT_COUNT) newsCatSel = 0;
      const char* chanName = (newsChanSel == 1) ? "GLOBAL" : "INDIA";
      uiNewsCategoriesMenuDrawFull(newsCatSel, 0, NEWS_CAT_ITEMS, NEWS_CAT_COUNT, chanName);
    }
    else if (screen == SCR_NEWS_STUDY) {
      // Encoder scrolls description (1 line per click), not article change
      newsStudyScrollOffset += d;
      if (newsStudyScrollOffset < 0) newsStudyScrollOffset = 0;
      uiNewsScreenUpdateStudy(newsGetCached(), newsStudyScrollOffset);
    }
    else if (screen == SCR_GAMES && !gamesInGame()) {
        const int prevSel = gamesSel;
        const int prevTop = gamesTop;
        gamesSel += d;
        clampGamesViewport();
        if (prevSel != gamesSel) gamesRedrawSelectionOnly(prevSel, prevTop);
      }
    else if (screen == SCR_TOOLS) {
      const int prevSel = toolsSel;
      const int prevTop = toolsTop;
      toolsSel += d;
      clampToolsViewport();
      if (prevSel != toolsSel) toolsRedrawSelectionOnly(prevSel, prevTop);
    }
    else if (screen == SCR_ALARM && d != 0) {
        if (alarmEditMode) {
            Alarm* a = alarmsGet(alarmSel);
            if (alarmEditField == 0) { 
                bool isPM = (a->hour >= 12);
                int h12 = a->hour % 12;
                if (h12 == 0) h12 = 12;
                h12 += d;
                while (h12 < 1) h12 += 12;
                while (h12 > 12) h12 -= 12;
                a->hour = (h12 == 12 ? 0 : h12) + (isPM ? 12 : 0);
                alarmsSave(); 
            }
            else if (alarmEditField == 1) { a->minute = (a->minute + d + 60) % 60; alarmsSave(); }
            else if (alarmEditField == 2) { a->hour = (a->hour + 12) % 24; alarmsSave(); }
            else if (alarmEditField == 3) { a->enabled = !a->enabled; alarmsSave(); }
        } else {
            alarmSel = (alarmSel + d + 3) % 3;
        }
        showAlarms();
    }
    else if (screen == SCR_CALC) {
      // ENHANCED: Calculator history navigation with encoder
      if (d > 0) calcHistoryNext();
      else calcHistoryPrev();
    }
    else if (screen == SCR_UNITCONV) {
      if (d > 0) ucInVal += (ucMode == 0 ? 0.5f : 10.0f);
      else ucInVal -= (ucMode == 0 ? 0.5f : 10.0f);
      unitConvRecalc(); showUnitConv();
    }
    else if (screen == SCR_SETTINGS) {
      int prev = settingsSel;
      settingsSel += d;
      clampSettingsViewport();
      if (prev != settingsSel) showSettings();
    }
    else if (screen == SCR_FORMAT_OPTIONS) {
      int prev = formatSel;
      formatSel += d;
      if (formatSel < 0) formatSel = FORMAT_COUNT - 1;
      if (formatSel >= FORMAT_COUNT) formatSel = 0;
      if (prev != formatSel) uiFormatOptionsRedrawSelection(formatSel, FORMAT_ITEMS, FORMAT_COUNT);
    }
    else if (screen == SCR_SKETCH_GALLERY) {
      int prev = gallerySel;
      gallerySel += d;
      clampGalleryViewport();
      if (prev != gallerySel) uiSketchGalleryDrawFull(gallerySel, galleryTop);
    }
    else if (screen == SCR_DOCS) {
      if (viewingDoc) {
        if (d > 0 && docCurrentPage + 1 < docPageCount) {
          docCurrentPage++;
          saveDocProgress(currentDocName, docCurrentPage);
          drawDocContent();
        } else if (d < 0 && docCurrentPage > 0) {
          docCurrentPage--;
          saveDocProgress(currentDocName, docCurrentPage);
          drawDocContent();
        }
      } else {
        int prevSel = docsSel;
        int prevTop = docsTop;
        docsSel += d;
        refreshDocFiles();
        int count = docFileCount;
        if (docsSel < 0) docsSel = 0;
        if (docsSel >= count) docsSel = (count > 0 ? count - 1 : 0);
        const int rowH = 28;
        const int topH = 5;
        const int bodyH = uiBodyH() - 34 - topH;
        const int maxRows = bodyH / rowH;
        int maxTop = (count > maxRows) ? (count - maxRows) : 0;
        if (docsTop > maxTop) docsTop = maxTop;
        if (docsSel < docsTop) docsTop = docsSel;
        if (docsSel >= docsTop + maxRows) docsTop = docsSel - (maxRows - 1);
        if (prevSel != docsSel) docsRedrawSelectionOnly(prevSel, prevTop);
      }
    }
  }

  if (eClick) {
    lastInteractionMs = now; // Reset idle timer on click
    if (screen == SCR_TITLE && titleReadyToExit) {
      // Encoder click on title: restore last active screen
      uiFadeOut(100);
      if (lastActiveScreen == SCR_TITLE || lastActiveScreen == SCR_HOME) {
        goHome();
      } else {
        screen = lastActiveScreen;
        switch (screen) {
          case SCR_HOME: goHome(); break;
          case SCR_TIME: enterTimeScreen(); break;
          case SCR_TIME_STUDY: enterStudyMode(); break;
          case SCR_WEATHER: enterWeatherScreen(); break;
          case SCR_NEWS: enterNewsScreen(); break;
          case SCR_QUOTES: enterQuotesScreen(); break;
          case SCR_QUOTES_STUDY: enterQuotesStudyMode(); break;
          case SCR_NEWS_STUDY: enterNewsStudyScreen(); break;
          case SCR_GAMES: enterGamesScreen(); break;
          case SCR_TEMP_PRESSURE: enterTempPressureScreen(); break;
          case SCR_STOPWATCH: showStopwatch(); break;
          case SCR_TIMER: showTimer(); break;
          case SCR_POMODORO: enterPomodoroScreen(); break;
          case SCR_UNITCONV: showUnitConv(); break;
          case SCR_ALARM: showAlarms(); break;
          case SCR_CALC: uiCalcDrawFull(calcExpr, calcResult); break;
          case SCR_TOOLS: showTools(); break;
          case SCR_SETTINGS: showSettings(); break;
          case SCR_DOCS: enterDocsScreen(); break;
          case SCR_SKETCH:
            uiSketchDrawFull(currentSketchColor);
            uiSketchRedrawCurrent();
            break;
          default: goHome(); break;
        }
      }
      uiFadeIn(100);
      return;
    }
    else if (screen == SCR_HOME) openSelectedHome();
    else if (screen == SCR_GAMES && !gamesInGame()) gamesStartByIndex(gamesSel);
    else if (screen == SCR_ALARM) {
        if (alarmEditMode) {
            alarmEditField++;
            if (alarmEditField > 3) {
                alarmEditMode = false;
            }
        } else {
            alarmEditMode = true;
            alarmEditField = 0;
        }
        showAlarms();
    }
    else if (screen == SCR_TOOLS) {
      const char* it = TOOLS_ITEMS[toolsSel];
      if (strcmp(it, "ALARM CLOCK") == 0) { showAlarms(); }
      else if (strcmp(it, "CALCULATOR") == 0) { calcReset(); showCalc(); }
      else if (strcmp(it, "STOPWATCH") == 0) { showStopwatch(); }
      else if (strcmp(it, "TIMER") == 0) { showTimer(); }
      else if (strcmp(it, "UNIT CONVERTER") == 0) { showUnitConv(); }
      else if (strcmp(it, "SKETCHES") == 0) { enterSketchGallery(); }
      else if (strcmp(it, "SETTINGS") == 0) { showSettings(); }
      else if (strcmp(it, "FORMAT OPTIONS") == 0) { showFormatOptions(); }
      else if (strcmp(it, "ABOUT") == 0) { uiShowScreenTitle("ABOUT"); uiShowPlaceholderBody("NAA TV Gadget", "ESP32-S3 multi-tool"); }
    }
    else if (screen == SCR_NEWS_CHANNELS) {
      uiFadeOut(100);
      showNewsCategories();
      uiFadeIn(100);
    }
    else if (screen == SCR_NEWS_CATEGORIES) {
      uiFadeOut(100);
      enterNewsScreen();
      uiFadeIn(100);
    }
    else if (screen == SCR_DOCS) {
      if (viewingDoc) { viewingDoc = false; showDocsMenu(); }
      else {
        // Use fixed-size docFileList instead of heap-allocating getDocFiles()
        refreshDocFiles();
        if (docsSel >= 0 && docsSel < docFileCount) openDoc(docFileList[docsSel]);
      }
    }
    else handleBack();
  }

  if (screen == SCR_TIME || screen == SCR_TIME_STUDY) timeScreenLoop(now);

  static bool wasFormatBusy = false;
  if (wasFormatBusy && !formatBusy && screen == SCR_FORMAT_OPTIONS) {
    showFormatOptions(); // Cleanly redraw the UI when async background formatting finishes
  }
  wasFormatBusy = formatBusy;
}

#pragma once
#include <stdint.h>
#include <stdbool.h>

#include <Adafruit_ILI9341.h>

#include "storage.h"   // WeatherCache
#include "quotes.h"    // QuoteData
#include "news.h"      // NewsData
#include "alarm.h"     // Alarm

// ===================== Core UI =====================
enum FaceExpression {
  EXPR_SMILE,
  EXPR_SAD,
  EXPR_CRYING,
  EXPR_WINK,
  EXPR_NEUTRAL,
  EXPR_BLINKING,
  EXPR_LOOK_LEFT,
  EXPR_LOOK_RIGHT,
  EXPR_BUSY,
  EXPR_SLEEPING
};

void uiBegin();
void uiSetBacklight(bool on);
bool uiBacklightIsOn();
void uiSetBrightnessLevel(int level);
void uiSetTheme(bool dark);
void uiSetAccentTheme(int idx); // NEW: 0=Blue, 1=Green, 2=Orange, 3=Purple
void uiSetFontSizeSetting(int size);
void uiSetMascotExpression(FaceExpression expr); // NEW: React to events
void uiRedrawHeaderMascot(); // NEW: Partial redraw for mascot
void uiSetNotification(bool active); // NEW: Notification badge
void uiClearContent(); // NEW: Partial redraw area
void uiDrawQuickSettings(int brightness, bool dark, bool wifi); // NEW: Quick Settings overlay
void uiFadeOut(uint16_t ms);
void uiFadeIn(uint16_t ms);

// ===== Title screen (no header/footer) =====
void uiTitleScreenDraw();
void uiTitleScreenUpdate(); // NEW: For face animations

// ===== Home/menu =====
void uiHomeInit();
void uiHomeSetSelection(int selected, int top);
void uiHomeDrawFull(int selected, int top);
void uiHomeDrawRow(int rowIndex, const char* label, bool selected);
void uiHomeDrawBottomNavHome(uint8_t activeIndex);

void uiShowScreenTitle(const char* title);
void uiShowPlaceholderBody(const char* line1, const char* line2);

// ===== TIME screen =====
void uiTimeScreenDrawFull(bool rtcOk);
void uiTimeScreenUpdate(const char* timeStr, const char* dateStr);
void uiTimeScreenDrawStudy(bool rtcOk);   // NEW: No header/footer
void uiTimeScreenUpdateStudy(const char* timeStr, const char* dateStr); // NEW: Full screen update

// ===== WEATHER screen =====
void uiWeatherScreenDrawFull(int cityIdx, const WeatherCache& wx, bool wifiOk, bool busy);
void uiWeatherScreenSetBusy(bool busy);
void uiWeatherScreenUpdateData(int cityIdx, const WeatherCache& wx, bool wifiOk);

// ===== Info/WiFi =====
void uiInfoScreenDraw(bool wifiOk, const char* line1, const char* line2, const char* line3);
void uiWifiScreenDraw(bool wifiOk, const char* ssid, const char* ip, int rssi);

// ===== Dot test =====
void uiDrawDot(int x, int y);

// ===== QUOTES =====
void uiQuotesScreenDrawFull(bool wifiOk, bool busy, const QuoteData& q);
void uiQuotesScreenSetBusy(bool busy);
void uiQuotesScreenUpdate(bool wifiOk, const QuoteData& q);
void uiQuotesScreenDrawStudy(const QuoteData& q);   // NEW: No header/footer
void uiQuotesScreenUpdateStudy(const QuoteData& q); // NEW: Full screen update

// ===== NEWS =====
void uiNewsScreenDrawFull(bool wifiOk, bool busy, const NewsData& n);
void uiNewsScreenSetBusy(bool busy);
void uiNewsScreenUpdate(bool wifiOk, const NewsData& n);
void uiNewsChannelsMenuDrawFull(int sel, int top, const char* const* items, int count);
void uiNewsCategoriesMenuDrawFull(int sel, int top, const char* const* items, int count, const char* chanName);
void uiNewsScreenDrawStudy(const NewsData& n, int scrollOffset = 0);   // Article reader with scroll
void uiNewsScreenUpdateStudy(const NewsData& n, int scrollOffset = 0); // Scroll-aware update

void uiTempPressDrawFull(bool rtcOk, float tC, float hPct, float pHpa, bool useF, bool useInHg);
void uiTempPressUpdate(bool rtcOk, float tC, float hPct, float pHpa, bool useF, bool useInHg);
void uiTempPressSetStatus(const char* fmt, ...); // warm-up / live status line

void uiToolsMenuDrawFull(int sel, int top, const char* const* items, int count);
void uiToolsMenuDrawRow(int rowIndex, const char* label, bool selected);

void uiGamesMenuDrawFull(int sel, int top, const char* const* items, int count);
void uiGamesMenuDrawRow(int rowIndex, const char* label, bool selected);

// ENHANCED: Calculator history structure definition (MUST be defined before extern declarations)
struct CalcHistoryEntry {
  char expression[64];
  char result[32];
  uint32_t timestamp;
};

// ENHANCED: External calculator variables for UI access
extern bool calcError;
extern int historyViewIndex;
extern int calcHistoryCount;  // total valid entries in ring buffer

void uiCalcDrawFull(const char* expr, const char* result);

void uiStopwatchDrawFull(uint32_t ms, bool running);
void uiStopwatchUpdateTime(uint32_t ms, bool running);
void uiTimerDrawFull(uint32_t remainMs, bool running, int presetIdx);
void uiTimerUpdateTime(uint32_t remainMs, bool running, int presetIdx);
void uiPomodoroDrawFull(uint32_t remainMs, bool running, bool isWork, int cycles);
void uiPomodoroUpdate(uint32_t remainMs, bool running, bool isWork, int cycles);
void uiSketchDrawFull(uint16_t currentColor);
void uiSketchDrawFooter(uint16_t currentColor);
void uiSketchRedrawCurrent();
void uiSketchClear();
void uiSketchDrawLine(int x1, int y1, int x2, int y2, uint16_t color, int z);
void uiSketchDrawBezier(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color, int z);
void uiSketchSave();
int uiSketchGalleryGetCount();
void uiSketchGalleryDrawFull(int sel, int top);
void uiSketchView(int idx, int sel, int top);
void uiSketchDelete(int idx);
void uiUnitConvDrawFull(int mode, float inVal, float outVal, const char* inLabel, const char* outLabel);
void uiAlarmsDrawFull(int sel, bool isEditing, int editField, Alarm* alarms, int maxAlarms);

// ============================================================================
// NEW: accessors so external modules (games) can draw consistently.
// These are read-only helpers. They do not change existing UI behavior.
// ============================================================================
Adafruit_ILI9341& uiTft();

int uiHeaderH();
int uiFooterH();
int uiBodyY0();
int uiBodyH();

uint16_t uiColorBg();
uint16_t uiColorText();
uint16_t uiColorSub();
uint16_t uiColorAccent();
uint16_t uiColorSelBg();
uint16_t uiColorDivider();

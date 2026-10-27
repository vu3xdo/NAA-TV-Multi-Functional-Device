#include "ui.h"
#include <stdarg.h>
#include "config.h"
#include "weather.h"
#include "touch.h"

#include <SPI.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <string.h>
#include <math.h>

// ===== Sans fonts =====
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

extern const char* HOME_ITEMS[];
extern const int HOME_COUNT;

static Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
static bool blOn = true;
static int uiBrightnessLevel = 1;
static bool uiThemeDark = true;
static int uiFontSizeSetting = 1;
static bool hasNotification = false; // NEW: Notification state

struct SketchLine { int16_t x1, y1, x2, y2; uint16_t color; uint8_t thick; };
static SketchLine* currentSketch = nullptr; // Allocated on demand to save RAM
static int currentSketchLen = 0;
static const int SKETCH_MAX_LINES = 4000;

static FaceExpression currentExpr = EXPR_NEUTRAL;
static uint32_t lastExprChangeMs = 0;
static uint32_t lastFaceUpdateMs = 0;
static float breathOffset = 0.0f;

// Canvas for flicker-free face animation (ESP32-S3 has plenty of RAM)
static GFXcanvas16* faceCanvas = nullptr;
static const int CANVAS_W = 160;
static const int CANVAS_H = 110;

// ================== Layout ==================
static const int UI_HEADER_H = 30;
static const int UI_FOOTER_H = 34;

// Forward declaration so helpers defined later are callable from anywhere in this file
static void drawFooterBackHint(const char* s);

static const int HOME_LIST_Y = UI_HEADER_H;
static const int HOME_LIST_H = TFT_H - UI_HEADER_H - UI_FOOTER_H;
static const int HOME_ROW_H  = 28;
static const int HOME_ROWS   = HOME_LIST_H / HOME_ROW_H;

// ================== Colors ==================
static uint16_t C_BG;
static uint16_t C_TEXT;
static uint16_t C_SUB;
static uint16_t C_ACCENT;
static uint16_t C_SEL_BG;
static uint16_t C_DIV;
static uint16_t C_OK;
static uint16_t C_BAD;
static uint16_t C_TILE_BG;
static uint16_t C_TILE_FG;
static uint16_t C_TILE_BORDER;
static uint16_t C_BTN_BG;
static uint16_t C_BTN_FG;
static uint16_t C_BTN_BORDER;

static int uiAccentIdx = 0; // 0: Cyber Blue, 1: Emerald Green, 2: Sunset Orange, 3: Royal Purple
static const uint16_t ACCENT_COLORS[] = {
  ILI9341_CYAN,   // Cyber Blue
  ILI9341_GREEN,  // Emerald Green
  ILI9341_ORANGE, // Sunset Orange
  0x780F          // Royal Purple
};

static const uint16_t ACCENT_COLORS_LIGHT[] = {
  0x0019, // Deep Royal Blue (better visibility in light theme)
  0x3C47, // Forest Green
  0xBA00, // Terracotta
  0x500A  // Deep Plum
};

static void uiApplyThemeInternal() {
  if (uiThemeDark) {
    C_BG     = ILI9341_BLACK;
    C_TEXT   = ILI9341_WHITE;
    C_SUB    = ILI9341_DARKGREY;
    C_ACCENT = ACCENT_COLORS[uiAccentIdx];
    C_SEL_BG = ILI9341_NAVY;
    C_DIV    = ILI9341_DARKGREY;
    C_OK     = ILI9341_GREEN;
    C_BAD    = ILI9341_RED;
    C_TILE_BG     = 0x2104; // Charcoal
    C_TILE_FG     = ILI9341_WHITE;
    C_TILE_BORDER = ILI9341_DARKGREY;
    C_BTN_BG      = ILI9341_DARKCYAN;
    C_BTN_FG      = ILI9341_WHITE;
    C_BTN_BORDER  = ILI9341_CYAN;
  } else {
    C_BG     = ILI9341_WHITE;
    C_TEXT   = ILI9341_BLACK;
    C_SUB    = 0x7BEF; // Medium Grey
    C_ACCENT = ACCENT_COLORS_LIGHT[uiAccentIdx];
    C_SEL_BG = 0xDEFB; // Very Soft Light Blue/Grey
    C_DIV    = 0xE71C; // Very Light Grey
    C_OK     = 0x0400; // Deep Green
    C_BAD    = 0xA000; // Deep Red
    C_TILE_BG     = 0xF7BE; // Cloud Grey
    C_TILE_FG     = ILI9341_BLACK;
    C_TILE_BORDER = 0xD6BA; // Silver Border
    C_BTN_BG      = 0x4208; // Slate Grey
    C_BTN_FG      = ILI9341_WHITE;
    C_BTN_BORDER  = 0x5ACB; // Slate Blue Border
  }
}

static void spiBeginOnce() {
  static bool done = false;
  if (done) return;
  done = true;
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
}

static void uiApplyBacklightPwm() {
  pinMode(TFT_BL, OUTPUT);
  int level = uiBrightnessLevel;
  if (!blOn || level <= 0) {
    analogWrite(TFT_BL, 0);
  } else {
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    int duty = (255 * level) / 4;
    analogWrite(TFT_BL, duty);
  }
}

void uiSetBacklight(bool on) {
  blOn = on;
  uiApplyBacklightPwm();
}

void uiSetBrightnessLevel(int level) {
  if (level < 0) level = 0;
  if (level > 4) level = 4;
  uiBrightnessLevel = level;
  if (blOn) uiApplyBacklightPwm();
}

void uiSetTheme(bool dark) {
  uiThemeDark = dark;
  uiApplyThemeInternal();
}

void uiSetAccentTheme(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 3) idx = 3;
  uiAccentIdx = idx;
  uiApplyThemeInternal();
}

void uiSetMascotExpression(FaceExpression expr) {
  currentExpr = expr;
  lastExprChangeMs = millis(); // Lock this expression for a while
}

void uiFadeOut(uint16_t ms) {
  if (!blOn) return;
  int steps = 10;
  int stepMs = ms / steps;
  for (int i = steps; i >= 0; i--) {
    int level = (uiBrightnessLevel * i) / steps;
    analogWrite(TFT_BL, (255 * level) / 4);
    delay(stepMs);
  }
}

void uiFadeIn(uint16_t ms) {
  if (!blOn) return;
  int steps = 10;
  int stepMs = ms / steps;
  for (int i = 0; i <= steps; i++) {
    int level = (uiBrightnessLevel * i) / steps;
    analogWrite(TFT_BL, (255 * level) / 4);
    delay(stepMs);
  }
}

bool uiBacklightIsOn() { return blOn; }

static const GFXfont* mapFontForSetting(const GFXfont* f) {
  if (uiFontSizeSetting == 0) {
    if (f == &FreeSans12pt7b)      return &FreeSans9pt7b;
    if (f == &FreeSansBold12pt7b)  return &FreeSans12pt7b;
    if (f == &FreeSansBold24pt7b)  return &FreeSansBold12pt7b;
  } else if (uiFontSizeSetting == 2) {
    if (f == &FreeSans9pt7b)       return &FreeSans12pt7b;
    if (f == &FreeSans12pt7b)      return &FreeSansBold12pt7b;
  }
  return f;
}

void uiSetFontSizeSetting(int size) {
  if (size < 0 || size > 2) size = 1;
  uiFontSizeSetting = size;
}

void uiSetNotification(bool active) {
  hasNotification = active;
}

void uiClearContent() {
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
}

static void setFont(const GFXfont* f) {
  tft.setFont(mapFontForSetting(f));
}

void uiDrawQuickSettings(int brightness, bool dark, bool wifi) {
  const int QS_H = 130;
  const uint16_t qsBg = C_TILE_BG;
  const uint16_t qsFg = C_TILE_FG;

  tft.fillRect(0, 0, TFT_W, QS_H, qsBg);
  tft.drawFastHLine(0, QS_H - 1, TFT_W, C_ACCENT);

  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(qsFg);
  tft.setCursor(10, 30);
  tft.print("QUICK SETTINGS");

  setFont(&FreeSans9pt7b);
  // Brightness Row
  tft.setCursor(20, 65);
  tft.print("Brightness: ");
  const char* blStr[] = {"Off", "25%", "50%", "75%", "100%"};
  tft.print(blStr[brightness]);

  // Theme Row
  tft.setCursor(20, 95);
  tft.print("Theme: ");
  tft.print(dark ? "Dark" : "Light");

  // WiFi Row
  tft.setCursor(20, 125);
  tft.print("WiFi: ");
  tft.print(wifi ? "Connected" : "Disconnected");

  // Indicator to close
  tft.fillRoundRect(TFT_W/2 - 20, QS_H - 10, 40, 4, 2, C_SUB);
}

static void getBounds(const char* s, int16_t& x1, int16_t& y1, uint16_t& w, uint16_t& h) {
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
}

static int xCentered(const char* s, const GFXfont* font) {
  setFont(font);
  int16_t x1, y1; uint16_t w, h;
  getBounds(s, x1, y1, w, h);
  int x = (TFT_W - (int)w) / 2;
  if (x < 2) x = 2;
  if (x > TFT_W - (int)w - 2) x = TFT_W - (int)w - 2;
  return x;
}

static int baselineCenteredBand(int yTop, int bandH, const char* s, const GFXfont* font) {
  setFont(font);
  int16_t x1, y1; uint16_t w, h;
  getBounds(s, x1, y1, w, h);
  return yTop + (bandH - (int)h) / 2 - (int)y1;
}

static void drawCenteredBand(int yTop, int bandH, const char* s, const GFXfont* font, uint16_t fg, uint16_t bg) {
  tft.fillRect(0, yTop, TFT_W, bandH, bg);
  setFont(font);
  tft.setTextColor(fg);
  int x = xCentered(s, font);
  int baseline = baselineCenteredBand(yTop, bandH, s, font);
  if (baseline < yTop + 10) baseline = yTop + 10;
  if (baseline > yTop + bandH - 3) baseline = yTop + bandH - 3;
  tft.setCursor(x, baseline);
  tft.print(s);
}

static void drawMascotIcon(int x, int y, int size, FaceExpression expr) {
  int r = size / 2;
  tft.drawRoundRect(x, y, size, size, 4, C_SUB);
  
  // Eyes (Simple pixels or small rects)
  int eyeY = y + size / 3;
  int eyeX1 = x + size / 4;
  int eyeX2 = x + 3 * size / 4;
  
  if (expr == EXPR_BLINKING) {
    tft.drawFastHLine(eyeX1 - 2, eyeY, 4, C_ACCENT);
    tft.drawFastHLine(eyeX2 - 2, eyeY, 4, C_ACCENT);
  } else {
    tft.fillRect(eyeX1 - 2, eyeY - 2, 4, 4, C_ACCENT);
    tft.fillRect(eyeX2 - 2, eyeY - 2, 4, 4, C_ACCENT);
  }

  // Mini spectrum mouth
  int mouthY = y + 2 * size / 3;
  int barW = 2;
  int barGap = 1;
  int barCount = 5;
  int startX = x + (size - (barCount * (barW + barGap))) / 2;
  
  for (int i = 0; i < barCount; i++) {
    int bH = 2;
    if (expr == EXPR_SMILE) bH = 2 + (i == 0 || i == 4 ? 2 : 6);
    else if (expr == EXPR_SAD) bH = 2 + (i == 2 ? 2 : 6);
    else if (expr == EXPR_BUSY) bH = 2 + (rand() % 8);
    else bH = 4;
    
    tft.fillRect(startX + i * (barW + barGap), mouthY - (bH / 2), barW, bH, C_ACCENT);
  }

  // Notification Badge (Red Dot)
  if (hasNotification) {
    tft.fillCircle(x + size - 2, y + 2, 4, ILI9341_RED);
    tft.drawCircle(x + size - 2, y + 2, 4, ILI9341_WHITE);
  }
}

static void drawHeader(const char* title, bool wifiOk) {
  tft.fillRect(0, 0, TFT_W, UI_HEADER_H, C_BG);
  tft.drawFastHLine(0, UI_HEADER_H - 1, TFT_W, C_DIV);

  // Draw mascot icon in the left (x=0 to 30)
  drawMascotIcon(4, 4, 22, currentExpr);

  // WiFi Dot on the right (x=TFT_W-20 to TFT_W)
  const int dotR = 4;
  const int dotX = TFT_W - 12;
  const int dotY = UI_HEADER_H / 2;
  tft.fillCircle(dotX, dotY, dotR, wifiOk ? C_OK : C_BAD);
  tft.drawCircle(dotX, dotY, dotR, C_TEXT);

  // Auto-adjusting Title in the middle (available width: TFT_W - 60)
  const int maxW = TFT_W - 60;
  const int startX = 30;
  
  // Try fonts from largest to smallest to fit maxW
  const GFXfont* fonts[] = { &FreeSansBold12pt7b, &FreeSans9pt7b, nullptr };
  const GFXfont* selectedFont = nullptr;
  
  for (int i = 0; i < 3; i++) {
    selectedFont = fonts[i];
    if (selectedFont == nullptr) break; // Use system font as last resort
    
    int16_t x1, y1; uint16_t w, h;
    tft.setFont(selectedFont);
    tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    if (w <= maxW) break;
  }
  
  tft.setFont(selectedFont);
  tft.setTextColor(C_TEXT);
  
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  
  // Center in the middle zone (startX to TFT_W-30)
  int drawX = startX + (maxW - w) / 2;
  // Correct baseline-aware vertical centering
  int baseline = (UI_HEADER_H + (int)h) / 2 - (int)y1;
  if (selectedFont == nullptr) baseline = UI_HEADER_H / 2 + 4; // System font adjustment
  if (baseline < 4) baseline = 4;
  if (baseline > UI_HEADER_H - 2) baseline = UI_HEADER_H - 2;
  
  tft.setCursor(drawX, baseline);
  tft.print(title);
}

void uiRedrawHeaderMascot() {
  tft.fillRect(0, 0, 30, UI_HEADER_H - 1, C_BG);
  drawMascotIcon(4, 4, 22, currentExpr);
}

void uiBegin() {
  spiBeginOnce();
  uiApplyThemeInternal();
  uiSetBacklight(true);

  tft.begin();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(C_BG);

  setFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);
  tft.setCursor(10, 26);
  tft.print("NAA TV");
}

void uiDrawDot(int x, int y) {
  tft.fillCircle(x, y, 3, ILI9341_CYAN);
  tft.drawCircle(x, y, 3, ILI9341_WHITE);
}

static void drawFace(FaceExpression expr, float breath) {
  if (!faceCanvas) return;
  
  faceCanvas->fillScreen(C_BG);
  
  const int x = CANVAS_W / 2;
  const int y = 40;
  const int eyeW = 28;
  const int eyeH = 24;
  const int eyeXOff = 42;
  const int eyeY = y + (int)(breath * 2.5f);
  const uint16_t glowColor = (expr == EXPR_SAD || expr == EXPR_CRYING) ? ILI9341_BLUE : C_ACCENT;

  // HUD corner markers
  const int mS = 8;
  faceCanvas->drawFastHLine(0, 0, mS, C_SUB); faceCanvas->drawFastVLine(0, 0, mS, C_SUB);
  faceCanvas->drawFastHLine(CANVAS_W-mS, 0, mS, C_SUB); faceCanvas->drawFastVLine(CANVAS_W-1, 0, mS, C_SUB);
  faceCanvas->drawFastHLine(0, CANVAS_H-1, mS, C_SUB); faceCanvas->drawFastVLine(0, CANVAS_H-mS, mS, C_SUB);
  faceCanvas->drawFastHLine(CANVAS_W-mS, CANVAS_H-1, mS, C_SUB); faceCanvas->drawFastVLine(CANVAS_W-1, CANVAS_H-mS, mS, C_SUB);

  // Left & Right Eye Units (Rounded Rects)
  for (int side = -1; side <= 1; side += 2) {
    int curX = x + (side * eyeXOff);
    
    // Outer frame glow
    faceCanvas->drawRoundRect(curX - (eyeW/2) - 2, eyeY - (eyeH/2) - 2, eyeW + 4, eyeH + 4, 6, C_SUB);
    
    if (expr == EXPR_BLINKING || (expr == EXPR_WINK && side == 1) || expr == EXPR_SLEEPING) {
      faceCanvas->fillRect(curX - (eyeW/2), eyeY - 2, eyeW, 4, glowColor);
    } else {
      faceCanvas->fillRoundRect(curX - (eyeW/2), eyeY - (eyeH/2), eyeW, eyeH, 4, ILI9341_BLACK);
      faceCanvas->drawRoundRect(curX - (eyeW/2), eyeY - (eyeH/2), eyeW, eyeH, 4, glowColor);
      
      // Technical Iris (Crosshair / Target style)
      int pX = 0, pY = 0;
      if (expr == EXPR_LOOK_LEFT) pX = -5;
      if (expr == EXPR_LOOK_RIGHT) pX = 5;
      
      faceCanvas->drawCircle(curX + pX, eyeY + pY, 6, glowColor);
      faceCanvas->drawFastHLine(curX + pX - 8, eyeY + pY, 16, glowColor);
      faceCanvas->drawFastVLine(curX + pX, eyeY + pY - 8, 16, glowColor);
      faceCanvas->fillCircle(curX + pX, eyeY + pY, 2, ILI9341_WHITE); // Bright core
    }
  }

  // Robotic Mouth (Voice Spectrum Bars)
  const int mouthX = x;
  const int mouthY = y + 50;
  const int barW = 6;
  const int barGap = 4;
  const int barCount = 7;
  
  for (int i = 0; i < barCount; i++) {
    int bH = 4;
    float distFromCenter = abs(i - (barCount / 2)) / (float)(barCount / 2);
    
    switch (expr) {
      case EXPR_SMILE:
        bH = 6 + (int)((1.0f - distFromCenter) * 16.0f);
        break;
      case EXPR_SAD:
        bH = 6 + (int)(distFromCenter * 16.0f);
        break;
      case EXPR_CRYING:
      case EXPR_BUSY:
        bH = 4 + (rand() % 12); // Rapid flickering/static
        break;
      case EXPR_NEUTRAL:
      case EXPR_BLINKING:
      case EXPR_SLEEPING:
        bH = 4 + (int)(sin(breath * 2.0f) * 4.0f); // Pulsing
        break;
      default:
        bH = 8;
        break;
    }
    
    int bX = mouthX - ((barCount * (barW + barGap)) / 2) + i * (barW + barGap);
    faceCanvas->fillRect(bX, mouthY - (bH / 2), barW, bH, glowColor);
  }

  // Scanline Effect
  int scanY = (int)((breath + 1.0f) * 0.5f * (CANVAS_H - 4));
  faceCanvas->drawFastHLine(0, scanY, CANVAS_W, ILI9341_DARKGREY);
  faceCanvas->drawFastHLine(0, scanY + 1, CANVAS_W, ILI9341_BLACK);
}

void uiTitleScreenDraw() {
  if (!faceCanvas) {
    faceCanvas = new GFXcanvas16(CANVAS_W, CANVAS_H);
  }
  
  tft.fillScreen(C_BG);
  // Title band at 165
  drawCenteredBand(165, 45, "NAA TV", &FreeSansBold24pt7b, C_ACCENT, C_BG);
  
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  int x = xCentered("Touch to continue", &FreeSans9pt7b);
  tft.setCursor(x, 230);
  tft.print("Touch to continue");
  
  currentExpr = EXPR_NEUTRAL;
  lastExprChangeMs = millis();
  lastFaceUpdateMs = millis();
  
  drawFace(currentExpr, 0);
  tft.drawRGBBitmap((TFT_W - CANVAS_W)/2, 35, faceCanvas->getBuffer(), CANVAS_W, CANVAS_H);
}

void uiTitleScreenUpdate() {
  uint32_t now = millis();
  if (now - lastFaceUpdateMs < FACE_ANIM_INTERVAL_MS) return;
  lastFaceUpdateMs = now;

  // Breathing calculation
  float phase = (float)(now % FACE_BREATH_PERIOD_MS) / FACE_BREATH_PERIOD_MS;
  breathOffset = sin(phase * 2.0f * PI);

  // Expression state machine
  if (now - lastExprChangeMs > 4000) {
    lastExprChangeMs = now;
    currentExpr = (FaceExpression)(esp_random() % 8);
  }

  // Draw to canvas and then push to screen (flicker-free)
  drawFace(currentExpr, breathOffset);
  tft.drawRGBBitmap((TFT_W - CANVAS_W)/2, 35, faceCanvas->getBuffer(), CANVAS_W, CANVAS_H);
}

// ================== HOME ==================
void uiHomeDrawBottomNavHome(uint8_t activeIndex) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  const char* items[4] = { "HOME", "INFO", "WIFI", "TOOLS" };
  const int w = TFT_W / 4;

  setFont(&FreeSans9pt7b);
  for (int i = 0; i < 4; i++) {
    const int x = i * w;
    const bool act = (i == activeIndex);
    if (act) tft.fillRect(x, y + 1, w, UI_FOOTER_H - 1, C_SEL_BG);

    tft.setTextColor(act ? C_ACCENT : C_TEXT);
    tft.setCursor(x + 10, y + 24);
    tft.print(items[i]);
  }
}

void uiHomeDrawRow(int rowIndex, const char* label, bool selected) {
  const int y = HOME_LIST_Y + rowIndex * HOME_ROW_H;
  const uint16_t bg = selected ? C_SEL_BG : C_BG;
  const uint16_t fg = selected ? C_ACCENT : C_TEXT;

  tft.fillRect(0, y, TFT_W, HOME_ROW_H, bg);

  setFont(&FreeSans12pt7b);
  tft.setTextColor(fg);
  tft.setCursor(14, y + 22);
  tft.print(label ? label : "");

  tft.drawFastHLine(0, y + HOME_ROW_H - 1, TFT_W, selected ? ILI9341_BLUE : C_DIV);
}

void uiHomeDrawFull(int selected, int top) {
  drawHeader("NAA TV", (WiFi.status() == WL_CONNECTED));
  // Partial redraw: only clear the content area
  uiClearContent();
  
  const int count = HOME_COUNT;

  for (int i = 0; i < HOME_ROWS; i++) {
    const int idx = top + i;
    const char* s = (idx >= 0 && idx < count) ? HOME_ITEMS[idx] : "";
    uiHomeDrawRow(i, s, (idx == selected));
  }
  uiHomeDrawBottomNavHome(0);
}

void uiHomeInit() { uiHomeDrawFull(0, 0); }
void uiHomeSetSelection(int selected, int top) { (void)selected; (void)top; }

void uiShowScreenTitle(const char* title) {
  tft.fillScreen(C_BG);
  drawHeader(title ? title : "", (WiFi.status() == WL_CONNECTED));
}

void uiShowPlaceholderBody(const char* line1, const char* line2) {
  setFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);
  tft.setCursor(12, 78);
  tft.print(line1 ? line1 : "");
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(12, 110);
  tft.print(line2 ? line2 : "");
}

// ================== INFO / WIFI ==================
void uiInfoScreenDraw(bool wifiOk, const char* line1, const char* line2, const char* line3) {
  tft.fillScreen(C_BG);
  drawHeader("INFO", wifiOk);

  // Accent left bar
  tft.fillRect(10, UI_HEADER_H + 6, 3, 96, C_ACCENT);

  setFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);
  tft.setCursor(20, 60); tft.print(line1 ? line1 : "");

  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(20, 86);  tft.print(line2 ? line2 : "");
  tft.setCursor(20, 108); tft.print(line3 ? line3 : "");

  // Divider + build label
  tft.drawFastHLine(10, 118, TFT_W - 20, C_DIV);
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(20, 132); tft.print("NAA TV v1.0  |  ESP32-S3");

  drawFooterBackHint("Swipe right / Click = Back");
}

void uiWifiScreenDraw(bool wifiOk, const char* ssid, const char* ip, int rssi) {
  tft.fillScreen(C_BG);
  drawHeader("WIFI", wifiOk);

  // Status pill
  uint16_t pillColor = wifiOk ? C_OK : C_BAD;
  tft.fillRoundRect(10, UI_HEADER_H + 5, 108, 22, 5, pillColor);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(22, UI_HEADER_H + 21);
  tft.print(wifiOk ? "CONNECTED" : "OFFLINE");

  // Accent left bar for data rows
  tft.fillRect(10, UI_HEADER_H + 34, 3, 76, C_ACCENT);

  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(20, UI_HEADER_H + 52); tft.print("SSID");
  tft.setTextColor(C_TEXT);
  tft.setCursor(70, UI_HEADER_H + 52); tft.print((ssid && ssid[0]) ? ssid : "-");

  tft.setTextColor(C_SUB);
  tft.setCursor(20, UI_HEADER_H + 72); tft.print("IP");
  tft.setTextColor(C_TEXT);
  tft.setCursor(70, UI_HEADER_H + 72); tft.print((ip && ip[0]) ? ip : "-");

  tft.setTextColor(C_SUB);
  tft.setCursor(20, UI_HEADER_H + 92); tft.print("RSSI");
  tft.setTextColor(C_TEXT);
  tft.setCursor(70, UI_HEADER_H + 92); tft.print(rssi); tft.print(" dBm");

  // Signal-strength bars (4 bars)
  int bars = 0;
  if      (rssi > -55) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  const int bBaseX = TFT_W - 58;
  const int bBaseY = UI_HEADER_H + 100;
  for (int i = 0; i < 4; i++) {
    int bH  = 6 + i * 6;
    int bX  = bBaseX + i * 13;
    int bY  = bBaseY - bH;
    tft.fillRect(bX, bY, 9, bH, (i < bars) ? C_ACCENT : C_DIV);
  }

  drawFooterBackHint("Swipe right / Click = Back");
}

// ================== TIME ==================
static char lastTimeStr[24] = {0};
static char lastDateStr[24] = {0};

void uiTimeScreenDrawFull(bool rtcOk) {
  lastTimeStr[0] = 0;
  lastDateStr[0] = 0;

  tft.fillScreen(C_BG);
  drawHeader("TIME", (WiFi.status() == WL_CONNECTED));

  // Decorative accent lines (top + bottom of content area)
  tft.fillRect(20, UI_HEADER_H + 4, TFT_W - 40, 2, C_ACCENT);
  tft.fillRect(20, TFT_H - UI_FOOTER_H - 8, TFT_W - 40, 2, C_ACCENT);

  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, y + 24);
  tft.print("Tap = Study Mode | Swipe = Back");

  if (!rtcOk) {
    drawCenteredBand(70, 46, "RTC NOT READY", &FreeSansBold12pt7b, C_BAD, C_BG);
    drawCenteredBand(120, 30, "Connect Wi-Fi", &FreeSans9pt7b, C_SUB, C_BG);
  }
}

void uiTimeScreenUpdate(const char* timeStr, const char* dateStr) {
  if (!timeStr) timeStr = "";
  if (!dateStr) dateStr = "";
  if (strcmp(timeStr, lastTimeStr) == 0 && strcmp(dateStr, lastDateStr) == 0) return;

  strncpy(lastTimeStr, timeStr, sizeof(lastTimeStr) - 1);
  lastTimeStr[sizeof(lastTimeStr) - 1] = 0;

  strncpy(lastDateStr, dateStr, sizeof(lastDateStr) - 1);
  lastDateStr[sizeof(lastDateStr) - 1] = 0;

  // Large time
  drawCenteredBand(44, 72, lastTimeStr, &FreeSansBold24pt7b, C_ACCENT, C_BG);
  // Thin separator between time and date
  tft.fillRect(30, 120, TFT_W - 60, 1, C_DIV);
  // Date row
  drawCenteredBand(122, 32, lastDateStr, &FreeSansBold12pt7b, C_TEXT, C_BG);
}

void uiTimeScreenDrawStudy(bool rtcOk) {
  lastTimeStr[0] = 0;
  lastDateStr[0] = 0;
  tft.fillScreen(C_BG);
  if (!rtcOk) {
    drawCenteredBand(70, 46, "RTC NOT READY", &FreeSansBold12pt7b, C_BAD, C_BG);
  }
}

void uiTimeScreenUpdateStudy(const char* timeStr, const char* dateStr) {
  if (!timeStr) timeStr = "";
  if (!dateStr) dateStr = "";
  if (strcmp(timeStr, lastTimeStr) == 0 && strcmp(dateStr, lastDateStr) == 0) return;

  strncpy(lastTimeStr, timeStr, sizeof(lastTimeStr) - 1);
  lastTimeStr[sizeof(lastTimeStr) - 1] = 0;

  strncpy(lastDateStr, dateStr, sizeof(lastDateStr) - 1);
  lastDateStr[sizeof(lastDateStr) - 1] = 0;

  // Extra large centered time for study mode
  drawCenteredBand((TFT_H / 2) - 40, 60, lastTimeStr, &FreeSansBold24pt7b, C_ACCENT, C_BG);
  // Add Day/Date right under it
  drawCenteredBand((TFT_H / 2) + 25, 30, lastDateStr, &FreeSansBold12pt7b, C_SUB, C_BG);
}

// ================== WEATHER ==================
static bool wxLastBusy = false;
static const int FOOT_Y = TFT_H - UI_FOOTER_H;
static const int BTN_H  = 26;

static void drawWeatherFooter(bool busy) {
  tft.fillRect(0, FOOT_Y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, FOOT_Y, TFT_W, C_DIV);

  const int cityX = 8, cityY = FOOT_Y + 4, cityW = 120, cityH = BTN_H;
  tft.fillRoundRect(cityX, cityY, cityW, cityH, 6, C_BTN_BG);
  tft.drawRoundRect(cityX, cityY, cityW, cityH, 6, C_BTN_BORDER);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_BTN_FG);
  // Centre "CITY" perfectly in button so it doesn't clip
  int16_t cx1, cy1; uint16_t cw, ch;
  tft.getTextBounds("CITY", 0, 0, &cx1, &cy1, &cw, &ch);
  tft.setCursor(cityX + (cityW - (int)cw) / 2, cityY + (cityH + (int)ch) / 2);
  tft.print("CITY");

  const int rx = TFT_W - 92, ry = FOOT_Y + 4, rw = 84, rh = BTN_H;
  uint16_t fill = busy ? ILI9341_DARKGREY : C_BTN_BG;
  tft.fillRoundRect(rx, ry, rw, rh, 6, fill);
  tft.drawRoundRect(rx, ry, rw, rh, 6, C_BTN_BORDER);
  const char* fetchLbl = busy ? "WAIT" : "FETCH";
  int16_t fx1, fy1; uint16_t fw, fh;
  tft.getTextBounds(fetchLbl, 0, 0, &fx1, &fy1, &fw, &fh);
  tft.setTextColor(C_BTN_FG);
  tft.setCursor(rx + (rw - (int)fw) / 2, ry + (rh + (int)fh) / 2);
  tft.print(fetchLbl);
}

// Helper: draw a single info tile with auto-scaling to prevent clip
static void drawWxTile(int x, int y, int w, int h,
                       const char* label, const char* value,
                       uint16_t accent) {
  tft.fillRoundRect(x, y, w, h, 5, C_TILE_BG);
  tft.drawRoundRect(x, y, w, h, 5, accent);

  // Label — top-left small font
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(x + 5, y + 11);
  tft.print(label);

  // Value — centred bold, scaled down if needed to fit
  if (value && value[0]) {
    setFont(&FreeSans9pt7b);
    int16_t vx1, vy1; uint16_t vw, vh;
    tft.getTextBounds(value, 0, 0, &vx1, &vy1, &vw, &vh);
    // If too wide for tile, fall back to tiny system font
    if ((int)vw > w - 8) {
      setFont(nullptr);
      tft.getTextBounds(value, 0, 0, &vx1, &vy1, &vw, &vh);
    }
    int tx = x + (w - (int)vw) / 2;
    if (tx < x + 3) tx = x + 3;
    tft.setTextColor(C_TEXT);
    tft.setCursor(tx, y + h - 6);
    tft.print(value);
  }
}

static void drawWeatherBody(const WeatherCache& wx, bool fullRedraw) {
  const int bodyY  = UI_HEADER_H;
  const int bodyH  = TFT_H - UI_HEADER_H - UI_FOOTER_H;

  if (fullRedraw) {
    tft.fillRect(0, bodyY, TFT_W, bodyH, C_BG);
  }

  // ── Row 1: City name + condition ────────────────────────
  {
    const int rowH = 28;
    tft.fillRect(0, bodyY, TFT_W, rowH, C_BG);

    // City name (left)
    setFont(&FreeSansBold12pt7b);
    tft.setTextColor(C_TEXT);
    int16_t nx1, ny1; uint16_t nw, nh;
    const char* cityName = wx.city[0] ? wx.city : "Unknown";
    tft.getTextBounds(cityName, 0, 0, &nx1, &ny1, &nw, &nh);
    tft.setCursor(10, bodyY + 22);
    tft.print(cityName);

    // Condition label (right)
    const char* cond = wxCodeLabel(wx.wcode);
    if (!cond || !cond[0]) cond = "---";
    
    // Use proper font first to avoid line overlap bug!
    setFont(&FreeSans9pt7b);
    tft.setTextColor(C_ACCENT);
    int16_t cx1, cy1; uint16_t cw, ch;
    tft.getTextBounds(cond, 0, 0, &cx1, &cy1, &cw, &ch);
    
    int condX = TFT_W - (int)cw - 8;
    int minX  = (int)nw + 18;
    if (condX < minX) {
      // FreeSans too wide, use tiny system font (top-down Y-coord)
      setFont(nullptr);
      tft.getTextBounds(cond, 0, 0, &cx1, &cy1, &cw, &ch);
      condX = TFT_W - (int)cw - 8;
      if (condX < minX) condX = minX;
      tft.setCursor(condX, bodyY + 12); // system font sits higher
    } else {
      tft.setCursor(condX, bodyY + 22); // FreeSans is baseline aligned
    }
    tft.print(cond);

    // Accent underline safely below everything
    tft.drawFastHLine(8, bodyY + 27, TFT_W - 16, C_ACCENT);
  }

  // ── Row 2: Temperature hero ─────────────────────────────
  {
    const int heroY = bodyY + 30;
    const int heroH = 54;
    tft.fillRect(0, heroY, TFT_W, heroH, C_BG);

    char tempNum[20];
    if (isnan(wx.tempC)) snprintf(tempNum, sizeof(tempNum), "--.-");
    else                  snprintf(tempNum, sizeof(tempNum), "%.1f", wx.tempC);

    // We calculate width to center everything nicely with custom °C
    setFont(&FreeSansBold24pt7b);
    int16_t tx1,ty1; uint16_t tw,th;
    tft.getTextBounds(tempNum, 0, 0, &tx1, &ty1, &tw, &th);
    
    int totalW = tw + 28; // add space for "C" and custom degree circle
    int startX = (TFT_W - totalW) / 2;
    int baseY = heroY + (int)th + 4; // Use true baseline

    tft.setTextColor(C_ACCENT);
    tft.setCursor(startX, baseY);
    tft.print(tempNum);

    // Draw manual degree symbol " ° " so it doesn't get clipped by GFX glitch
    int degX = startX + tw + 6;
    int degY = baseY - th + 9; 
    tft.drawCircle(degX, degY, 3, C_ACCENT);
    tft.drawCircle(degX, degY, 4, C_ACCENT);

    // Draw exact "C"
    setFont(&FreeSansBold12pt7b);
    tft.setCursor(degX + 8, baseY - 8);
    tft.print("C");

    // Feels-like subtitle below and to the right
    if (!isnan(wx.apparentTempC)) {
      char feelsNum[24];
      snprintf(feelsNum, sizeof(feelsNum), "Feels %.1f C", wx.apparentTempC);
      setFont(&FreeSans9pt7b);
      tft.setTextColor(C_SUB);
      int16_t fx1,fy1; uint16_t fw,fh;
      tft.getTextBounds(feelsNum, 0, 0, &fx1, &fy1, &fw, &fh);
      
      int fX = TFT_W - (int)fw - 8;
      // Make sure Feels doesn't overlap the "C"
      if (fX < (degX + 22)) fX = degX + 22; 
      
      tft.setCursor(fX, heroY + heroH - 4);
      tft.print(feelsNum);
    }
  }

  // ── Divider ─────────────────────────────────────────────
  if (fullRedraw) {
    tft.drawFastHLine(8, bodyY + 86, TFT_W - 16, C_DIV);
  }

  // ── Row 3: 3 large tiles centered perfectly in the remaining space
  {
    // Divider is at bodyY + 86. Lower section goes up to bodyH (which is 176).
    // Remaining space = 90 pixels. We center a 58px tall tile.
    const int tileY = bodyY + 104;
    const int tileH = 58;
    const int gap   = 4;
    const int tileW = (TFT_W - gap * 4) / 3;

    // Tile 1: Wind
    char wVal[16];
    if (isnan(wx.windKmh)) snprintf(wVal, sizeof(wVal), "---");
    else                   snprintf(wVal, sizeof(wVal), "%.1f km/h", wx.windKmh);
    drawWxTile(gap, tileY, tileW, tileH, "WIND", wVal, C_TILE_BORDER);

    // Tile 2: Humidity
    char hVal[12];
    if (wx.humidity < 0) snprintf(hVal, sizeof(hVal), "---");
    else                 snprintf(hVal, sizeof(hVal), "%d%%", wx.humidity);
    drawWxTile(gap + tileW + gap, tileY, tileW, tileH, "HUMIDITY", hVal, C_TILE_BORDER);

    // Tile 3: Updated Time
    const char* updStr = wx.updated[0] ? wx.updated : "Never";
    const char* updDisp = updStr;
    if (strlen(updStr) > 10 && updStr[4] == '-') updDisp = updStr + 11;
    drawWxTile(gap*3 + tileW*2, tileY, tileW, tileH, "UPDATED", updDisp, C_ACCENT);
  }
}

void uiWeatherScreenDrawFull(int cityIdx, const WeatherCache& wx, bool wifiOk, bool busy) {
  (void)cityIdx;
  wxLastBusy = busy;
  tft.fillScreen(C_BG);
  drawHeader("WEATHER", wifiOk);
  drawWeatherBody(wx, true);
  drawWeatherFooter(busy);
}

void uiWeatherScreenSetBusy(bool busy) {
  if (busy == wxLastBusy) return;
  wxLastBusy = busy;
  drawWeatherFooter(busy);
}

// Called when a fresh fetch completes — updates only the dynamic body
// without touching the header or footer, eliminating flicker.
void uiWeatherScreenUpdateData(int cityIdx, const WeatherCache& wx, bool wifiOk) {
  (void)cityIdx;
  // Redraw header to refresh WiFi indicator only
  drawHeader("WEATHER", wifiOk);
  // Body redraws each zone in-place (no full-screen clear)
  drawWeatherBody(wx, false);
}

// ================== QUOTES UI ==================
static bool qBusyLast = false;

static void drawQuotesFooter(bool busy) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  tft.fillRoundRect(8, y + 4, 90, 26, 6, busy ? C_DIV : C_BTN_BG);
  tft.drawRoundRect(8, y + 4, 90, 26, 6, C_BTN_BORDER);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_BTN_FG);
  // Center the label text vertically in the 26px button
  int16_t bx1, by1; uint16_t bw1, bh1;
  const char* btnLbl = busy ? "WAIT" : "NEW";
  tft.getTextBounds(btnLbl, 0, 0, &bx1, &by1, &bw1, &bh1);
  tft.setCursor(8 + (90 - (int)bw1) / 2, y + 4 + (26 + (int)bh1) / 2);
  tft.print(btnLbl);

  // Short hint text — constrained to remaining space (104..312)
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(108, y + 22);
  tft.print("Tap NEW | Swipe=Back");
}

static void drawQuotesBody(const QuoteData& q) {
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);

  // Small timestamp label — drawn ABOVE the box so it doesn't overlap
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, UI_HEADER_H + 9);
  tft.print(q.updated[0] ? q.updated : "No data");

  // Box starts well below the timestamp (timestamp bottom ≈ UI_HEADER_H + 16)
  int boxX = 6;
  int boxY = UI_HEADER_H + 18;
  int boxW = TFT_W - 12;
  int boxH = (TFT_H - UI_FOOTER_H - 4) - boxY;

  // Rounded accent border only — no decorative bars (they looked like blue hyphens)
  tft.drawRoundRect(boxX, boxY, boxW, boxH, 6, C_ACCENT);

  String text = String(q.quote[0] ? q.quote : "");
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.trim();
  while (text.indexOf("  ") >= 0) text.replace("  ", " ");

  setFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);

  // Text starts at boxY+22 (below the border at boxY)
  int cursorY = boxY + 22;
  const int maxY = boxY + boxH - 40; // Reserve space for author at bottom
  const int maxW = boxW - 18;

  String line;
  int i = 0;
  while (i < (int)text.length() && cursorY <= maxY) {
    while (i < (int)text.length() && text[i] == ' ') i++;
    if (i >= (int)text.length()) break;

    int j = i;
    while (j < (int)text.length() && text[j] != ' ') j++;
    String word = text.substring(i, j);
    i = j;

    String test = line.length() ? (line + " " + word) : word;

    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);

    if ((int)w <= maxW) line = test;
    else {
      tft.setCursor(boxX + 10, cursorY);
      tft.print(line);
      cursorY += 22;
      line = word;
    }
  }
  if (cursorY <= maxY && line.length()) {
    tft.setCursor(boxX + 10, cursorY);
    tft.print(line);
  }

  // Author - right aligned, clean style
  if (q.author[0]) {
    setFont(&FreeSans9pt7b);
    tft.setTextColor(C_ACCENT);
    String auth = String(q.author);
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(auth.c_str(), 0, 0, &x1, &y1, &w, &h);
    int authX = boxX + boxW - (int)w - 10;
    if (authX < boxX + 10) authX = boxX + 10;
    // Place author 14px above the bottom border so text doesn't clip
    tft.setCursor(authX, boxY + boxH - 14);
    tft.print(auth);
  }
}






void uiQuotesScreenDrawFull(bool wifiOk, bool busy, const QuoteData& q) {
  qBusyLast = busy;
  tft.fillScreen(C_BG);
  drawHeader("QUOTES", wifiOk);
  drawQuotesBody(q);
  drawQuotesFooter(busy);
}

void uiQuotesScreenSetBusy(bool busy) {
  if (busy == qBusyLast) return;
  qBusyLast = busy;
  drawQuotesFooter(busy);
}

void uiQuotesScreenUpdate(bool wifiOk, const QuoteData& q) {
  drawHeader("QUOTES", wifiOk);
  drawQuotesBody(q);
}

static void drawQuotesBodyStudy(const QuoteData& q) {
  tft.fillRect(0, 0, TFT_W, TFT_H, C_BG);

  int boxX = 10;
  int boxY = 10;
  int boxW = TFT_W - 20;
  int boxH = TFT_H - 20;

  tft.drawRect(boxX, boxY, boxW, boxH, ILI9341_DARKCYAN);

  String text = String(q.quote[0] ? q.quote : "");
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.trim();
  while (text.indexOf("  ") >= 0) text.replace("  ", " ");

  setFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);

  int cursorY = boxY + 34;
  const int maxY = boxY + boxH - 34;
  const int maxW = boxW - 16;

  String line;
  int i = 0;
  while (i < (int)text.length() && cursorY <= maxY) {
    while (i < (int)text.length() && text[i] == ' ') i++;
    if (i >= (int)text.length()) break;

    int j = i;
    while (j < (int)text.length() && text[j] != ' ') j++;
    String word = text.substring(i, j);
    i = j;

    String test = line.length() ? (line + " " + word) : word;

    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);

    if ((int)w <= maxW) line = test;
    else {
      tft.setCursor(boxX + 8, cursorY);
      tft.print(line);
      cursorY += 26;
      line = word;
    }
  }
  if (cursorY <= maxY && line.length()) {
    tft.setCursor(boxX + 8, cursorY);
    tft.print(line);
  }

  if (q.author[0]) {
    setFont(&FreeSansBold12pt7b);
    tft.setTextColor(C_ACCENT);
    int16_t x1, y1; uint16_t w, h;
    String auth = String("- ") + q.author;
    tft.getTextBounds(auth.c_str(), 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(boxX + boxW - (int)w - 8, boxY + boxH - 12);
    tft.print(auth);
  }
}

void uiQuotesScreenDrawStudy(const QuoteData& q) {
  tft.fillScreen(C_BG);
  drawQuotesBodyStudy(q);
}

void uiQuotesScreenUpdateStudy(const QuoteData& q) {
  drawQuotesBodyStudy(q);
}

// ================== NEWS UI ==================
static bool nBusyLast = false;

static void drawNewsFooter(bool busy) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  tft.fillRoundRect(8, y + 4, 110, 26, 6, busy ? C_DIV : C_BTN_BG);
  tft.drawRoundRect(8, y + 4, 110, 26, 6, C_BTN_BORDER);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_BTN_FG);
  // Vertically center button text inside the 26px button height
  int16_t bx1, by1; uint16_t bw1, bh1;
  const char* btnLbl = busy ? "WAIT" : "FETCH";
  tft.getTextBounds(btnLbl, 0, 0, &bx1, &by1, &bw1, &bh1);
  tft.setCursor(8 + (110 - (int)bw1) / 2, y + 4 + (26 + (int)bh1) / 2);
  tft.print(btnLbl);

  // Short hint — limit to remaining width (126..319 = 193px)
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(130, y + 22);
  tft.print("Next:Swipe | Back:Swipe");
}

static bool wrapPrintInBox(const String& text, int boxX, int boxY, int boxW, int boxH,
                           const GFXfont* font, int lineStep, bool addEllipsisIfCut) {
  setFont(font);
  tft.setTextWrap(false); // CRITICAL: Stop Adafruit GFX from silently wrapping and breaking our manual baseline math
  tft.setTextColor(C_TEXT);

  const int maxW = boxW - 12;
  int cursorY = boxY + 24;
  const int maxY = boxY + boxH - 12;

  String t = text;
  String line;
  int i = 0;

  while (i < (int)t.length() && cursorY <= maxY) {
    while (i < (int)t.length() && t[i] == ' ') i++;
    if (i >= (int)t.length()) break;

    int j = i;
    while (j < (int)t.length() && t[j] != ' ') j++;
    String word = t.substring(i, j);
    i = j;

    String test = line.length() ? (line + " " + word) : word;

    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);

    if ((int)w <= maxW) {
      line = test;
    } else {
      if (line.length()) {
        tft.setCursor(boxX + 6, cursorY);
        tft.print(line);
        cursorY += lineStep;
      }
      line = word;

      if (cursorY > maxY) break;
    }
  }

  bool remains = false;
  for (int k = i; k < (int)t.length(); k++) {
    if (t[k] != ' ') { remains = true; break; }
  }

  if (cursorY <= maxY && line.length()) {
    if (remains && addEllipsisIfCut) {
      while (line.length() > 0) {
        String test = line + "...";
        int16_t x1, y1; uint16_t w, h;
        tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);
        if ((int)w <= maxW) { line = test; break; }
        line.remove(line.length() - 1);
      }
    }
    tft.setCursor(boxX + 6, cursorY);
    tft.print(line);
  }

  return !remains;
}

static void drawNewsBody(const NewsData& n) {
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);

  // Source Badge
  const char* src = (n.channel == 1) ? "GLOBAL NEWS" : "INDIA NEWS";
  uint16_t srcColor = (n.channel == 1) ? 0x001F : ILI9341_ORANGE; // Blue for Global, Orange for India
  
  setFont(&FreeSans9pt7b);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(src, 0, 0, &x1, &y1, &w, &h);
  tft.fillRoundRect(8, UI_HEADER_H + 4, w + 10, h + 6, 4, srcColor);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(13, UI_HEADER_H + 18);
  tft.print(src);

  // Category info
  const char* catNames[] = { "TOP HEADLINES", "TECHNOLOGY", "BUSINESS", "SPORTS", "ENTERTAINMENT", "WORLD", "SCIENCE", "HEALTH" };
  const char* cat = (n.category >= 0 && n.category < 8) ? catNames[n.category] : "TOP";

  tft.setTextColor(C_ACCENT);
  tft.setCursor(w + 25, UI_HEADER_H + 18);
  tft.print(cat);

  // Meta info line
  tft.setFont(nullptr); // Small system font
  tft.setTextColor(C_SUB);
  tft.setCursor(10, UI_HEADER_H + 28);
  tft.print("Item ");
  tft.print(n.index + 1);
  tft.print("/");
  tft.print(n.count);
  tft.print("  -  ");
  tft.print(n.updated[0] ? n.updated : "Never updated");

  int boxX = 10;
  int boxY = UI_HEADER_H + 36;
  int boxW = TFT_W - 20;
  int boxH = (TFT_H - UI_FOOTER_H - 10) - boxY;

  tft.drawRect(boxX, boxY, boxW, boxH, ILI9341_DARKCYAN);

  const char* title = (n.count > 0) ? n.title[n.index] : "No news cached.";
  String text = String(title ? title : "");
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.trim();
  while (text.indexOf("  ") >= 0) text.replace("  ", " ");

  // "READ MORE" tap hint — only if description is available
  const char* desc = (n.count > 0) ? n.desc[n.index] : "";
  bool hasDesc = (desc && desc[0]);

  // Reserve the bottom ~28px so text wrapping NEVER encroaches into the pill area
  int textLimitH = hasDesc ? boxH - 28 : boxH;

  // Use 12pt if it fits, note: lineStep increased to 30 (from 22) because FreeSans12pt7b ascenders are tall
  bool fit = wrapPrintInBox(text, boxX, boxY, boxW, textLimitH, &FreeSans12pt7b, 30, true);

  if (!fit) {
    tft.fillRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, C_BG);
    tft.drawRect(boxX, boxY, boxW, boxH, ILI9341_DARKCYAN);
    // If it doesn't fit, step down to 9pt font
    wrapPrintInBox(text, boxX, boxY, boxW, textLimitH, &FreeSans9pt7b, 20, true);
  }

  if (hasDesc) {
    // Pill sits in bottom-right corner of the article box, 3px inset
    const int pillW = 108;
    const int pillH = 24;
    const int pillX = boxX + boxW - pillW - 3;
    const int pillY = boxY + boxH - pillH - 3;
    tft.fillRoundRect(pillX, pillY, pillW, pillH, 6, C_ACCENT);
    tft.drawRoundRect(pillX, pillY, pillW, pillH, 6, C_BG); // subtle dark border
    // Center label in pill — system font is 6x8px drawn from top-left (size 1)
    setFont(nullptr);
    tft.setTextSize(1);
    tft.setTextColor(C_BG);
    const char* lbl = "READ MORE >>";
    int16_t bx, by; uint16_t bw, bh;
    tft.getTextBounds(lbl, 0, 0, &bx, &by, &bw, &bh);
    tft.setCursor(pillX + (pillW - bw) / 2, pillY + (pillH - bh) / 2);
    tft.print(lbl);
  }
}

void uiNewsScreenDrawFull(bool wifiOk, bool busy, const NewsData& n) {
  nBusyLast = busy;
  tft.fillScreen(C_BG);
  drawHeader("NEWS", wifiOk);
  drawNewsBody(n);
  drawNewsFooter(busy);
}

void uiNewsScreenSetBusy(bool busy) {
  if (busy == nBusyLast) return;
  nBusyLast = busy;
  drawNewsFooter(busy);
}

void uiNewsScreenUpdate(bool wifiOk, const NewsData& n) {
  drawHeader("NEWS", wifiOk);
  drawNewsBody(n);
}

static void drawNewsBodyStudy(const NewsData& n, int scrollOffset) {
  // ---- Full screen article reader ----
  tft.fillScreen(C_BG);

  const int PAD   = 8;
  const int W     = TFT_W - PAD * 2;

  // ---- SOURCE / INDEX bar ----
  // Note: setFont(nullptr) = built-in 6x8 font, cursor is TOP-LEFT of chars.
  // At cursor Y=3 chars span Y=3..11, so divider at Y=20 is well below them.
  const char* src = (n.channel == 1) ? "GLOBAL" : "INDIA";
  const char* catNames[] = { "TOP", "TECH", "BIZ", "SPORTS", "ENTERTAIN", "WORLD", "SCI", "HEALTH" };
  const char* cat = (n.category >= 0 && n.category < 8) ? catNames[n.category] : "TOP";
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  char badge[32];
  snprintf(badge, sizeof(badge), "%s  %s  %d/%d", src, cat, n.index + 1, n.count);
  tft.setCursor(PAD, 3);      // top of 8px chars at Y=3, bottom at Y=11
  tft.print(badge);
  tft.drawFastHLine(PAD, 20, W, C_DIV); // divider 9px below text bottom

  // ---- HEADLINE (bold, wrapped, up to 3 lines) ----
  tft.setTextWrap(false); // CRITICAL: Disable GFX wrapping so our custom line breaker measures & prints strictly

  const char* title = (n.count > 0) ? n.title[n.index] : "No news cached.";
  String titleStr = String(title ? title : "");
  titleStr.replace("\r", " "); titleStr.replace("\n", " "); titleStr.trim();
  while (titleStr.indexOf("  ") >= 0) titleStr.replace("  ", " ");

  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_ACCENT);

  // Wrap headline into lines (max 3)
  const int maxTitleLines = 3;
  const int titleLineH    = 32; // FreeSansBold12pt7b needs at least ~28-30px line height
  String titleLines[maxTitleLines];
  int numTitleLines = 0;
  {
    String line;
    int i = 0;
    while (i < (int)titleStr.length() && numTitleLines < maxTitleLines) {
      while (i < (int)titleStr.length() && titleStr[i] == ' ') i++;
      if (i >= (int)titleStr.length()) break;
      int j = i;
      while (j < (int)titleStr.length() && titleStr[j] != ' ') j++;
      String word = titleStr.substring(i, j);
      i = j;
      String test = line.length() ? (line + " " + word) : word;
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);
      if ((int)w <= W - 4) {
        line = test;
      } else {
        if (line.length()) { titleLines[numTitleLines++] = line; }
        line = word;
      }
    }
    if (line.length() && numTitleLines < maxTitleLines) titleLines[numTitleLines++] = line;
  }

  int titleY = 44; // baseline — ascenders reach Y≈27, divider at Y=20, 7px clear gap
  for (int i = 0; i < numTitleLines; i++) {
    tft.setCursor(PAD, titleY + i * titleLineH);
    tft.print(titleLines[i]);
  }

  // Divider after headline, placed relative to the baseline of the *last* headline line
  int divY = titleY + (numTitleLines > 0 ? (numTitleLines - 1) * titleLineH + 12 : 12);
  tft.drawFastHLine(PAD, divY, W, C_ACCENT);

  // ---- DESCRIPTION (normal font, scrollable) ----
  tft.setTextWrap(false);

  const char* desc = (n.count > 0) ? n.desc[n.index] : "";
  String descStr = String(desc ? desc : "");
  descStr.replace("\r", " "); descStr.replace("\n", " "); descStr.trim();
  while (descStr.indexOf("  ") >= 0) descStr.replace("  ", " ");

  const int descFont9LineH = 18;
  const int descAreaY0 = divY + 6;
  const int descAreaH  = TFT_H - descAreaY0 - 18; // leave bottom for scroll hint

  // Build all word-wrapped description lines into a small fixed array
  static const int MAX_DESC_LINES = 60;
  static String descLineArr[MAX_DESC_LINES];
  int totalDescLines = 0;

  if (descStr.length() > 0) {
    setFont(&FreeSans9pt7b);
    String line;
    int i = 0;
    while (i < (int)descStr.length() && totalDescLines < MAX_DESC_LINES) {
      while (i < (int)descStr.length() && descStr[i] == ' ') i++;
      if (i >= (int)descStr.length()) break;
      int j = i;
      while (j < (int)descStr.length() && descStr[j] != ' ') j++;
      String word = descStr.substring(i, j);
      i = j;
      String test = line.length() ? (line + " " + word) : word;
      int16_t x1, y1; uint16_t w, h;
      tft.getTextBounds(test, 0, 0, &x1, &y1, &w, &h);
      if ((int)w <= W - 4) {
        line = test;
      } else {
        if (line.length()) { descLineArr[totalDescLines++] = line; }
        line = word;
      }
    }
    if (line.length() && totalDescLines < MAX_DESC_LINES) descLineArr[totalDescLines++] = line;
  }

  // Clamp scrollOffset
  const int visLines = descAreaH / descFont9LineH;
  int maxScroll = totalDescLines - visLines;
  if (maxScroll < 0) maxScroll = 0;
  int sOff = scrollOffset;
  if (sOff < 0) sOff = 0;
  if (sOff > maxScroll) sOff = maxScroll;

  // Draw visible description lines
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_TEXT);
  for (int li = sOff; li < totalDescLines && (li - sOff) < visLines; li++) {
    int cy = descAreaY0 + (li - sOff) * descFont9LineH + 14;
    tft.setCursor(PAD, cy);
    tft.print(descLineArr[li]);
  }

  // If no description at all
  if (totalDescLines == 0) {
    setFont(&FreeSans9pt7b);
    tft.setTextColor(C_SUB);
    tft.setCursor(PAD, descAreaY0 + 20);
    tft.print("No description available.");
  }

  // ---- SCROLL INDICATORS ----
  setFont(nullptr);
  tft.setTextColor(C_ACCENT);
  // Up arrow if scrolled down
  if (sOff > 0) {
    tft.setCursor(TFT_W / 2 - 3, descAreaY0 + 6);
    tft.print("^");
  }
  // Down arrow / count indicator at bottom
  const int bottomY = TFT_H - 12;
  tft.fillRect(0, bottomY - 10, TFT_W, 14, C_BG);
  tft.setTextColor(C_SUB);
  if (sOff < maxScroll) {
    tft.setCursor(TFT_W / 2 - 20, bottomY);
    tft.print("v Swipe UP");
  } else {
    // At bottom — show back hint
    tft.setCursor(PAD, bottomY);
    tft.print("Tap: next article | Swipe R: back");
  }
}

void uiNewsScreenDrawStudy(const NewsData& n, int scrollOffset) {
  drawNewsBodyStudy(n, scrollOffset);
}

void uiNewsScreenUpdateStudy(const NewsData& n, int scrollOffset) {
  drawNewsBodyStudy(n, scrollOffset);
}

static void drawFooterBackHint(const char* s) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, y + 26);
  tft.print(s);
}

void uiNewsChannelsMenuDrawFull(int sel, int top, const char* const* items, int count) {
  drawHeader("NEWS CHANNELS", (WiFi.status() == WL_CONNECTED));
  const int by0 = UI_HEADER_H;
  const int bh = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  tft.fillRect(0, by0, TFT_W, bh, C_BG);

  // Two large tiles for 2 channels
  const int tileH = (bh / 2) - 10;
  const int tileW = TFT_W - 20;

  for (int i = 0; i < 2; i++) {
    int y = by0 + 5 + i * (tileH + 10);
    bool selected = (i == sel);
    uint16_t bg = selected ? C_SEL_BG : C_TILE_BG;
    uint16_t fg = selected ? C_ACCENT : C_TILE_FG;
    uint16_t border = selected ? C_ACCENT : C_TILE_BORDER;

    tft.fillRoundRect(10, y, tileW, tileH, 8, bg);
    tft.drawRoundRect(10, y, tileW, tileH, 8, border);

    setFont(&FreeSansBold12pt7b);
    tft.setTextColor(fg);
    
    const char* label = (i < count) ? items[i] : "";
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(10 + (tileW - w) / 2, y + (tileH + h) / 2);
    tft.print(label);
  }

  drawFooterBackHint("Select Channel / Back");
}

void uiNewsCategoriesMenuDrawFull(int sel, int top, const char* const* items, int count, const char* chanName) {
  char head[32];
  snprintf(head, sizeof(head), "%s NEWS", chanName);
  drawHeader(head, (WiFi.status() == WL_CONNECTED));
  
  const int by0 = UI_HEADER_H;
  const int bh = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  tft.fillRect(0, by0, TFT_W, bh, C_BG);

  // 2x4 Grid of tiles
  const int cols = 2;
  const int rows = 4;
  const int tileW = (TFT_W / cols) - 8;
  const int tileH = (bh / rows) - 8;

  for (int i = 0; i < count; i++) {
    int r = i / cols;
    int c = i % cols;
    int x = 4 + c * (tileW + 8);
    int y = by0 + 4 + r * (tileH + 8);
    
    bool selected = (i == sel);
    uint16_t bg = selected ? C_SEL_BG : C_TILE_BG;
    uint16_t fg = selected ? C_ACCENT : C_TILE_FG;
    uint16_t border = selected ? C_ACCENT : C_TILE_BORDER;

    tft.fillRoundRect(x, y, tileW, tileH, 6, bg);
    tft.drawRoundRect(x, y, tileW, tileH, 6, border);

    // Use consistent font for all tiles in the grid
    const char* label = items[i];
    setFont(&FreeSans9pt7b);
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    
    tft.setTextColor(fg);
    tft.setCursor(x + (tileW - w) / 2, y + (tileH + h) / 2);
    tft.print(label);
  }

  drawFooterBackHint("Select Category / Back");
}

void uiToolsMenuDrawRow(int rowIndex, const char* label, bool selected) {
  const int rowH = 28;
  const int y = UI_HEADER_H + rowIndex * rowH;
  const uint16_t bg = selected ? C_SEL_BG : C_BG;
  const uint16_t fg = selected ? C_ACCENT : C_TEXT;
  tft.fillRect(0, y, TFT_W, rowH, bg);
  setFont(&FreeSans12pt7b);
  tft.setTextColor(fg);
  tft.setCursor(14, y + 22);
  tft.print(label ? label : "");
  tft.drawFastHLine(0, y + rowH - 1, TFT_W, selected ? ILI9341_BLUE : C_DIV);
}

void uiToolsMenuDrawFull(int sel, int top, const char* const* items, int count) {
  drawHeader("TOOLS", (WiFi.status() == WL_CONNECTED));
  // Clear body area to remove ghosting from Home screen or Title screen
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
  
  const int rowH = 28;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int rows = bodyH / rowH;
  for (int i = 0; i < rows; i++) {
    int idx = top + i;
    const char* s = (idx >= 0 && idx < count) ? items[idx] : "";
    uiToolsMenuDrawRow(i, s, (idx == sel));
  }
  drawFooterBackHint("Swipe back / Click = Back");
}

void uiGamesMenuDrawRow(int rowIndex, const char* label, bool selected) {
  const int rowH = 28;
  const int y = UI_HEADER_H + rowIndex * rowH;
  const uint16_t bg = selected ? C_SEL_BG : C_BG;
  const uint16_t fg = selected ? C_ACCENT : C_TEXT;
  tft.fillRect(0, y, TFT_W, rowH, bg);
  setFont(&FreeSans12pt7b);
  tft.setTextColor(fg);
  tft.setCursor(14, y + 22);
  tft.print(label ? label : "");
  tft.drawFastHLine(0, y + rowH - 1, TFT_W, selected ? ILI9341_BLUE : C_DIV);
}

void uiGamesMenuDrawFull(int sel, int top, const char* const* items, int count) {
  drawHeader("GAMES", (WiFi.status() == WL_CONNECTED));
  // Clear body area
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
  
  const int rowH = 28;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int rows = bodyH / rowH;
  for (int i = 0; i < rows; i++) {
    int idx = top + i;
    const char* s = (idx >= 0 && idx < count) ? items[idx] : "";
    uiGamesMenuDrawRow(i, s, (idx == sel));
  }
  drawFooterBackHint("Swipe back / Click = Back");
}

static void drawUnitsRefreshFooter(bool unitsBusy) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  const int btnH = 26;
  // UNITS button
  const int bx1 = 8, by1 = y + 4, bw1 = 120;
  tft.fillRoundRect(bx1, by1, bw1, btnH, 6, ILI9341_DARKCYAN);
  tft.drawRoundRect(bx1, by1, bw1, btnH, 6, ILI9341_CYAN);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_BLACK);
  int16_t ux1, uy1; uint16_t uw, uh;
  tft.getTextBounds("UNITS", 0, 0, &ux1, &uy1, &uw, &uh);
  tft.setCursor(bx1 + (bw1 - (int)uw) / 2, by1 + (btnH + (int)uh) / 2);
  tft.print("UNITS");

  // REFRESH button
  const int bx2 = TFT_W - 92, by2 = y + 4, bw2 = 84;
  tft.fillRoundRect(bx2, by2, bw2, btnH, 6, unitsBusy ? ILI9341_DARKGREY : ILI9341_DARKCYAN);
  tft.drawRoundRect(bx2, by2, bw2, btnH, 6, ILI9341_CYAN);
  tft.setTextColor(ILI9341_BLACK);
  int16_t rx1, ry1; uint16_t rw, rh;
  tft.getTextBounds("REFRESH", 0, 0, &rx1, &ry1, &rw, &rh);
  tft.setCursor(bx2 + (bw2 - (int)rw) / 2, by2 + (btnH + (int)rh) / 2);
  tft.print("REFRESH");
}

static void drawTempPressBody(bool rtcOk, float tC, float hPct, float pHpa, bool useF, bool useInHg, bool fullRedraw) {
  if (fullRedraw) {
    tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
  }

  float tDisp = tC;
  char tUnit[4] = "C";
  if (useF && !isnan(tC)) { tDisp = tC * 9.0f / 5.0f + 32.0f; strcpy(tUnit, "F"); }
  float pDisp = pHpa;
  char pUnit[6] = "hPa";
  if (useInHg && !isnan(pHpa)) { pDisp = pHpa * 0.0295299830714f; strcpy(pUnit, "inHg"); }

  // Large temperature hero
  char tempStr[20];
  if (isnan(tDisp)) snprintf(tempStr, sizeof(tempStr), "--.- %s", tUnit);
  else snprintf(tempStr, sizeof(tempStr), "%.1f %s", tDisp, tUnit);
  drawCenteredBand(UI_HEADER_H + 2, 70, tempStr, &FreeSansBold24pt7b, C_ACCENT, C_BG);

  if (fullRedraw) {
    // Divider
    tft.drawFastHLine(10, UI_HEADER_H + 74, TFT_W - 20, C_DIV);
  }

  // Single centered pressure tile (BMP280 has no humidity)
  const int tileY  = UI_HEADER_H + 82;
  const int tileH  = 50;
  const int tileW  = TFT_W - 30;  // full-width, centered
  const int tileX  = 15;

  tft.fillRoundRect(tileX, tileY, tileW, tileH, 6, C_TILE_BG);
  tft.drawRoundRect(tileX, tileY, tileW, tileH, 6, C_ACCENT); // accent border

  // Label: small font at top
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(tileX + 12, tileY + 13);
  tft.print("PRESSURE");

  // Value: large centered font
  char pressBuf[20];
  if (isnan(pDisp)) snprintf(pressBuf, sizeof(pressBuf), "--.- %s", pUnit);
  else snprintf(pressBuf, sizeof(pressBuf), "%.1f %s", pDisp, pUnit);
  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT);
  int16_t px1, py1; uint16_t pw, ph;
  tft.getTextBounds(pressBuf, 0, 0, &px1, &py1, &pw, &ph);
  tft.setCursor(tileX + (tileW - pw) / 2, tileY + 42);
  tft.print(pressBuf);

  if (fullRedraw) {
    // Status row
    setFont(nullptr);
    tft.setTextColor(C_SUB);
    tft.setCursor(12, UI_HEADER_H + 148);
    // Show live indicator or offline status
    bool liveData = (!isnan(tC) && !isnan(pHpa));
    if (liveData)
      tft.print("BMP280 LIVE  |  RTC: " + String(rtcOk ? "OK" : "OFFLINE"));
    else
      tft.print(String(rtcOk ? "RTC: OK" : "RTC: OFFLINE") + "  |  Warming up...");
  }
}

void uiTempPressDrawFull(bool rtcOk, float tC, float hPct, float pHpa, bool useF, bool useInHg) {
  tft.fillScreen(C_BG);
  drawHeader("TEMP & PRESSURE", (WiFi.status() == WL_CONNECTED));
  drawTempPressBody(rtcOk, tC, hPct, pHpa, useF, useInHg, true);
  drawUnitsRefreshFooter(false);
}

void uiTempPressUpdate(bool rtcOk, float tC, float hPct, float pHpa, bool useF, bool useInHg) {
  drawTempPressBody(rtcOk, tC, hPct, pHpa, useF, useInHg, false);
}

// Update ONLY the small status line at the bottom of the Temp & Pressure screen
// (used for warm-up countdown messages without redrawing the whole body)
void uiTempPressSetStatus(const char* fmt, ...) {
  const int statusY = UI_HEADER_H + 148;
  // Clear the row
  tft.fillRect(0, statusY - 10, TFT_W, 18, C_BG);
  // Format string
  char buf[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  setFont(nullptr);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setCursor(12, statusY);
  tft.print(buf);
}

void uiSettingsDrawFull(int sel, int top, const char* v0, const char* v1, const char* v2, const char* v3, const char* v4, const char* v5, const char* v6, const char* v7, const char* v8, const char* v9) {
  tft.fillScreen(C_BG);
  drawHeader("SETTINGS", (WiFi.status() == WL_CONNECTED));
  const int rowH = 28;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int maxRows = bodyH / rowH;
  const char* labels[10] = { "Brightness", "Theme", "Accent", "Font", "Temp Units", "Pressure Units", "Timeout", "Pomo Work", "Pomo Break", "Temp Offset" };
  const char* vals[10]   = { v0?v0:"", v1?v1:"", v2?v2:"", v3?v3:"", v4?v4:"", v5?v5:"", v6?v6:"", v7?v7:"", v8?v8:"", v9?v9:"" };
  for (int i = 0; i < maxRows; i++) {
    int idx = top + i;
    if (idx >= 10) break;
    int y = UI_HEADER_H + i * rowH;
    bool selected = (idx == sel);
    uint16_t bg = selected ? C_SEL_BG : C_BG;
    uint16_t fg = selected ? C_ACCENT : C_TEXT;
    tft.fillRect(0, y, TFT_W, rowH, bg);

    // Accent left bar on selected row
    if (selected) tft.fillRect(0, y, 3, rowH, C_ACCENT);

    setFont(&FreeSans9pt7b);
    tft.setTextColor(fg);
    tft.setCursor(10, y + 20);
    tft.print(labels[idx]);

    // Value right-aligned
    tft.setTextColor(selected ? C_TEXT : C_SUB);
    int16_t vx1, vy1; uint16_t vw, vh;
    tft.getTextBounds(vals[idx], 0, 0, &vx1, &vy1, &vw, &vh);
    int valX = TFT_W - (int)vw - (selected ? 22 : 10);
    tft.setCursor(valX, y + 20);
    tft.print(vals[idx]);

    // Chevron ">" on selected row
    if (selected) {
      setFont(nullptr);
      tft.setTextColor(C_ACCENT);
      tft.setCursor(TFT_W - 14, y + 18);
      tft.print(">");
    }
    tft.drawFastHLine(0, y + rowH - 1, TFT_W, selected ? C_ACCENT : C_DIV);
  }
  drawFooterBackHint("Scroll: Swipe | Back: R-Swipe");
}


void uiFormatOptionsDrawFull(int sel, const char** items, int count) {
  tft.fillScreen(C_BG);
  drawHeader("FORMAT OPTIONS", (WiFi.status() == WL_CONNECTED));
  const int rowH = 34;
  const int bodyY = UI_HEADER_H + 10;
  
  for (int i = 0; i < count; i++) {
    int y = bodyY + i * rowH;
    uint16_t bg = (i == sel) ? C_SEL_BG : C_BG;
    uint16_t fg = (i == sel) ? C_ACCENT : C_TEXT;
    
    tft.fillRect(0, y, TFT_W, rowH, bg);
    setFont(&FreeSans9pt7b);
    tft.setTextColor(fg);
    tft.setCursor(14, y + 24);
    tft.print("FORMAT ");
    tft.print(items[i]);
    
    tft.drawFastHLine(0, y + rowH - 1, TFT_W, (i == sel) ? C_ACCENT : C_DIV);
  }
  drawFooterBackHint("Tap: Format | R-Swipe: Back");
}

void uiFormatOptionsRedrawSelection(int sel, const char** items, int count) {
  const int rowH = 34;
  const int bodyY = UI_HEADER_H + 10;
  for (int i = 0; i < count; i++) {
    int y = bodyY + i * rowH;
    uint16_t bg = (i == sel) ? C_SEL_BG : C_BG;
    uint16_t fg = (i == sel) ? C_ACCENT : C_TEXT;
    tft.fillRect(0, y, TFT_W, rowH, bg);
    setFont(&FreeSans9pt7b);
    tft.setTextColor(fg);
    tft.setCursor(14, y + 24);
    tft.print("FORMAT ");
    tft.print(items[i]);
    tft.drawFastHLine(0, y + rowH - 1, TFT_W, (i == sel) ? C_ACCENT : C_DIV);
  }
}

void uiCalcDrawFull(const char* expr, const char* result) {
  tft.fillScreen(C_BG);
  drawHeader("CALCULATOR", (WiFi.status() == WL_CONNECTED));
  
  const int by0 = uiBodyY0();
  const int bh = uiBodyH();
  
  // Display area
  int dy = by0 + 6;
  int dH = 68;
  tft.fillRect(6, dy, TFT_W - 12, dH, C_BG);
  tft.drawRect(6, dy, TFT_W - 12, dH, C_DIV);
  
  // ENHANCED: Show history indicator if viewing history
  if (historyViewIndex >= 0) {
    setFont(nullptr);
    tft.setTextColor(C_SUB);
    tft.setCursor(10, dy + 12);
    tft.printf("HISTORY %d/%d", historyViewIndex + 1, calcHistoryCount);
  }
  
  // Expression (Top right)
  const char* exprStr = expr ? expr : "";
  setFont(&FreeSans12pt7b);
  tft.setTextColor(calcError ? ILI9341_RED : C_TEXT); // ENHANCED: Red text for errors
  int16_t bx, by; uint16_t bw, bh_txt;
  tft.getTextBounds(exprStr, 0, 0, &bx, &by, &bw, &bh_txt);
  int ex = TFT_W - 14 - (int)bw;
  if (ex < 14) ex = 14;
  tft.setCursor(ex, dy + 28);
  tft.print(exprStr);
  
  // Result (Bottom right)
  const char* resStr = result ? result : "0";
  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(calcError ? ILI9341_RED : C_ACCENT); // ENHANCED: Red text for errors
  tft.getTextBounds(resStr, 0, 0, &bx, &by, &bw, &bh_txt);
  int rx = TFT_W - 14 - (int)bw;
  if (rx < 14) rx = 14;
  tft.setCursor(rx, dy + 58);
  tft.print(resStr);
  
  // ENHANCED: Updated grid for 5x4 layout
  const int gridY = by0 + 80;
  const int cellW = TFT_W / 4;
  const int availH = bh - 80;
  const int cellH = availH / 5; // ENHANCED: Changed from 4 to 5 rows
  
  // ENHANCED: Updated keys array with ASCII characters for better visibility
  const char* keys[5][4] = {
    { "C",  "<",  "%",  "/" },  // ENHANCED: Use "<" instead of "←" for backspace
    { "7",  "8",  "9",  "*" },
    { "4",  "5",  "6",  "-" },
    { "1",  "2",  "3",  "+" },
    { "0",  ".",  "=",  "+/-" } // ENHANCED: Use "+/-" instead of "±"
  };
  
  for (int r = 0; r < 5; r++) { // ENHANCED: Changed from 4 to 5 rows
    for (int c = 0; c < 4; c++) {
      int x = c * cellW;
      int y = gridY + r * cellH;
      
      const char* s = keys[r][c];
      char c_first = s[0];
      bool isDigit = (c_first >= '0' && c_first <= '9') || c_first == '.';
      bool isOp = !isDigit && strcmp(s, "=") != 0 && strcmp(s, "C") != 0 && strcmp(s, "<") != 0;
      bool isEq = strcmp(s, "=") == 0;
      bool isSpecial = strcmp(s, "C") == 0 || strcmp(s, "<") == 0; // ENHANCED: Special button coloring
      
      // ENHANCED: Clear button background first
      tft.fillRect(x + 3, y + 3, cellW - 6, cellH - 6, C_BG);
      
      // ENHANCED: Better button border
      tft.drawRect(x + 2, y + 2, cellW - 4, cellH - 4, C_DIV);
      
      // ENHANCED: Better text color
      if (isSpecial) tft.setTextColor(ILI9341_RED); 
      else if (isOp) tft.setTextColor(C_ACCENT);
      else if (isEq) tft.setTextColor(ILI9341_ORANGE);
      else tft.setTextColor(C_TEXT);
      
      // ENHANCED: Use appropriate font size based on text length
      if (strlen(s) >= 3) {
        setFont(&FreeSans9pt7b); // Small font for "+/-"
      } else {
        setFont(&FreeSansBold12pt7b); // Large font for single/double characters
      }
      
      // ENHANCED: Get text bounds for proper centering
      int16_t x1, y1;
      uint16_t w, h;
      tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
      
      // ENHANCED: Calculate center position with better algorithm
      int textX = x + (cellW - w) / 2;
      int textY = y + (cellH / 2) + (h / 2) - 1;
      
      // ENHANCED: Ensure text fits within button with margins
      int margin = 6;
      if (textX < x + margin) textX = x + margin;
      if (textX + w > x + cellW - margin) textX = x + cellW - margin - w;
      
      tft.setCursor(textX, textY);
      tft.print(s);
    }
  }
  
  // Footer hint - placed safely in footer area, not overlapping buttons
  const int calcFooterY = TFT_H - UI_FOOTER_H;
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(4, calcFooterY + 26);
  tft.print("Enc:History | Long:Back");
}

void uiStopwatchUpdateTime(uint32_t ms, bool running) {
  uint32_t t = ms;
  uint32_t centi = (t / 10) % 100;
  uint32_t secs = (t / 1000) % 60;
  uint32_t mins = (t / 60000);
  char buf[24];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", (unsigned long)mins, (unsigned long)secs, (unsigned long)centi);
  setFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_ACCENT);
  int bandY = UI_HEADER_H + 10;
  int bandH = 80;
  tft.fillRect(0, bandY, TFT_W, bandH, C_BG);
  int x = xCentered(buf, &FreeSansBold24pt7b);
  int base = baselineCenteredBand(bandY, bandH, buf, &FreeSansBold24pt7b);
  tft.setCursor(x, base);
  tft.print(buf);
}

void uiStopwatchDrawFull(uint32_t ms, bool running) {
  tft.fillScreen(C_BG);
  drawHeader("STOPWATCH", (WiFi.status() == WL_CONNECTED));

  // Running-state indicator dot under header
  tft.fillCircle(TFT_W / 2, UI_HEADER_H + 12, 5, running ? C_OK : C_DIV);

  uiStopwatchUpdateTime(ms, running);

  // Accent line above footer
  tft.fillRect(20, TFT_H - UI_FOOTER_H - 6, TFT_W - 40, 2, C_ACCENT);

  // Symmetric color-coded footer buttons — anchored to both edges
  const int btnW = (TFT_W - 18) / 2; // 18 = 6(left pad) + 6(gap) + 6(right pad)
  const int y    = TFT_H - UI_FOOTER_H + 4;
  const int bh   = 26;
  const int rightX = TFT_W - 6 - btnW; // Anchored to right edge

  // START / PAUSE button
  uint16_t startCol = running ? ILI9341_ORANGE : C_OK;
  tft.fillRoundRect(6, y, btnW, bh, 6, startCol);
  tft.drawRoundRect(6, y, btnW, bh, 6, ILI9341_WHITE);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  const char* startLbl = running ? "PAUSE" : "START";
  int16_t tx1, ty1; uint16_t tw, th;
  tft.getTextBounds(startLbl, 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(6 + (btnW - tw) / 2, y + 19);
  tft.print(startLbl);

  // RESET button — right-anchored
  tft.fillRoundRect(rightX, y, btnW, bh, 6, C_BTN_BG);
  tft.drawRoundRect(rightX, y, btnW, bh, 6, C_BTN_BORDER);
  tft.setTextColor(C_BTN_FG);
  tft.getTextBounds("RESET", 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(rightX + (btnW - tw) / 2, y + 19);
  tft.print("RESET");
}

void uiTimerUpdateTime(uint32_t remainMs, bool running, int presetIdx) {
  uint32_t t = remainMs;
  uint32_t secs = (t / 1000) % 60;
  uint32_t mins = (t / 60000);
  char buf[24];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)mins, (unsigned long)secs);
  setFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_ACCENT);
  int bandY = UI_HEADER_H + 10;
  int bandH = 80;
  tft.fillRect(0, bandY, TFT_W, bandH, C_BG);
  int x = xCentered(buf, &FreeSansBold24pt7b);
  int base = baselineCenteredBand(bandY, bandH, buf, &FreeSansBold24pt7b);
  tft.setCursor(x, base);
  tft.print(buf);
}

void uiTimerDrawFull(uint32_t remainMs, bool running, int presetIdx) {
  tft.fillScreen(C_BG);
  drawHeader("TIMER", (WiFi.status() == WL_CONNECTED));
  uiTimerUpdateTime(remainMs, running, presetIdx);

  // Preset badge pills — larger (56×24) with proper text centering
  const char* pnames[] = { "1 min", "3 min", "5 min" };
  const int pillW    = 56;
  const int pillH    = 24;
  const int pillGap  = 8;
  const int dotAreaY = UI_HEADER_H + 96;
  const int dotX0    = (TFT_W - (3 * pillW + 2 * pillGap)) / 2;
  setFont(&FreeSans9pt7b);
  for (int pi = 0; pi < 3; pi++) {
    int px = dotX0 + pi * (pillW + pillGap);
    bool sel = (pi == presetIdx);
    tft.fillRoundRect(px, dotAreaY, pillW, pillH, 5, sel ? C_ACCENT : C_TILE_BG);
    tft.drawRoundRect(px, dotAreaY, pillW, pillH, 5, sel ? C_ACCENT : C_TILE_BORDER);
    tft.setTextColor(sel ? ILI9341_WHITE : C_SUB);
    // Center text in pill
    int16_t tx1, ty1; uint16_t tw, th;
    tft.getTextBounds(pnames[pi], 0, 0, &tx1, &ty1, &tw, &th);
    tft.setCursor(px + (pillW - tw) / 2, dotAreaY + 17);
    tft.print(pnames[pi]);
  }

  // Accent line above footer
  tft.fillRect(20, TFT_H - UI_FOOTER_H - 6, TFT_W - 40, 2, C_ACCENT);

  // Symmetric color-coded footer buttons — anchored to both edges
  const int btnW = (TFT_W - 18) / 2;
  const int y    = TFT_H - UI_FOOTER_H + 4;
  const int bh   = 26;
  const int rightX = TFT_W - 6 - btnW;

  uint16_t startCol = running ? ILI9341_ORANGE : C_OK;
  tft.fillRoundRect(6, y, btnW, bh, 6, startCol);
  tft.drawRoundRect(6, y, btnW, bh, 6, ILI9341_WHITE);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  const char* startLbl = running ? "PAUSE" : "START";
  int16_t tx1, ty1; uint16_t tw, th;
  tft.getTextBounds(startLbl, 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(6 + (btnW - tw) / 2, y + 19);
  tft.print(startLbl);

  tft.fillRoundRect(rightX, y, btnW, bh, 6, C_BTN_BG);
  tft.drawRoundRect(rightX, y, btnW, bh, 6, C_BTN_BORDER);
  tft.setTextColor(C_BTN_FG);
  tft.getTextBounds("RESET", 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(rightX + (btnW - tw) / 2, y + 19);
  tft.print("RESET");
}

// POMODORO
static void drawPomodoroBody(uint32_t remainMs, bool running, bool isWork, int cycles, bool full) {
  if (full) {
    tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
  }

  // Status pill
  if (full) {
    const char* statusStr = isWork ? "WORK TIME" : "BREAK TIME";
    uint16_t pillColor    = isWork ? C_ACCENT : ILI9341_ORANGE;
    setFont(&FreeSansBold12pt7b);
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(statusStr, 0, 0, &x1, &y1, &w, &h);
    int pillX = (TFT_W - (int)w - 20) / 2;
    tft.fillRoundRect(pillX, UI_HEADER_H + 8, (int)w + 20, (int)h + 8, 6, pillColor);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(pillX + 10, UI_HEADER_H + 8 + (int)h + 2);
    tft.print(statusStr);

    // Cycle dots (up to 4)
    const int maxDots  = 4;
    const int dotGap   = 16;
    int dotBaseX = (TFT_W - maxDots * dotGap) / 2;
    int dotBaseY = UI_HEADER_H + 54;
    for (int d = 0; d < maxDots; d++) {
      bool filled = (d < (cycles % (maxDots + 1)));
      if (filled) tft.fillCircle(dotBaseX + d * dotGap, dotBaseY, 5, pillColor);
      else        tft.drawCircle(dotBaseX + d * dotGap, dotBaseY, 5, C_DIV);
    }
  }

  // Large timer
  uint32_t sec = (remainMs + 999) / 1000;
  uint32_t m = sec / 60;
  uint32_t s = sec % 60;
  char timeBuf[16];
  sprintf(timeBuf, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);

  static char lastTimeBuf[16] = "";
  if (full || strcmp(timeBuf, lastTimeBuf) != 0) {
    int timeY = UI_HEADER_H + 112;
    int timeH = 44;
    tft.fillRect(0, timeY - timeH + 5, TFT_W, timeH, C_BG);
    setFont(&FreeSansBold24pt7b);
    tft.setTextColor(C_TEXT);
    int xTime = xCentered(timeBuf, &FreeSansBold24pt7b);
    tft.setCursor(xTime, timeY);
    tft.print(timeBuf);
    strcpy(lastTimeBuf, timeBuf);
  }

  // Wide rounded progress bar
  const int barW = TFT_W - 30;
  const int barH = 14;
  const int barX = 15;
  const int barY = UI_HEADER_H + 128;
  extern uint32_t pomoWorkMs;
  extern uint32_t pomoBreakMs;
  uint32_t totalMs = isWork ? pomoWorkMs : pomoBreakMs;
  int fillW = (totalMs > 0) ? (int)((remainMs * (uint32_t)barW) / totalMs) : 0;
  if (fillW > barW) fillW = barW;

  static int lastFillW = -1;
  if (full || fillW != lastFillW) {
    if (full) tft.drawRoundRect(barX, barY, barW, barH, 4, C_SUB);
    tft.fillRoundRect(barX + 1, barY + 1, barW - 2, barH - 2, 3, C_BG);
    if (fillW > 2) tft.fillRoundRect(barX + 1, barY + 1, fillW - 2, barH - 2, 3,
                                     isWork ? C_ACCENT : ILI9341_ORANGE);
    lastFillW = fillW;
  }
}

void uiPomodoroDrawFull(uint32_t remainMs, bool running, bool isWork, int cycles) {
  tft.fillScreen(C_BG);
  drawHeader("POMODORO", (WiFi.status() == WL_CONNECTED));
  drawPomodoroBody(remainMs, running, isWork, cycles, true);

  // Symmetric color-coded footer buttons — anchored to both edges
  const int btnW = (TFT_W - 18) / 2;
  const int y    = TFT_H - UI_FOOTER_H + 4;
  const int bh   = 26;
  const int rightX = TFT_W - 6 - btnW;

  uint16_t startCol = running ? ILI9341_ORANGE : C_OK;
  tft.fillRoundRect(6, y, btnW, bh, 6, startCol);
  tft.drawRoundRect(6, y, btnW, bh, 6, ILI9341_WHITE);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  const char* startLbl = running ? "STOP" : "START";
  int16_t tx1, ty1; uint16_t tw, th;
  tft.getTextBounds(startLbl, 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(6 + (btnW - tw) / 2, y + 19);
  tft.print(startLbl);

  tft.fillRoundRect(rightX, y, btnW, bh, 6, C_BTN_BG);
  tft.drawRoundRect(rightX, y, btnW, bh, 6, C_BTN_BORDER);
  tft.setTextColor(C_BTN_FG);
  tft.getTextBounds("RESET", 0, 0, &tx1, &ty1, &tw, &th);
  tft.setCursor(rightX + (btnW - tw) / 2, y + 19);
  tft.print("RESET");
}

void uiPomodoroUpdate(uint32_t remainMs, bool running, bool isWork, int cycles) {
  drawPomodoroBody(remainMs, running, isWork, cycles, false);
}

// SKETCH PAD
// Draws the full sketch screen (header + blank canvas + footer).
// Does NOT reset currentSketchLen — callers must call uiSketchClear() first
// if they need a fresh canvas (e.g., new session from menu).
// For title-screen wake restore, do NOT call uiSketchClear() — just call
// uiSketchDrawFull() then uiSketchRedrawCurrent() to replay the stored lines.
void uiSketchDrawFull(uint16_t currentColor) {
  if (!currentSketch) {
    currentSketch = (SketchLine*)malloc(SKETCH_MAX_LINES * sizeof(SketchLine));
    currentSketchLen = 0; // Only reset on first-time allocation
  }
  tft.fillScreen(C_BG);
  drawHeader("SKETCH PAD", (WiFi.status() == WL_CONNECTED));
  uiSketchDrawFooter(currentColor);
}

// Helper for premium line drawing across all functions perfectly evenly
static void __drawSleekLine(int x1, int y1, int x2, int y2, uint16_t color) {
  uint16_t realColor = (color == 0x0001) ? C_BG : color;
  if (color == 0x0001) {
    // Premium Eraser Mode: Thicker 5x5 brush for rapid clearing
    for(int dx=-2; dx<=2; dx++) {
      for(int dy=-2; dy<=2; dy++) {
        tft.drawLine(x1+dx, y1+dy, x2+dx, y2+dy, realColor);
      }
    }
    tft.fillCircle(x1, y1, 2, realColor);
    tft.fillCircle(x2, y2, 2, realColor);
  } else {
    // Standard sleek 3x3 drawing brush
    tft.drawLine(x1, y1, x2, y2, color);
    tft.drawLine(x1+1, y1, x2+1, y2, color);
    tft.drawLine(x1-1, y1, x2-1, y2, color);
    tft.drawLine(x1, y1+1, x2, y2+1, color);
    tft.drawLine(x1, y1-1, x2, y2-1, color);
    tft.fillCircle(x1, y1, 1, color);
    tft.fillCircle(x2, y2, 1, color);
  }
}

void uiSketchRedrawCurrent() {
  if (!currentSketch) return;
  for (int i = 0; i < currentSketchLen; i++) {
    SketchLine& l = currentSketch[i];
    __drawSleekLine(l.x1, l.y1, l.x2, l.y2, l.color);
  }
}

void uiSketchClear() {
  currentSketchLen = 0;
  tft.fillRect(0, UI_HEADER_H, TFT_W, TFT_H - UI_HEADER_H - UI_FOOTER_H, C_BG);
}

void uiSketchDrawFooter(uint16_t currentColor) {
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, y, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, TFT_W, C_DIV);

  // High-end pill buttons
  setFont(nullptr); 

  // EXIT button
  tft.fillRoundRect(6, y + 4, 38, 26, 5, C_BTN_BG);
  tft.drawRoundRect(6, y + 4, 38, 26, 5, C_BTN_BORDER);
  tft.setTextColor(C_BTN_FG);
  tft.setCursor(14, y + 13);
  tft.print("EXIT");

  // SAVE button
  tft.fillRoundRect(48, y + 4, 38, 26, 5, C_BTN_BG);
  tft.drawRoundRect(48, y + 4, 38, 26, 5, C_BTN_BORDER);
  tft.setCursor(56, y + 13);
  tft.print("SAVE");

  // CLR button
  tft.fillRoundRect(90, y + 4, 34, 26, 5, C_BTN_BG);
  tft.drawRoundRect(90, y + 4, 34, 26, 5, ILI9341_RED); 
  tft.setTextColor(ILI9341_RED);
  tft.setCursor(98, y + 13);
  tft.print("CLR");

  // Flush-right HD Color slider UI + Eraser
  const uint16_t colors[] = { ILI9341_WHITE, ILI9341_BLACK, ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE, ILI9341_YELLOW, ILI9341_CYAN, 0x0001 };
  const int count = 8;
  const int dotW = 21;
  const int startX = TFT_W - (count * dotW) - 2;
  
  for (int i = 0; i < count; i++) {
    int dx = startX + i * dotW + (dotW / 2);
    int dy = y + (UI_FOOTER_H / 2);
    
    if (colors[i] == 0x0001) {
      // Eraser Tool Rendering
      if (currentColor == 0x0001) {
        tft.fillCircle(dx, dy, 10, ILI9341_WHITE);
        tft.fillCircle(dx, dy, 7, C_BG);
        setFont(nullptr); tft.setTextColor(ILI9341_WHITE); 
        tft.setCursor(dx - 3, dy - 3); tft.print("E"); // Label 'E'
      } else {
        tft.fillCircle(dx, dy, 8, C_BG);
        tft.drawCircle(dx, dy, 8, C_SUB); 
        setFont(nullptr); tft.setTextColor(C_SUB); 
        tft.setCursor(dx - 3, dy - 3); tft.print("E"); // Label 'E'
      }
    } else {
      // Standard Color Dots
      if (colors[i] == currentColor) {
        tft.fillCircle(dx, dy, 10, ILI9341_WHITE);
        tft.fillCircle(dx, dy, 7, colors[i]); // Inside core
      } else {
        tft.fillCircle(dx, dy, 8, colors[i]);
        tft.drawCircle(dx, dy, 8, C_SUB); // Faint ring to distinguish black on black
      }
    }
  }
}

void uiSketchDrawLine(int x1, int y1, int x2, int y2, uint16_t color, int z) {
  // Only draw in the body area (between header and footer)
  if (y1 < UI_HEADER_H || y1 >= (TFT_H - UI_FOOTER_H)) return;
  if (y2 < UI_HEADER_H || y2 >= (TFT_H - UI_FOOTER_H)) return;
  
  // FIXED THICKNESS: 3px solid for perfect consistency across the entire screen
  // (Removes the 'thin on one side, thick on other' issue caused by screen pressure variation)
  const int thick = 3;

  // Record the line in our buffer for saving
  if (currentSketch && currentSketchLen < SKETCH_MAX_LINES) {
    currentSketch[currentSketchLen++] = { (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, color, (uint8_t)thick };
  }

  __drawSleekLine(x1, y1, x2, y2, color);
}

void uiSketchDrawBezier(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color, int z) {
  // Quadratic Bézier curve: P0=start, P1=control, P2=end
  // 12 segments for ultra-smooth phone-like paths
  const int segments = 12;
  int lastX = x0, lastY = y0;
  
  for (int i = 1; i <= segments; i++) {
    float t = (float)i / segments;
    float tInv = 1.0f - t;
    
    // B(t) = (1-t)^2*P0 + 2(1-t)t*P1 + t^2*P2
    float bx = (tInv * tInv * x0) + (2.0f * tInv * t * x1) + (t * t * x2);
    float by = (tInv * tInv * y0) + (2.0f * tInv * t * y1) + (t * t * y2);
    
    int curX = (int)(bx + 0.5f);
    int curY = (int)(by + 0.5f);
    
    uiSketchDrawLine(lastX, lastY, curX, curY, color, z);
    lastX = curX;
    lastY = curY;
  }
}

// SAVE logic
void uiSketchSave() {
  if (!LittleFS.begin(false)) {
    Serial.println("FS Mount Failed");
    return;
  }
  
  // Find next sketch index
  int id = 1;
  while (id < 100) {
    char path[32];
    snprintf(path, sizeof(path), "/sketch%d.bin", id);
    if (!LittleFS.exists(path)) break;
    id++;
  }

  char path[32];
  snprintf(path, sizeof(path), "/sketch%d.bin", id);
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("Failed to open file for writing");
    return;
  }

  // Write metadata (num lines)
  f.write((uint8_t*)&currentSketchLen, sizeof(int));
  // Write lines
  if (currentSketchLen > 0 && currentSketch) {
    f.write((uint8_t*)currentSketch, currentSketchLen * sizeof(SketchLine));
  }
  f.close();
  Serial.printf("Saved sketch to %s\n", path);

  // Quick feedback: flash the SAVE button green
  const int y = TFT_H - UI_FOOTER_H;
  tft.fillRoundRect(48, y + 4, 38, 26, 5, ILI9341_GREEN);
  delay(120);
  tft.fillRoundRect(48, y + 4, 38, 26, 5, C_BTN_BG);
  tft.drawRoundRect(48, y + 4, 38, 26, 5, C_BTN_BORDER);
  setFont(nullptr); tft.setTextColor(C_BTN_FG);
  tft.setCursor(56, y + 13);
  tft.print("SAVE");
}

int uiSketchGalleryGetCount() {
  if (!LittleFS.begin(false)) return 0;
  int count = 0;
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (strstr(f.name(), "sketch") && strstr(f.name(), ".bin")) {
      count++;
    }
    f.close(); // Prevent file descriptor leak
    f = root.openNextFile();
  }
  root.close();
  return count;
}

// Gallery logic
void uiSketchGalleryDrawFull(int sel, int top) {
  tft.fillScreen(C_BG);
  drawHeader("SKETCH GALLERY", (WiFi.status() == WL_CONNECTED));
  
  // Standard list layout
  const int rowH = 28;
  const int bodyY = UI_HEADER_H;
  const int bodyH = TFT_H - UI_HEADER_H - UI_FOOTER_H;
  const int maxRows = bodyH / rowH;
  
  if (!LittleFS.begin(false)) return;
  
  int count = 0;
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (strstr(f.name(), "sketch") && strstr(f.name(), ".bin")) {
      if (count >= top && count < top + maxRows) {
        int y = bodyY + (count - top) * rowH;
        uint16_t bg = (count == sel) ? C_SEL_BG : C_BG;
        uint16_t fg = (count == sel) ? C_ACCENT : C_TEXT;
        
        tft.fillRect(0, y, TFT_W, rowH, bg);
        setFont(&FreeSans9pt7b);
        tft.setTextColor(fg);
        tft.setCursor(14, y + 20);
        
        // Extract number from filename safely accommodating ESP32 VFS '/'
        int num = 0;
        const char* ptr = f.name();
        if (ptr[0] == '/') ptr++; 
        sscanf(ptr, "sketch%d.bin", &num);
        
        char label[32];
        snprintf(label, sizeof(label), "Canvas Drawing #%d", num);
        tft.print(label);
        tft.drawFastHLine(0, y + rowH - 1, TFT_W, (count == sel) ? C_ACCENT : C_DIV);
      }
      count++;
    }
    f.close(); // Prevent file descriptor leak
    f = root.openNextFile();
  }
  root.close();
  
  // Standard footer with DELETE button
  tft.fillRect(0, TFT_H - UI_FOOTER_H, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, TFT_H - UI_FOOTER_H, TFT_W, C_DIV);
  
  // Instructions (Left aligned)
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, TFT_H - 22);
  tft.print("Swipe:Scroll | Back:Swipe-R");
  
  // Delete Button (Red, Larger)
  int btnW = 75;
  int btnH = 26;
  int delX = TFT_W - btnW - 8;
  int delY = TFT_H - UI_FOOTER_H + 4;
  
  tft.fillRoundRect(delX, delY, btnW, btnH, 5, ILI9341_RED);
  tft.drawRoundRect(delX, delY, btnW, btnH, 5, ILI9341_WHITE); // White border for clarity
  
  tft.setTextColor(ILI9341_WHITE);
  setFont(nullptr); // Use default font for button for maximum clarity
  
  // High-precision centering for "DELETE"
   const char* delText = "DELETE";
   int16_t x1, y1; uint16_t w, h;
   tft.getTextBounds(delText, 0, 0, &x1, &y1, &w, &h);
   // For default font, setCursor is top-left
   tft.setCursor(delX + (btnW - w) / 2, delY + (btnH - h) / 2);
   tft.print(delText);
 }

void uiSketchView(int idx, int sel, int top) {
  // Find the file for idx
  if (!LittleFS.begin(false)) return;
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  int count = 0;
  while (f) {
    if (strstr(f.name(), "sketch") && strstr(f.name(), ".bin")) {
      if (count == idx) {
        // Read lines and draw
        int numLines = 0;
        f.read((uint8_t*)&numLines, sizeof(int));
        
        tft.fillScreen(C_BG);
        drawHeader(f.name(), (WiFi.status() == WL_CONNECTED));
        
        for (int i = 0; i < numLines; i++) {
          SketchLine line;
          f.read((uint8_t*)&line, sizeof(SketchLine));
          __drawSleekLine(line.x1, line.y1, line.x2, line.y2, line.color);
        }
        
        // Wait for touch to exit view
        while (touchIsTouched()) delay(10);
        while (!touchIsTouched()) delay(10);
        while (touchIsTouched()) delay(10);
        
        uiSketchGalleryDrawFull(sel, top);
        return;
      }
      count++;
    }
    f.close(); // Prevent file descriptor leak
    f = root.openNextFile();
  }
  root.close();
}

void uiSketchDelete(int idx) {
  if (!LittleFS.begin(false)) return;
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  int count = 0;
  while (f) {
    if (strstr(f.name(), "sketch") && strstr(f.name(), ".bin")) {
      if (count == idx) {
        String path = "/" + String(f.name());
        f.close();
        root.close();
        LittleFS.remove(path);
        Serial.printf("Deleted %s\n", path.c_str());
        return;
      }
      count++;
    }
    f.close(); // Prevent file descriptor leak
    f = root.openNextFile();
  }
  root.close();
}

void uiUnitConvDrawFull(int mode, float inVal, float outVal, const char* inLabel, const char* outLabel) {
  tft.fillScreen(C_BG);
  drawHeader("UNIT CONVERTER", (WiFi.status() == WL_CONNECTED));

  // Mode label centered under header
  const char* modeStr = (mode == 0) ? "TEMPERATURE" : "PRESSURE";
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_ACCENT);
  int16_t ml1, ml2; uint16_t mw, mh;
  tft.getTextBounds(modeStr, 0, 0, &ml1, &ml2, &mw, &mh);
  tft.setCursor((TFT_W - mw) / 2, UI_HEADER_H + 18);
  tft.print(modeStr);
  tft.drawFastHLine((TFT_W - mw) / 2, UI_HEADER_H + 21, mw, C_ACCENT);

  // Tile dimensions
  const int tileW = TFT_W - 20;
  const int tileH = 50;
  const int tile1Y = UI_HEADER_H + 28;

  // Input tile
  tft.fillRoundRect(10, tile1Y, tileW, tileH, 6, C_TILE_BG);
  tft.drawRoundRect(10, tile1Y, tileW, tileH, 6, C_TILE_BORDER);
  // Label (readable font)
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(18, tile1Y + 16);
  tft.print("Input: ");
  tft.print(inLabel);  // e.g. "C" or "hPa"
  // Value (large)
  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT);
  tft.setCursor(18, tile1Y + 40);
  tft.print(inVal, mode == 0 ? 1 : 0);

  // Arrow
  int arrowY = tile1Y + tileH + 6;
  tft.fillRect(TFT_W/2 - 1, arrowY, 2, 10, C_ACCENT);
  for (int ai = 0; ai < 5; ai++)
    tft.drawFastHLine(TFT_W/2 - ai, arrowY + 10 + ai, ai * 2 + 2, C_ACCENT);

  // Output tile
  const int tile2Y = tile1Y + tileH + 26;
  tft.fillRoundRect(10, tile2Y, tileW, tileH, 6, C_TILE_BG);
  tft.drawRoundRect(10, tile2Y, tileW, tileH, 6, C_ACCENT);
  // Label (readable font)
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(18, tile2Y + 16);
  tft.print("Output: ");
  tft.print(outLabel); // e.g. "F" or "inHg"
  // Value (large, accent)
  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_ACCENT);
  tft.setCursor(18, tile2Y + 40);
  tft.print(outVal, mode == 0 ? 1 : 2);

  // Tap hint
  setFont(nullptr);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, tile2Y + tileH + 12);
  tft.print(mode == 0 ? "Tap tile = switch C/F" : "Tap tile = switch hPa/inHg");

  // Footer buttons — symmetric halves (left=minus, right=plus)
  const int btnW = (TFT_W - 30) / 2; // matches inUcMinus / inUcPlus hitboxes
  int yf = TFT_H - UI_FOOTER_H + 4;
  int bh = 26;
  tft.fillRoundRect(6, yf, btnW, bh, 6, C_BTN_BG);
  tft.drawRoundRect(6, yf, btnW, bh, 6, C_BTN_BORDER);
  setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_BTN_FG);
  tft.setCursor(6 + (btnW - 10) / 2, yf + 20);
  tft.print("-");

  tft.fillRoundRect(6 + btnW + 6, yf, btnW, bh, 6, C_BTN_BG);
  tft.drawRoundRect(6 + btnW + 6, yf, btnW, bh, 6, C_BTN_BORDER);
  tft.setCursor(6 + btnW + 6 + (btnW - 10) / 2, yf + 20);
  tft.print("+");
}

// ============================================================================
// NEW: UI accessors (needed by games.cpp)
// ============================================================================
Adafruit_ILI9341& uiTft() { return tft; }

int uiHeaderH() { return UI_HEADER_H; }
int uiFooterH() { return UI_FOOTER_H; }

int uiBodyY0() { return UI_HEADER_H; }
int uiBodyH()  { return TFT_H - UI_HEADER_H - UI_FOOTER_H; }

uint16_t uiColorBg()      { return C_BG; }
uint16_t uiColorText()    { return C_TEXT; }
uint16_t uiColorSub()     { return C_SUB; }
uint16_t uiColorAccent()  { return C_ACCENT; }
uint16_t uiColorSelBg()   { return C_SEL_BG; }
uint16_t uiColorDivider() { return C_DIV; }

void uiAlarmsDrawFull(int sel, bool isEditing, int editField, Alarm* alarms, int maxAlarms) {
  tft.fillScreen(C_BG);
  drawHeader("ALARM CLOCK", (WiFi.status() == WL_CONNECTED));
  const int rowH = 46;
  const int bodyY = UI_HEADER_H + 10;
  
  for (int i = 0; i < maxAlarms; i++) {
    int y = bodyY + i * rowH;
    bool selected = (i == sel);
    uint16_t bg = selected ? C_SEL_BG : C_BG;
    uint16_t fg = selected ? C_ACCENT : C_TEXT;
    
    tft.fillRect(0, y, TFT_W, rowH, bg);
    
    // Left border accent if selected
    if (selected) tft.fillRect(0, y, 4, rowH, C_ACCENT);

    setFont(&FreeSansBold12pt7b);
    
    char hStr[4], mStr[4];
    int dispH = alarms[i].hour % 12;
    if (dispH == 0) dispH = 12;
    const char* ampm = alarms[i].hour >= 12 ? "PM" : "AM";
    snprintf(hStr, sizeof(hStr), "%02d", dispH);
    snprintf(mStr, sizeof(mStr), "%02d", alarms[i].minute);
    
    // Draw Hours
    tft.setTextColor((selected && isEditing && editField == 0) ? ILI9341_YELLOW : fg);
    tft.setCursor(20, y + 30);
    tft.print(hStr);

    // Draw Colon
    tft.setTextColor(fg);
    tft.print(":");

    // Draw Minutes
    tft.setTextColor((selected && isEditing && editField == 1) ? ILI9341_YELLOW : fg);
    tft.print(mStr);
    
    setFont(&FreeSans9pt7b);
    tft.setTextColor((selected && isEditing && editField == 2) ? ILI9341_YELLOW : C_SUB);
    tft.setCursor(85, y + 30);
    tft.print(ampm);

    // Toggle switch (ON/OFF)
    int swX = TFT_W - 60;
    int swY = y + 10;
    bool en = alarms[i].enabled;
    
    if (selected && isEditing && editField == 3) {
      tft.fillRoundRect(swX - 2, swY - 2, 48, 28, 14, ILI9341_YELLOW); // glow
    }
    
    tft.fillRoundRect(swX, swY, 44, 24, 12, en ? ILI9341_GREEN : ILI9341_DARKGREY);
    if (en) {
      tft.fillCircle(swX + 32, swY + 12, 10, ILI9341_WHITE);
    } else {
      tft.fillCircle(swX + 12, swY + 12, 10, ILI9341_LIGHTGREY);
    }

    tft.drawFastHLine(0, y + rowH - 1, TFT_W, selected ? C_ACCENT : C_DIV);
  }
  
  const int botY = TFT_H - UI_FOOTER_H;
  tft.fillRect(0, botY, TFT_W, UI_FOOTER_H, C_BG);
  tft.drawFastHLine(0, botY, TFT_W, C_DIV);
  setFont(&FreeSans9pt7b);
  tft.setTextColor(C_SUB);
  tft.setCursor(10, botY + 24);
  
  if (isEditing) {
    if (editField == 0) tft.print("Set Hour | Click -> to Min");
    else if (editField == 1) tft.print("Set Min | Click -> to Tgl");
    else tft.print("Set On/Off | Click -> Save");
  } else {
    tft.print("Click: Edit | Long: Back");
  }
}

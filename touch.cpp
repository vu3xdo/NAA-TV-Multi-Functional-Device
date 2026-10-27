
#include "touch.h"
#include "config.h"
#include "ui.h"
#include "storage.h"

#include <SPI.h>
#include <XPT2046_Touchscreen.h>

static XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
static bool ok = false;

static inline int clampi(int v, int lo, int hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

struct CalData {
  int minX, maxX, minY, maxY;
  bool swapXY;
  bool invX;
  bool invY;
  bool valid;
};

static CalData cal;

static void forceTftCsHigh() {
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
}

static bool calLooksSane(const CalData &c) {
  const int rx = c.maxX - c.minX;
  const int ry = c.maxY - c.minY;
  if (rx < 500 || ry < 500) return false;
  if (c.maxX <= 0 && c.maxY <= 0) return false;
  return true;
}

static void clearSavedCal() {
  storageSetBool("cal_v", false);
  storageRemove("cal_minx");
  storageRemove("cal_maxx");
  storageRemove("cal_miny");
  storageRemove("cal_maxy");
  storageRemove("cal_swp");
  storageRemove("cal_inx");
  storageRemove("cal_iny");
  Serial.println("[CAL] cleared saved calibration (was invalid)");
}

static void loadCal() {
  cal.valid = storageGetBool("cal_v", false);

  if (!cal.valid) {
    cal.minX = TS_MIN_X; cal.maxX = TS_MAX_X;
    cal.minY = TS_MIN_Y; cal.maxY = TS_MAX_Y;
    cal.swapXY = false; cal.invX = false; cal.invY = false;
    return;
  }

  cal.minX   = storageGetI32("cal_minx", TS_MIN_X);
  cal.maxX   = storageGetI32("cal_maxx", TS_MAX_X);
  cal.minY   = storageGetI32("cal_miny", TS_MIN_Y);
  cal.maxY   = storageGetI32("cal_maxy", TS_MAX_Y);
  cal.swapXY = storageGetBool("cal_swp", false);
  cal.invX   = storageGetBool("cal_inx", false);
  cal.invY   = storageGetBool("cal_iny", false);

  if (!calLooksSane(cal)) {
    clearSavedCal();
    cal.valid = false;
    cal.minX = TS_MIN_X; cal.maxX = TS_MAX_X;
    cal.minY = TS_MIN_Y; cal.maxY = TS_MAX_Y;
    cal.swapXY = false; cal.invX = false; cal.invY = false;
  }
}

static void saveCal(const CalData &c) {
  if (!calLooksSane(c)) {
    Serial.println("[CAL] refused to save invalid calibration");
    return;
  }

  storageSetI32("cal_minx", c.minX);
  storageSetI32("cal_maxx", c.maxX);
  storageSetI32("cal_miny", c.minY);
  storageSetI32("cal_maxy", c.maxY);
  storageSetBool("cal_swp", c.swapXY);
  storageSetBool("cal_inx", c.invX);
  storageSetBool("cal_iny", c.invY);
  storageSetBool("cal_v", true);
}

static void applyMapping(const TouchRaw &r, int &mx, int &my, const CalData &c) {
  int ax = r.x;
  int ay = r.y;

  if (c.swapXY) {
    int t = ax; ax = ay; ay = t;
  }

  int x1 = c.invX ? c.maxX : c.minX;
  int x2 = c.invX ? c.minX : c.maxX;
  int y1 = c.invY ? c.maxY : c.minY;
  int y2 = c.invY ? c.minY : c.maxY;

  int x = map(ax, x1, x2, 0, TFT_W - 1);
  int y = map(ay, y1, y2, 0, TFT_H - 1);

  mx = clampi(x, 0, TFT_W - 1);
  my = clampi(y, 0, TFT_H - 1);
}

bool touchBegin() {
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);

  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  ok = ts.begin();
  if (!ok) {
    Serial.println("[TOUCH] init FAIL");
    return false;
  }

  ts.setRotation(TOUCH_ROTATION);
  Serial.println("[TOUCH] init OK");

  loadCal();

#if (TOUCH_FORCE_INV_X == 0)
  cal.invX = false;
#elif (TOUCH_FORCE_INV_X == 1)
  cal.invX = true;
#endif

#if (TOUCH_FORCE_INV_Y == 0)
  cal.invY = false;
#elif (TOUCH_FORCE_INV_Y == 1)
  cal.invY = true;
#endif

  if (storageGetBool("cal_v", false)) {
    Serial.printf("[CAL] loaded minX=%d maxX=%d minY=%d maxY=%d swap=%d invX=%d invY=%d\n",
                  cal.minX, cal.maxX, cal.minY, cal.maxY,
                  (int)cal.swapXY, (int)cal.invX, (int)cal.invY);
  } else {
    Serial.println("[CAL] no saved calibration (using defaults)");
  }

  return true;
}

bool touchIsOk() { return ok; }

bool touchIsTouched() {
  if (!ok) return false;
  return ts.touched();
}

bool touchReadRaw(TouchRaw &out) {
  if (!ok) return false;

  forceTftCsHigh();
  // Quick initial check for touch
  if (!ts.touched()) return false;

  // Ultra-Fast High-Fidelity Sampling: 11 points for maximum noise rejection
  constexpr int N = 11;
  int xs[N], ys[N], zs[N];
  int n = 0;

  // No loop delays, just read as fast as SPI allows
  for (int i = 0; i < 20 && n < N; i++) {
    TS_Point p = ts.getPoint();
    // Hardware Debounce: Point must have coordinates > 10 and pressure > 500
    // (Fixes the "scattered dots" and "phantom lines" noise issue)
    if (p.x > 10 && p.y > 10 && p.z > 500) { 
      xs[n] = p.x; ys[n] = p.y; zs[n] = p.z;
      n++;
    }
  }

  if (n < 3) return false;

  auto sortSmall = [](int *a, int len) {
    for (int i = 1; i < len; i++) {
      int v = a[i];
      int j = i - 1;
      while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
      a[j + 1] = v;
    }
  };

  sortSmall(xs, n);
  sortSmall(ys, n);
  sortSmall(zs, n);

  out.x = xs[n / 2];
  out.y = ys[n / 2];
  out.z = zs[n / 2];
  return true;
}

bool touchReadMapped(int &mx, int &my, TouchRaw &rawOut) {
  if (!touchReadRaw(rawOut)) return false;
  applyMapping(rawOut, mx, my, cal);
  return true;
}

// ===================== Event Engine =====================
static bool downState = false;
static uint32_t downAt = 0;
static int downX = 0, downY = 0;
static int lastX = 0, lastY = 0;
static float filtX = 0, filtY = 0; // EMA filtered coordinates
static bool longSent = false;
static bool resilienceOn = false; // NEW: High-resilience mode

static uint32_t lastEmitAt = 0;
static int lostCount = 0; // NEW: track consecutive frames without touch

static uint32_t lastTapAt = 0; // For double-tap
static int lastTapX = 0, lastTapY = 0;

static int iabs(int v) { return v < 0 ? -v : v; }

void touchSetResilience(bool on) {
  resilienceOn = on;
}

bool touchPollEvent(TouchEvent &ev) {
  ev.type = TE_NONE;

  TouchRaw r;
  int rawX, rawY;

  // Ultra-Fast Filter: 1ms throttle
  const uint32_t now = millis();
  if (now - lastEmitAt < 1) return false;

  bool has = touchReadMapped(rawX, rawY, r);

  // Simplified Resilience: 20ms bridge (2 frames)
  if (resilienceOn && downState) {
    if (!has) {
      lostCount++;
      if (lostCount < 2) { 
        has = true;
        rawX = (int)filtX;
        rawY = (int)filtY;
      }
    } else {
      // Stroke Break: 30px distance limit
      float dx = (float)rawX - filtX;
      float dy = (float)rawY - filtY;
      if (dx*dx + dy*dy > 900.0f) has = false; 
      else lostCount = 0;
    }
  }

  // Ultra-Smooth Adaptive EMA (alpha changes based on speed)
  // Slow = low alpha (ultra-smooth for dots/taps), Fast = high alpha (responsive)
  float alpha = 0.4f; 
  if (has && downState) {
    float dxd = (float)rawX - filtX;
    float dyd = (float)rawY - filtY;
    if (dxd*dxd + dyd*dyd > 25.0f) alpha = 0.85f; // Fast stroke
    else if (dxd*dxd + dyd*dyd > 4.0f) alpha = 0.6f; // Normal drawing
  }

  if (has) {
    if (!downState) {
      filtX = (float)rawX;
      filtY = (float)rawY;
    } else {
      filtX = (alpha * (float)rawX) + ((1.0f - alpha) * filtX);
      filtY = (alpha * (float)rawY) + ((1.0f - alpha) * filtY);
    }
  }

  int x = clampi((int)(filtX + 0.5f), 0, TFT_W - 1);
  int y = clampi((int)(filtY + 0.5f), 0, TFT_H - 1);

  // state transitions
  if (has && !downState) {
    downState = true;
    downAt = now;
    downX = x; downY = y;
    lastX = x; lastY = y;
    longSent = false;
    lostCount = 0;

    ev.type = TE_DOWN;
    ev.x = x; ev.y = y;
    ev.dx = 0; ev.dy = 0;
    ev.ms = 0;
    ev.raw = r;

    lastEmitAt = now;
    return true;
  }

  if (!has && downState) {
    downState = false;
    const uint32_t held = now - downAt;
    const int dx = lastX - downX;
    const int dy = lastY - downY;

    // Swipe detect on UP
    if (iabs(dx) >= TOUCH_SWIPE_MIN_PX && iabs(dy) <= TOUCH_SWIPE_MAX_OFFPX) {
      ev.type = (dx > 0) ? TE_SWIPE_RIGHT : TE_SWIPE_LEFT;
      ev.x = lastX; ev.y = lastY;
      ev.dx = dx; ev.dy = dy;
      ev.ms = held;
      ev.raw = r;
      lastEmitAt = now;
      return true;
    }
    
    // Vertical swipe detect
    if (iabs(dy) >= TOUCH_SWIPE_MIN_PX && iabs(dx) <= TOUCH_SWIPE_MAX_OFFPX) {
      ev.type = (dy > 0) ? TE_SWIPE_DOWN : TE_SWIPE_UP;
      ev.x = lastX; ev.y = lastY;
      ev.dx = dx; ev.dy = dy;
      ev.ms = held;
      ev.raw = r;
      lastEmitAt = now;
      return true;
    }

    // Tap detect
    if (!longSent &&
        held <= TOUCH_TAP_MAX_MS &&
        iabs(dx) <= TOUCH_TAP_MAX_MOVE_PX &&
        iabs(dy) <= TOUCH_TAP_MAX_MOVE_PX) {
      
      bool isDbl = (now - lastTapAt < 350) && 
                   (iabs(lastX - lastTapX) < 40) && 
                   (iabs(lastY - lastTapY) < 40);
      
      if (isDbl) {
        ev.type = TE_DBL_TAP;
        lastTapAt = 0; // reset
      } else {
        ev.type = TE_TAP;
        lastTapAt = now;
        lastTapX = lastX;
        lastTapY = lastY;
      }
      
      ev.x = lastX; ev.y = lastY;
      ev.dx = dx; ev.dy = dy;
      ev.ms = held;
      ev.raw = r;
      lastEmitAt = now;
      return true;
    }

    ev.type = TE_UP;
    ev.x = lastX; ev.y = lastY;
    ev.dx = dx; ev.dy = dy;
    ev.ms = held;
    ev.raw = r;
    lastEmitAt = now;
    return true;
  }

  // longpress while held (fire once)
  if (downState && !longSent) {
    const uint32_t held = now - downAt;
    const int dx = lastX - downX;
    const int dy = lastY - downY;

    if (held >= TOUCH_LONGPRESS_MS &&
        iabs(dx) <= TOUCH_TAP_MAX_MOVE_PX * 2 &&
        iabs(dy) <= TOUCH_TAP_MAX_MOVE_PX * 2) {
      longSent = true;
      ev.type = TE_LONGPRESS;
      ev.x = lastX; ev.y = lastY;
      ev.dx = dx; ev.dy = dy;
      ev.ms = held;
      ev.raw = r;
      lastEmitAt = now;
      return true;
    }

    // Drag detection (continuous move while down) with 3px Deadzone
    if (iabs(x - lastX) >= 3 || iabs(y - lastY) >= 3) {
      ev.type = TE_DRAG;
      ev.x = x; ev.y = y;
      ev.dx = x - lastX;
      ev.dy = y - lastY;
      ev.ms = held;
      ev.raw = r;
      // update lastX/lastY for next drag delta
      lastX = x; lastY = y;
      lastEmitAt = now;
      return true;
    }
  }

  return false;
}

void touchResetState() {
  downState = false;
  downAt = 0;
  downX = downY = 0;
  lastX = lastY = 0;
  longSent = false;
  lastEmitAt = 0;
}

// ===================== Step tools =====================
void touchStep2RawLoggerLoop() {
  static uint32_t last = 0;
  TouchRaw r;
  if (!touchReadRaw(r)) return;

  const uint32_t now = millis();
  if (now - last < TOUCH_SAMPLE_THROTTLE) return;
  last = now;

  Serial.printf("[RAW] x=%d y=%d z=%d irq=%d\n", r.x, r.y, r.z, digitalRead(TOUCH_IRQ));
}

void touchStep3DotLoop() {
  static uint32_t last = 0;
  TouchRaw r;
  int mx, my;
  if (!touchReadMapped(mx, my, r)) return;

  const uint32_t now = millis();
  if (now - last < TOUCH_SAMPLE_THROTTLE) return;
  last = now;

  uiDrawDot(mx, my);
  Serial.printf("[TOUCH] raw=(%d,%d,%d) map=(%d,%d)\n", r.x, r.y, r.z, mx, my);
}

void touchStep7BacklightLoop() {
  if (!ok) return;

  static bool isDown = false;
  static uint32_t downAt2 = 0;

  forceTftCsHigh();
  const bool down = ts.touched();
  const uint32_t now = millis();

  if (down && !isDown) {
    isDown = true;
    downAt2 = now;
  } else if (!down && isDown) {
    isDown = false;
  }

  if (isDown && uiBacklightIsOn() && (now - downAt2 > TOUCH_LONGPRESS_MS)) {
    uiSetBacklight(false);
    Serial.println("[SLEEP] Backlight OFF (soft sleep)");
    while (ts.touched()) delay(10);
    isDown = false;
  }
}

// ---------------- STEP 8 DEBUG ----------------
void touchStep8DebugTouchState() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 500) return;
  last = now;

  forceTftCsHigh();
  const int irq = digitalRead(TOUCH_IRQ);
  const bool down = ts.touched();

  Serial.printf("[DBG] irq=%d touched=%d\n", irq, (int)down);
}

// ---------------- STEP 8 CALIBRATION ----------------
struct CornerSample { int rx; int ry; };

static bool captureCornerHold(CornerSample &out) {
  static bool inPress = false;
  static uint32_t pressAt = 0;
  static CornerSample lastGood{0,0};

  forceTftCsHigh();
  const bool down = ts.touched();
  const uint32_t now = millis();

  if (down && !inPress) {
    inPress = true;
    pressAt = now;
    lastGood = {0,0};
    return false;
  }

  if (down && inPress) {
    for (int i = 0; i < 6; i++) {
      TS_Point p = ts.getPoint();
      if (!(p.x == 0 && p.y == 0)) {
        lastGood.rx = p.x;
        lastGood.ry = p.y;
      }
      delay(2);
    }
    return false;
  }

  if (!down && inPress) {
    inPress = false;
    if (now - pressAt < 120) return false;
    if (lastGood.rx == 0 && lastGood.ry == 0) return false;
    out = lastGood;
    return true;
  }

  return false;
}

static int scoreCombo(const CornerSample s[4], const CalData &c) {
  const int ex[4] = {0, TFT_W - 1, 0, TFT_W - 1};
  const int ey[4] = {0, 0, TFT_H - 1, TFT_H - 1};

  int total = 0;
  for (int i = 0; i < 4; i++) {
    TouchRaw r{ s[i].rx, s[i].ry, 0 };
    int mx, my;
    applyMapping(r, mx, my, c);
    int dx = mx - ex[i];
    int dy = my - ey[i];
    total += dx * dx + dy * dy;
  }
  return total;
}

bool touchStep8CalibrateLoop(uint8_t &stage) {
  stage = 0;
  if (!ok) return false;

  static bool started = false;
  static CornerSample samp[4];
  static int idx = 0;

  if (!started) {
    started = true;
    idx = 0;
    clearSavedCal();
    Serial.println("[CAL] Tap corners: TL -> TR -> BL -> BR");
  }

  CornerSample tmp{0,0};
  const bool got = captureCornerHold(tmp);
  stage = (uint8_t)idx;
  if (!got) return false;

  samp[idx] = tmp;
  Serial.printf("[CAL] sample %d raw=(%d,%d)\n", idx, tmp.rx, tmp.ry);
  idx++;
  stage = (uint8_t)idx;

  if (idx < 4) return false;

  int minRX = samp[0].rx, maxRX = samp[0].rx;
  int minRY = samp[0].ry, maxRY = samp[0].ry;
  for (int i = 1; i < 4; i++) {
    if (samp[i].rx < minRX) minRX = samp[i].rx;
    if (samp[i].rx > maxRX) maxRX = samp[i].rx;
    if (samp[i].ry < minRY) minRY = samp[i].ry;
    if (samp[i].ry > maxRY) maxRY = samp[i].ry;
  }

  if ((maxRX - minRX) < 500 || (maxRY - minRY) < 500) {
    Serial.println("[CAL] range too small. Tap extreme corners (very edge) and try again.");
    started = false;
    idx = 0;
    stage = 0;
    return false;
  }

  CalData best{};
  best.valid = true;
  int bestScore = INT32_MAX;

  for (int sw = 0; sw <= 1; sw++) {
    for (int ix = 0; ix <= 1; ix++) {
      for (int iy = 0; iy <= 1; iy++) {
        CalData c{};
        c.valid = true;
        c.swapXY = (sw == 1);
        c.invX = (ix == 1);
        c.invY = (iy == 1);

        if (!c.swapXY) {
          c.minX = minRX; c.maxX = maxRX;
          c.minY = minRY; c.maxY = maxRY;
        } else {
          c.minX = minRY; c.maxX = maxRY;
          c.minY = minRX; c.maxY = maxRX;
        }

        const int sc = scoreCombo(samp, c);
        if (sc < bestScore) {
          bestScore = sc;
          best = c;
        }
      }
    }
  }

  const int margin = 140;
  best.minX -= margin; best.maxX += margin;
  best.minY -= margin; best.maxY += margin;

  saveCal(best);
  cal = best;

  Serial.printf("[CAL] BEST score=%d minX=%d maxX=%d minY=%d maxY=%d swap=%d invX=%d invY=%d\n",
                bestScore, cal.minX, cal.maxX, cal.minY, cal.maxY,
                (int)cal.swapXY, (int)cal.invX, (int)cal.invY);

  return true;
}

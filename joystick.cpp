#include "joystick.h"
#include "config.h"

static bool s_btnLast = true;
static bool s_enabled = false;

static int readAdcAvg(int pin) {
  if (pin < 0) return 0;
  uint32_t sum = 0;
  for (int i = 0; i < JOY_ADC_SAMPLES; i++) {
    sum += (uint32_t)analogRead((uint8_t)pin);
  }
  return (int)(sum / (uint32_t)JOY_ADC_SAMPLES);
}

static int axisNorm(int raw, int center) {
  int d = raw - center;
  if (abs(d) < JOY_DEADZONE) return 0;

  const int maxD = 1800;
  if (d >  maxD) d =  maxD;
  if (d < -maxD) d = -maxD;

  long v = (long)d * 100L / maxD;
  if (v > 100) v = 100;
  if (v < -100) v = -100;
  return (int)v;
}

void joyBegin() {
  analogReadResolution(12);
  if (JOY_X_PIN >= 0) analogSetPinAttenuation((uint8_t)JOY_X_PIN, JOY_ADC_ATTEN);
  if (JOY_Y_PIN >= 0) analogSetPinAttenuation((uint8_t)JOY_Y_PIN, JOY_ADC_ATTEN);

  if (JOY_BTN_PIN >= 0) {
    pinMode(JOY_BTN_PIN, INPUT_PULLUP);
    s_btnLast = digitalRead(JOY_BTN_PIN);
  }

  // Default disabled; main decides enabling policy.
  s_enabled = false;
}

bool joyAvailable() {
  return (JOY_X_PIN >= 0) && (JOY_Y_PIN >= 0);
}

bool joyEnabled() {
  return s_enabled && joyAvailable();
}

void joySetEnabled(bool en) {
  s_enabled = en;
}

JoyDir joyReadDir() {
  if (!joyEnabled()) return JOY_CENTER;

  int x = readAdcAvg(JOY_X_PIN);
  int y = readAdcAvg(JOY_Y_PIN);

  int dx = x - JOY_CENTER_X;
  int dy = y - JOY_CENTER_Y;

  if (abs(dx) < JOY_DEADZONE && abs(dy) < JOY_DEADZONE) return JOY_CENTER;

  if (abs(dx) > abs(dy)) return (dx > 0) ? JOY_RIGHT : JOY_LEFT;
  return (dy > 0) ? JOY_DOWN : JOY_UP;
}

bool joyBtnPressedEdge() {
  if (!joyEnabled() || (JOY_BTN_PIN < 0)) return false;

  bool now = digitalRead(JOY_BTN_PIN); // pullup => LOW pressed
  bool pressed = (s_btnLast == true && now == false);
  s_btnLast = now;
  return pressed;
}

int joyNormX() {
  if (!joyEnabled()) return 0;
  return axisNorm(readAdcAvg(JOY_X_PIN), JOY_CENTER_X);
}

int joyNormY() {
  if (!joyEnabled()) return 0;
  return axisNorm(readAdcAvg(JOY_Y_PIN), JOY_CENTER_Y);
}
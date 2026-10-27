
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct TouchRaw {
  int x;
  int y;
  int z;
};

enum TouchEventType : uint8_t {
  TE_NONE = 0,
  TE_DOWN,
  TE_UP,
  TE_TAP,
  TE_DBL_TAP,    // NEW
  TE_LONGPRESS,
  TE_SWIPE_RIGHT,
  TE_SWIPE_LEFT,
  TE_SWIPE_UP,
  TE_SWIPE_DOWN,
  TE_DRAG,
};

struct TouchEvent {
  TouchEventType type;
  int x;
  int y;
  int dx;
  int dy;
  uint32_t ms;
  TouchRaw raw;
};

bool touchBegin();
bool touchIsOk();
bool touchIsTouched();
void touchSetResilience(bool on);

// Read raw / mapped
bool touchReadRaw(TouchRaw &out);
bool touchReadMapped(int &mx, int &my, TouchRaw &rawOut);

// Event engine (use this for real UI)
bool touchPollEvent(TouchEvent &ev);

// Reset internal gesture / debounce state (useful after wake-up so the first touch
// doesn't accidentally trigger a tap/long-press from a stale DOWN state).
void touchResetState();

// Step loops (your old step build tools)
void touchStep2RawLoggerLoop();
void touchStep3DotLoop();
void touchStep7BacklightLoop();
bool touchStep8CalibrateLoop(uint8_t &stage);
void touchStep8DebugTouchState();

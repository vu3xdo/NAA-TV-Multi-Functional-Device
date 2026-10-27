#pragma once
#include <Arduino.h>
#include <stdint.h>

enum JoyDir : uint8_t {
  JOY_CENTER = 0,
  JOY_UP,
  JOY_DOWN,
  JOY_LEFT,
  JOY_RIGHT
};

void joyBegin();

bool joyAvailable();          // pins configured?
bool joyEnabled();
void joySetEnabled(bool en);

// Digital-like direction (uses deadzone + dominant axis)
JoyDir joyReadDir();

// Edge-detect button (pullup => LOW pressed)
bool joyBtnPressedEdge();

// Normalized axes: -100..100 (deadzone => 0)
int joyNormX();
int joyNormY();

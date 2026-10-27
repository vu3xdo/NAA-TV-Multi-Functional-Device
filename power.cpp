
#include "power.h"

#include "config.h"
#include "ui.h"

#include <Arduino.h>

// ---------------- Internals ----------------
static volatile bool sWakeReq = false;
static uint32_t sLastActivityMs = 0;
static uint32_t sLastWakeEdgeMs = 0;
static bool sSleeping = false;

static void IRAM_ATTR touchIrqIsr() {
  // Touch IRQ is active-low; any falling edge means "wake requested".
  sWakeReq = true;
}

void powerBegin() {
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), touchIrqIsr, FALLING);

  const uint32_t now = millis();
  sLastActivityMs = now;
  sLastWakeEdgeMs = 0;
  sWakeReq = false;
  sSleeping = false;
}

void powerNoteActivity(uint32_t now) {
  sLastActivityMs = now;

  // If we're sleeping, wake immediately on activity.
  if (sSleeping) {
    uiSetBacklight(true);
    sSleeping = false;
  }
}

static void tryWake(uint32_t now) {
  if (!sWakeReq) return;
  sWakeReq = false;

  // Debounce IRQ edges (XPT2046 can chatter)
  if (sLastWakeEdgeMs && (now - sLastWakeEdgeMs) < POWER_WAKE_DEBOUNCE_MS) return;
  sLastWakeEdgeMs = now;

  // Mark activity; this also wakes display
  powerNoteActivity(now);
}

void powerLoop(uint32_t now) {
  // If IRQ fired while sleeping, wake ASAP.
  if (sSleeping) {
    tryWake(now);
    return;
  }

  // If not sleeping, optionally transition to sleep on idle.
  if (POWER_IDLE_BACKLIGHT_MS > 0 && (now - sLastActivityMs) >= POWER_IDLE_BACKLIGHT_MS) {
    uiSetBacklight(false);
    sSleeping = true;
    // Keep sLastActivityMs unchanged; next wake sets it.
  }

  // Even when awake, process queued wake edges so repeated touches don't lag.
  tryWake(now);
}

bool powerIsSleeping() { return sSleeping; }

void powerForceSleep(uint32_t now) {
  (void)now;
  uiSetBacklight(false);
  sSleeping = true;
}
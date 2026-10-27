#pragma once

#include <stdint.h>
#include <stdbool.h>

// Power manager for "soft sleep":
// - Backlight off after idle
// - Wake on touch IRQ / encoder / joystick activity
// - No deep-sleep (keeps WiFi alive)

void powerBegin();

// Call whenever user interacts (touch/encoder/joystick).
void powerNoteActivity(uint32_t now);

// Call from loop; manages backlight based on idle.
void powerLoop(uint32_t now);

bool powerIsSleeping();

// Force sleep (backlight off) immediately.
void powerForceSleep(uint32_t now);

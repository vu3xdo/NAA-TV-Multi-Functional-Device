#pragma once

#include "touch.h"
#include <stdint.h>

void spaceinvadersEnter();
void spaceinvadersLoop(uint32_t now);
void spaceinvadersOnTouch(const TouchEvent& ev);
void spaceinvadersOnEncoderDelta(int d);
void spaceinvadersOnEncoderClick();

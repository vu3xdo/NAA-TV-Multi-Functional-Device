#pragma once
#include <stdint.h>
#include "touch.h"

void breakoutEnter();
void breakoutLoop(uint32_t now);
void breakoutOnTouch(const TouchEvent& ev);
void breakoutOnEncoderDelta(int d);
void breakoutOnEncoderClick();

bool breakoutIsGameOver();
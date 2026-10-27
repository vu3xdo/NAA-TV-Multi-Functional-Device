
#pragma once
#include <stdint.h>
#include "touch.h"

void carEnter();
void carLoop(uint32_t now);
void carOnTouch(const TouchEvent& ev);
void carOnEncoderDelta(int d);
void carOnEncoderClick();

bool carIsGameOver();

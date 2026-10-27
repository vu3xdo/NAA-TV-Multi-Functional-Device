#pragma once
#include <stdint.h>
#include "touch.h"

void dinoEnter();
void dinoLoop(uint32_t now);
void dinoOnTouch(const TouchEvent& ev);
void dinoOnEncoderDelta(int d);
void dinoOnEncoderClick();

bool dinoIsGameOver();

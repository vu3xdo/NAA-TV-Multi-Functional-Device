
#pragma once
#include <stdint.h>
#include "touch.h"

void snakeEnter();
void snakeLoop(uint32_t now);

void snakeOnTouch(const TouchEvent& ev);
void snakeOnEncoderDelta(int d);
void snakeOnEncoderClick();

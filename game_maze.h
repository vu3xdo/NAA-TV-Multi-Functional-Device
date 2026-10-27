
#pragma once
#include <stdint.h>
#include "touch.h"

void mazeEnter();
void mazeLoop(uint32_t now);
void mazeOnTouch(const TouchEvent& ev);
void mazeOnEncoderDelta(int d);
void mazeOnEncoderClick();

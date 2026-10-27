#pragma once

#include "touch.h"
#include <stdint.h>

void painterEnter();
void painterLoop(uint32_t now);
void painterOnTouch(const TouchEvent& ev);
void painterOnEncoderDelta(int d);
void painterOnEncoderClick();

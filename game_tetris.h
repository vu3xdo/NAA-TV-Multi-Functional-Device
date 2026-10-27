#pragma once

#include "touch.h"
#include <stdint.h>

void tetrisEnter();
void tetrisLoop(uint32_t now);
void tetrisOnTouch(const TouchEvent& ev);
void tetrisOnEncoderDelta(int d);
void tetrisOnEncoderClick();

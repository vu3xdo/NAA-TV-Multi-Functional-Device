
/* -------------------- games.h -------------------- */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "touch.h"

enum GameScreenMode : uint8_t {
  GM_MENU = 0,

  // Existing
  GM_DICE,
  GM_REACTION,
  GM_PONG,

  // New
  GM_BREAKOUT,
  GM_DINO,
  GM_CAR,

  // Added
  GM_SNAKE,
  GM_MAZE,
  GM_TETRIS,
  GM_SPACEINVADERS,
  GM_PAINTER,
};

void gamesBegin();
void gamesEnter();
void gamesLeave();

void gamesLoop(uint32_t now);
void gamesStartByIndex(int index);

// Input routing (called by main when SCR_GAMES)
void gamesOnTouch(const TouchEvent& ev);
void gamesOnEncoderDelta(int d);
void gamesOnEncoderClick();

// Helpers used by main UI
bool gamesInGame();
void gamesExitToMenu();


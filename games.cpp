
#include "games.h"

#include "ui.h"
#include "joystick.h"
#include "touch.h"

#include <Arduino.h>
#include <string.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include "game_breakout.h"
#include "game_dino.h"
#include "game_car.h"
#include "game_snake.h"
#include "game_maze.h"
#include "game_tetris.h"
#include "game_spaceinvaders.h"
#include "game_painter.h"

// ============================
// Screen helpers
// ============================
static inline int SW() { return (int)uiTft().width(); }
static inline int SH() { return (int)uiTft().height(); }
static inline int bodyY0() { return uiBodyY0(); }
static inline int bodyH()  { return uiBodyH(); }
static inline int bodyY1() { return bodyY0() + bodyH(); }
static inline int footerY(){ return SH() - uiFooterH(); }
static inline int clampi(int v,int lo,int hi){ return (v<lo)?lo:(v>hi)?hi:v; }

static void clearBody() {
  uiTft().fillRect(0, bodyY0(), SW(), bodyH(), uiColorBg());
}

// Footer (built-in font; never clips)
static void drawFooterHint(const char* s) {
  auto& tft = uiTft();
  const int fy = footerY();
  tft.fillRect(0, fy, SW(), uiFooterH(), uiColorBg());
  tft.drawFastHLine(0, fy, SW(), uiColorDivider());

  tft.setFont(nullptr);
  tft.setTextSize(1);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(8, fy + (uiFooterH()/2) + 4);
  tft.print(s ? s : "");
}

// ==========================================================
// MENU (smooth partial redraw, no whole-screen flash)
// ==========================================================
struct GameItem { const char* name; GameScreenMode mode; };

static const GameItem ITEMS[] = {
  { "Dice",     GM_DICE },
  { "Reaction", GM_REACTION },
  { "Pong",     GM_PONG },
  { "Breakout", GM_BREAKOUT },
  { "Dino",     GM_DINO },
  { "Car",      GM_CAR },
  { "Snake",    GM_SNAKE },  // NEW
  { "Maze",     GM_MAZE },
  { "Tetris",   GM_TETRIS },
  { "Space Invaders", GM_SPACEINVADERS },
  { "Pixel Art", GM_PAINTER },
};
static const int ITEM_COUNT = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

static GameScreenMode sMode = GM_MENU;

static int  sSel = 0;
static int  sOffset = 0;
static bool sMenuFrameDrawn = false;
static bool sMenuFullDirty  = true;
static int  sPrevSel = -1;
static int  sPrevOff = -1;

static const int MENU_TOP_PAD = 22;
static const int MENU_BOT_PAD = 22;
static const int MENU_ROW_H   = 28;
static const int MENU_ROW_GAP = 6;
static const int MENU_STEP    = MENU_ROW_H + MENU_ROW_GAP;

static int menuVisibleCount() {
  int avail = bodyH() - MENU_TOP_PAD - MENU_BOT_PAD;
  int n = avail / MENU_STEP;
  if (n < 1) n = 1;
  if (n > ITEM_COUNT) n = ITEM_COUNT;
  return n;
}

static int menuRowTopYVis(int visIndex) {
  return bodyY0() + MENU_TOP_PAD + visIndex * MENU_STEP;
}

static void ensureSelVisible() {
  const int vis = menuVisibleCount();
  if (sSel < sOffset) sOffset = sSel;
  if (sSel >= sOffset + vis) sOffset = sSel - vis + 1;

  if (sOffset < 0) sOffset = 0;
  const int maxOff = (ITEM_COUNT > vis) ? (ITEM_COUNT - vis) : 0;
  if (sOffset > maxOff) sOffset = maxOff;
}

static void drawMenuScrollHints() {
  auto& tft = uiTft();

  // top strip
  tft.fillRect(0, bodyY0(), SW(), MENU_TOP_PAD, uiColorBg());
  tft.setFont(nullptr);
  tft.setTextSize(1);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(10, bodyY0() + 14);
  tft.print("Tap top/bottom to scroll");

  if (sOffset > 0) {
    int ax = SW() - 14;
    int ay = bodyY0() + 8;
    tft.fillTriangle(ax-6, ay+4, ax+6, ay+4, ax, ay-2, uiColorAccent());
  }

  // bottom strip
  int by0 = bodyY1() - MENU_BOT_PAD;
  tft.fillRect(0, by0, SW(), MENU_BOT_PAD, uiColorBg());
  if (sOffset + menuVisibleCount() < ITEM_COUNT) {
    int ax = SW() - 14;
    int ay = by0 + 10;
    tft.fillTriangle(ax-6, ay-4, ax+6, ay-4, ax, ay+2, uiColorAccent());
  }
}

static void drawMenuRowByIndex(int itemIndex, bool hi) {
  auto& tft = uiTft();

  int visIndex = itemIndex - sOffset;
  if (visIndex < 0 || visIndex >= menuVisibleCount()) return;

  const int y = menuRowTopYVis(visIndex);
  const uint16_t bg = hi ? uiColorSelBg() : uiColorBg();
  const uint16_t fg = hi ? uiColorAccent() : uiColorText();

  tft.fillRect(10, y, SW() - 20, MENU_ROW_H, bg);
  tft.drawRect(10, y, SW() - 20, MENU_ROW_H, hi ? uiColorAccent() : uiColorDivider());

  tft.setFont(&FreeSans12pt7b);
  tft.setTextColor(fg, bg);
  tft.setCursor(18, y + 21);
  tft.print(ITEMS[itemIndex].name);
}

static void redrawAllMenuRows() {
  auto& tft = uiTft();
  const int rowsY = bodyY0() + MENU_TOP_PAD;
  const int rowsH = bodyH() - MENU_TOP_PAD - MENU_BOT_PAD;
  tft.fillRect(0, rowsY, SW(), rowsH, uiColorBg());

  const int vis = menuVisibleCount();
  for (int i = 0; i < vis; i++) {
    int idx = sOffset + i;
    if (idx >= ITEM_COUNT) break;
    drawMenuRowByIndex(idx, idx == sSel);
  }
}

static void drawMenuFrameOnce() {
  if (sMenuFrameDrawn) return;

  uiShowScreenTitle("GAMES");
  clearBody();
  drawMenuScrollHints();
  drawFooterHint("Tap:Play  Enc:Select/Play  Long:Home");

  sMenuFrameDrawn = true;
}

static void menuRenderIfNeeded() {
  if (sMode != GM_MENU) return;

  drawMenuFrameOnce();
  ensureSelVisible();

  if (sMenuFullDirty) {
    drawMenuScrollHints();
    redrawAllMenuRows();
    sMenuFullDirty = false;
    sPrevOff = sOffset;
    sPrevSel = sSel;
    return;
  }

  if (sPrevOff != sOffset) {
    drawMenuScrollHints();
    redrawAllMenuRows();
    sPrevOff = sOffset;
    sPrevSel = sSel;
    return;
  }

  if (sPrevSel != sSel) {
    if (sPrevSel >= 0) drawMenuRowByIndex(sPrevSel, false);
    drawMenuRowByIndex(sSel, true);
    sPrevSel = sSel;
  }
}

static void menuMoveSel(int d) {
  int oldOff = sOffset;
  int oldSel = sSel;

  sSel = clampi(sSel + d, 0, ITEM_COUNT - 1);
  ensureSelVisible();

  if (sOffset != oldOff) sMenuFullDirty = true;
  else if (sSel != oldSel) {
    // selection-only; handled in menuRenderIfNeeded()
  }
}

static void menuScroll(int dir) {
  const int vis = menuVisibleCount();
  const int maxOff = (ITEM_COUNT > vis) ? (ITEM_COUNT - vis) : 0;

  sOffset = clampi(sOffset + dir, 0, maxOff);

  if (sSel < sOffset) sSel = sOffset;
  if (sSel >= sOffset + vis) sSel = sOffset + vis - 1;

  sMenuFullDirty = true;
}

// ==========================================================
// DICE (embedded)
// ==========================================================
static int diceVal = 1;

static void diceDrawStatic() {
  uiShowScreenTitle("DICE");
  clearBody();
  drawFooterHint("Tap/Press:Roll  Long:Menu");
}

static void diceDrawFace() {
  auto& tft = uiTft();
  const int box = 92;
  const int x = (SW() - box) / 2;
  const int y = bodyY0() + (bodyH() - box) / 2;

  tft.fillRect(x - 10, y - 10, box + 20, box + 20, uiColorBg());
  tft.fillRoundRect(x, y, box, box, 10, uiColorText());
  tft.drawRoundRect(x, y, box, box, 10, uiColorAccent());

  auto pip = [&](int px, int py) { tft.fillCircle(px, py, 6, uiColorBg()); };

  const int cx = x + box / 2;
  const int cy = y + box / 2;
  const int d  = 22;
  auto p = [&](int ox, int oy){ pip(cx + ox, cy + oy); };

  switch (diceVal) {
    case 1: p(0,0); break;
    case 2: p(-d,-d); p(d,d); break;
    case 3: p(-d,-d); p(0,0); p(d,d); break;
    case 4: p(-d,-d); p(d,-d); p(-d,d); p(d,d); break;
    case 5: p(-d,-d); p(d,-d); p(0,0); p(-d,d); p(d,d); break;
    case 6: p(-d,-d); p(d,-d); p(-d,0); p(d,0); p(-d,d); p(d,d); break;
  }
}

static void diceEnter() { diceVal = 1; diceDrawStatic(); diceDrawFace(); }
static void diceRoll()  { diceVal = 1 + (int)(esp_random() % 6); diceDrawFace(); }

// ==========================================================
// REACTION (embedded)
// ==========================================================
static bool     reactArmed=false;
static bool     reactGoShown=false;
static uint32_t reactGoAt=0;

static void reactionDrawMessage(const char* big, const char* small) {
  uiShowScreenTitle("REACTION");
  clearBody();

  auto& tft = uiTft();
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(uiColorText(), uiColorBg());

  int16_t x1,y1; uint16_t w,h;
  tft.getTextBounds(big ? big : "", 0, 0, &x1, &y1, &w, &h);
  int x = (SW() - (int)w) / 2;
  if (x < 12) x = 12;

  tft.setCursor(x, bodyY0() + 64);
  tft.print(big ? big : "");

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(12, bodyY0() + 98);
  tft.print(small ? small : "");

  drawFooterHint("Tap/Press:start  Long:Menu");
}

static void reactionReset() {
  reactArmed=false; reactGoShown=false; reactGoAt=0;
  reactionDrawMessage("Tap to START", "Wait for GO then tap fast");
}
static void reactionStart() {
  reactArmed=true; reactGoShown=false;
  uint32_t now=millis();
  reactGoAt = now + 1200 + (esp_random() % 2300);
  reactionDrawMessage("Get ready...", "Wait for GO");
}
static void reactionShowGo() { reactGoShown=true; reactionDrawMessage("GO!", "Tap NOW!"); }
static void reactionTooEarly() {
  reactArmed=false; reactGoShown=false;
  reactionDrawMessage("TOO EARLY!", "Tap to try again");
}
static void reactionShowResult(uint32_t ms) {
  reactArmed=false; reactGoShown=false;
  char buf[32];
  snprintf(buf, sizeof(buf), "Time: %lums", (unsigned long)ms);
  reactionDrawMessage("DONE", buf);
}
static void reactionTap() {
  uint32_t now=millis();
  if(!reactArmed){ reactionStart(); return; }
  if(now < reactGoAt){ reactionTooEarly(); return; }
  reactionShowResult(now - reactGoAt);
}

// ==========================================================
// PONG (embedded, non-flicker dirty redraw)
// ==========================================================
static bool     pongOver=false;
static uint32_t pongLast=0;

static const int PONG_PAD=10;
static const int PONG_BALL_R=4;
static const int PONG_PADDLE_W=60;
static const int PONG_PADDLE_H=8;

static int pongCx0(){ return PONG_PAD; }
static int pongCy0(){ return bodyY0()+PONG_PAD; }
static int pongCw(){  return SW()-2*PONG_PAD; }
static int pongCh(){  return bodyH()-2*PONG_PAD; }
static int pongPaddleY(){ return bodyY0()+bodyH()-26; }

static int pongMinX(){ return pongCx0()+PONG_BALL_R+2; }
static int pongMaxX(){ return pongCx0()+pongCw()-PONG_BALL_R-2; }
static int pongMinY(){ return pongCy0()+PONG_BALL_R+2; }
static int pongMaxY(){ return pongCy0()+pongCh()-PONG_BALL_R-2; }

static int pongPaddleX=0, pongLastPaddleX=0;
static int pongBallX=0, pongBallY=0, pongLastBallX=0, pongLastBallY=0;
static int pongVX=2, pongVY=2;

static void pongDrawCourt(){
  uiTft().drawRect(pongCx0(), pongCy0(), pongCw(), pongCh(), uiColorDivider());
}

static void pongDrawStatic(){
  uiShowScreenTitle("PONG");
  clearBody();
  pongDrawCourt();
  drawFooterHint("Joy/Touch:Move  Tap/Press:Restart  Long:Menu");
}

static void pongEraseBall(int x,int y){
  uiTft().fillCircle(x,y,PONG_BALL_R+1, uiColorBg());
  pongDrawCourt(); // keeps border intact (no wall holes)
}
static void pongDrawBall(int x,int y){
  uiTft().fillCircle(x,y,PONG_BALL_R, uiColorText());
}

static void pongErasePaddle(int cx){
  auto& tft=uiTft();
  int px=clampi(cx-PONG_PADDLE_W/2, pongCx0()+2, pongCx0()+pongCw()-2-PONG_PADDLE_W);
  tft.fillRect(px-1, pongPaddleY()-1, PONG_PADDLE_W+2, PONG_PADDLE_H+2, uiColorBg());
  pongDrawCourt();
}
static void pongDrawPaddle(int cx){
  auto& tft=uiTft();
  int px=clampi(cx-PONG_PADDLE_W/2, pongCx0()+2, pongCx0()+pongCw()-2-PONG_PADDLE_W);
  tft.fillRect(px, pongPaddleY(), PONG_PADDLE_W, PONG_PADDLE_H, uiColorAccent());
}

static void pongOverlay(){
  auto& tft=uiTft();
  int y=bodyY0()+bodyH()/2-10;
  tft.fillRect(40,y,SW()-80,26,uiColorBg());
  tft.drawRect(40,y,SW()-80,26,uiColorDivider());
  tft.setFont(nullptr); tft.setTextSize(1);
  tft.setTextColor(ILI9341_RED, uiColorBg());
  tft.setCursor(58,y+8);
  tft.print("GAME OVER");
}

static void pongReset(){
  pongOver=false;
  pongLast=0;

  pongPaddleX = SW()/2;
  pongLastPaddleX = pongPaddleX;

  pongBallX = clampi(SW()/2, pongMinX(), pongMaxX());
  pongBallY = clampi(pongCy0()+30, pongMinY(), pongMaxY());
  pongLastBallX = pongBallX;
  pongLastBallY = pongBallY;

  pongVX = (esp_random() & 1) ? 2 : -2;
  pongVY = 2;

  pongDrawStatic();
  pongDrawPaddle(pongPaddleX);
  pongDrawBall(pongBallX, pongBallY);
}

static void pongStep(uint32_t now){
  if(pongOver) return;
  if(pongLast && (now-pongLast)<18) return;
  pongLast=now;

  // paddle input
  if(joyEnabled()){
    int nx=joyNormX();
    if(abs(nx)>8) pongPaddleX += (nx/14);
  }
  pongPaddleX = clampi(pongPaddleX, pongCx0()+8, pongCx0()+pongCw()-8);

  // ball move
  pongBallX += pongVX;
  pongBallY += pongVY;

  // bounce + clamp (prevents oscillation)
  if(pongBallX <= pongMinX()){ pongBallX = pongMinX(); pongVX = -pongVX; }
  if(pongBallX >= pongMaxX()){ pongBallX = pongMaxX(); pongVX = -pongVX; }
  if(pongBallY <= pongMinY()){ pongBallY = pongMinY(); pongVY = -pongVY; }

  // paddle collision
  int px=clampi(pongPaddleX-PONG_PADDLE_W/2, pongCx0()+2, pongCx0()+pongCw()-2-PONG_PADDLE_W);
  int py=pongPaddleY();
  if(pongBallY>=py-PONG_BALL_R-2 && pongBallY<=py+PONG_PADDLE_H+2 && pongBallX>=px && pongBallX<=px+PONG_PADDLE_W){
    pongVY = -abs(pongVY);
    int off = pongBallX - (px + PONG_PADDLE_W/2);
    if(off < -20) pongVX = -3;
    else if(off > 20) pongVX = 3;
    else pongVX = (pongVX<0)?-2:2;
  }

  // miss bottom
  if(pongBallY >= pongMaxY()) pongOver=true;

  // dirty redraw
  pongEraseBall(pongLastBallX, pongLastBallY);
  if(pongLastPaddleX != pongPaddleX) pongErasePaddle(pongLastPaddleX);

  pongDrawPaddle(pongPaddleX);
  pongDrawBall(pongBallX, pongBallY);

  pongLastBallX = pongBallX;
  pongLastBallY = pongBallY;
  pongLastPaddleX = pongPaddleX;

  if(pongOver) pongOverlay();
}

static void pongOnTouchInternal(const TouchEvent& ev){
  if(ev.type==TE_TAP){
    pongReset();
    return;
  }
  if(ev.type==TE_DOWN){
    if(ev.y>=bodyY0() && ev.y<footerY()){
      pongPaddleX = clampi(ev.x, pongCx0()+8, pongCx0()+pongCw()-8);
    }
  }
}
static void pongOnEncoderDeltaInternal(int d){
  if(!d) return;
  pongPaddleX = clampi(pongPaddleX + d*6, pongCx0()+8, pongCx0()+pongCw()-8);
}
static void pongOnEncoderClickInternal(){
  pongReset();
}

// ==========================================================
// Mode entry + routing
// ==========================================================
static void enterMode(GameScreenMode m) {
  sMode = m;

  if (m == GM_MENU) {
    return;
  }

  switch (m) {
    case GM_DICE:     diceEnter();     break;
    case GM_REACTION: reactionReset(); break;
    case GM_PONG:     pongReset();     break;

    case GM_BREAKOUT: breakoutEnter(); break;
    case GM_DINO:     dinoEnter();     break;
    case GM_CAR:      carEnter();      break;

    case GM_SNAKE:    snakeEnter();    break;
    case GM_MAZE:     mazeEnter();     break;

    case GM_TETRIS:   tetrisEnter();   break;
    case GM_SPACEINVADERS: spaceinvadersEnter(); break;
    case GM_PAINTER:  painterEnter();  break;

    default: enterMode(GM_MENU); break;
  }
}

// ==========================================================
// Public API
// ==========================================================
void gamesBegin() {
  joyBegin();
  joySetEnabled(false);
}

void gamesEnter() {
  joySetEnabled(true);
  sSel = 0;
  sOffset = 0;
  enterMode(GM_MENU);
}

void gamesLeave() {
  joySetEnabled(false);
}

bool gamesInGame() { return (sMode != GM_MENU); }
void gamesExitToMenu() { enterMode(GM_MENU); }

void gamesLoop(uint32_t now) {
  // The GM_MENU is rendered by NAA_TV.ino via uiGamesMenuDrawFull()
  // Do not call the internal menuRenderIfNeeded() here — it would overwrite the standard list menu
  if (sMode == GM_MENU) return;

  // Reaction needs its own GO timing
  if (sMode == GM_REACTION) {
    if (reactArmed && !reactGoShown && now >= reactGoAt) reactionShowGo();
    return;
  }

  if (sMode == GM_PONG) { pongStep(now); return; }

  if (sMode == GM_BREAKOUT) { breakoutLoop(now); return; }
  if (sMode == GM_DINO)     { dinoLoop(now);     return; }
  if (sMode == GM_CAR)      { carLoop(now);      return; }

  if (sMode == GM_SNAKE)    { snakeLoop(now);    return; }
  if (sMode == GM_MAZE)     { mazeLoop(now);     return; }

  if (sMode == GM_TETRIS)         { tetrisLoop(now);         return; }
  if (sMode == GM_SPACEINVADERS)  { spaceinvadersLoop(now);  return; }
  if (sMode == GM_PAINTER)        { painterLoop(now);        return; }
}

void gamesStartByIndex(int index) {
  if (index >= 0 && index < ITEM_COUNT) {
    sSel = index;
    enterMode(ITEMS[sSel].mode);
  }
}

void gamesOnTouch(const TouchEvent& ev) {
  if (ev.type == TE_LONGPRESS) return;

  if (sMode == GM_MENU) return; // Menu is handled by NAA_TV.ino

  if (sMode == GM_DICE) {
    if (ev.type == TE_TAP) diceRoll();
    return;
  }

  if (sMode == GM_REACTION) {
    if (ev.type == TE_TAP) reactionTap();
    return;
  }

  if (sMode == GM_PONG) {
    pongOnTouchInternal(ev);
    return;
  }

  if (sMode == GM_BREAKOUT) { breakoutOnTouch(ev); return; }
  if (sMode == GM_DINO)     { dinoOnTouch(ev);     return; }
  if (sMode == GM_CAR)      { carOnTouch(ev);      return; }

  if (sMode == GM_SNAKE)    { snakeOnTouch(ev);    return; }
  if (sMode == GM_MAZE)     { mazeOnTouch(ev);     return; }

  if (sMode == GM_TETRIS)         { tetrisOnTouch(ev);         return; }
  if (sMode == GM_SPACEINVADERS)  { spaceinvadersOnTouch(ev);  return; }
  if (sMode == GM_PAINTER)        { painterOnTouch(ev);        return; }
}

void gamesOnEncoderDelta(int d) {
  if (!d) return;

  if (sMode == GM_MENU) { menuMoveSel(d); return; }

  if (sMode == GM_PONG) { pongOnEncoderDeltaInternal(d); return; }

  if (sMode == GM_BREAKOUT) { breakoutOnEncoderDelta(d); return; }
  if (sMode == GM_DINO)     { dinoOnEncoderDelta(d);     return; }
  if (sMode == GM_CAR)      { carOnEncoderDelta(d);      return; }

  if (sMode == GM_SNAKE)    { snakeOnEncoderDelta(d);    return; }
  if (sMode == GM_MAZE)     { mazeOnEncoderDelta(d);     return; }

  if (sMode == GM_TETRIS)         { tetrisOnEncoderDelta(d);         return; }
  if (sMode == GM_SPACEINVADERS)  { spaceinvadersOnEncoderDelta(d);  return; }
  if (sMode == GM_PAINTER)        { painterOnEncoderDelta(d);        return; }
}

void gamesOnEncoderClick() {
  if (sMode == GM_MENU) {
    enterMode(ITEMS[sSel].mode);
    return;
  }

  if (sMode == GM_DICE)     { diceRoll(); return; }
  if (sMode == GM_REACTION) { reactionTap(); return; }
  if (sMode == GM_PONG)     { pongOnEncoderClickInternal(); return; }

  if (sMode == GM_BREAKOUT) { breakoutOnEncoderClick(); return; }
  if (sMode == GM_DINO)     { dinoOnEncoderClick();     return; }
  if (sMode == GM_CAR)      { carOnEncoderClick();      return; }

  if (sMode == GM_SNAKE)    { snakeOnEncoderClick();    return; }
  if (sMode == GM_MAZE)     { mazeOnEncoderClick();     return; }

  if (sMode == GM_TETRIS)         { tetrisOnEncoderClick();         return; }
  if (sMode == GM_SPACEINVADERS)  { spaceinvadersOnEncoderClick();  return; }
  if (sMode == GM_PAINTER)        { painterOnEncoderClick();        return; }
}

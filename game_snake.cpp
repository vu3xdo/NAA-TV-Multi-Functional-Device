#include "game_snake.h"
#include "ui.h"
#include "joystick.h"
#include "storage.h"
#include <Arduino.h>

// ============================================================
//  SNAKE — Professional glitch-free implementation
//
//  Rendering contract:
//    • Only the head cell and the vacated tail cell are
//      redrawn each step — everything else is untouched.
//    • eraseCell fills the FULL CELL (no 1-px margin issues).
//    • Court frame drawn once after full redraws only.
//    • Food blink redraws one cell only.
//    • No mid-game fillRect(full body area).
// ============================================================

static inline int SW()      { return (int)uiTft().width(); }
static inline int SH()      { return (int)uiTft().height(); }
static inline int bodyY0()  { return uiBodyY0(); }
static inline int bodyH()   { return uiBodyH(); }
static inline int footerY() { return SH()-uiFooterH(); }

// ---- Palette ----
static const uint16_t COL_HEAD  = 0x07FF;  // cyan
static const uint16_t COL_BODY  = 0x0600;  // dark green
static const uint16_t COL_TAIL  = 0x02A0;  // faded green
static const uint16_t COL_FOOD  = 0xFBE0;  // gold
static const uint16_t COL_FOOD2 = 0xF800;  // red blink
static const uint16_t COL_WALL  = 0x4208;  // grey
static const uint16_t COL_LV    = 0xFFE0;  // yellow level

// ---- Grid ----
static const int PAD  = 6;
static const int CELL = 9;
static int gX0, gY0, gW, gH;   // pixel origin and size of grid
static int COLS, ROWS;

static inline int cellX(int c) { return gX0+c*CELL; }
static inline int cellY(int r) { return gY0+r*CELL; }
static uint16_t  idxRC(int r,int c)             { return (uint16_t)(r*COLS+c); }
static void      rcOf(uint16_t i,int& r,int& c) { r=i/COLS; c=i%COLS; }

// ---- Ring buffer ----
static uint16_t* sBody    = nullptr;
static int       sBodyCap = 0;
static int       sLen=0, sHead=0, sTail=0;

static bool bodyContains(uint16_t idx) {
  int i=sTail;
  for (int k=0;k<sLen;k++){
    if (sBody[i]==idx) return true;
    i=(i+1)%sBodyCap;
  }
  return false;
}

// ---- Direction ----
enum Dir:uint8_t { D_UP=0, D_RIGHT=1, D_DOWN=2, D_LEFT=3 };
static Dir sDir, sNextDir;
static inline bool isOpp(Dir a,Dir b){ return ((int)a+2)%4==(int)b; }

// ---- State ----
static bool     sPaused,sOver,sOverDrawn;
static uint32_t sAccum,sLastMs,sTickMs;
static int      sScore,sHi,sLevel;
static uint16_t sFood;
static bool     sFoodBlink;
static uint32_t sFoodLast;

// ============================================================
//  Cell drawing (1-px inner margin for grid feel)
// ============================================================
static void drawCell(uint16_t idx, uint16_t col) {
  int r,c; rcOf(idx,r,c);
  uiTft().fillRect(cellX(c)+1, cellY(r)+1, CELL-2, CELL-2, col);
}
static void eraseCell(uint16_t idx) {
  int r,c; rcOf(idx,r,c);
  uiTft().fillRect(cellX(c), cellY(r), CELL, CELL, uiColorBg());
}
static void drawCourtFrame() {
  uiTft().drawRect(gX0-1, gY0-1, gW+2, gH+2, COL_WALL);
}

// Body colour: head=cyan, body=green, tail=faded
static uint16_t bodyCol(int posFromHead) {
  if (posFromHead==0) return COL_HEAD;
  if (sLen<=3)        return COL_BODY;
  return (posFromHead < sLen/2) ? COL_BODY : COL_TAIL;
}

static void redrawFull() {
  int i=sTail;
  for (int k=0;k<sLen;k++) {
    drawCell(sBody[i], bodyCol(sLen-1-k));
    i=(i+1)%sBodyCap;
  }
}

// ============================================================
//  HUD / overlays
// ============================================================
static void drawHUD() {
  auto& T=uiTft();
  T.fillRect(PAD, bodyY0()+2, SW()-2*PAD, 16, uiColorBg());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(), uiColorBg());
  T.setCursor(PAD, bodyY0()+5);
  T.print("Score "); T.print(sScore);
  T.print("  Hi "); T.print(sHi);
  T.setTextColor(COL_LV, uiColorBg());
  char b[12]; snprintf(b,sizeof(b),"  Lv%d",sLevel); T.print(b);
  if (sPaused) { T.setTextColor(uiColorSub(),uiColorBg()); T.print("  PAUSED"); }
}

static void overlayC(const char* l1,const char* l2) {
  auto& T=uiTft();
  int y=bodyY0()+bodyH()/2-20;
  T.fillRoundRect(18,y,SW()-36,48,6,uiColorBg());
  T.drawRoundRect(18,y,SW()-36,48,6,uiColorAccent());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorAccent(),uiColorBg());
  T.setCursor(28,y+12); T.print(l1?l1:"");
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(28,y+28); T.print(l2?l2:"");
}

static void footerTxt(const char* s) {
  auto& T=uiTft();
  int fy=footerY();
  T.fillRect(0,fy,SW(),uiFooterH(),uiColorBg());
  T.drawFastHLine(0,fy,SW(),uiColorDivider());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(8,fy+uiFooterH()/2+4);
  T.print(s?s:"");
}

// ============================================================
//  Food
// ============================================================
static void placeFood() {
  for (int t=0;t<300;t++) {
    uint16_t idx=idxRC((int)(esp_random()%ROWS),(int)(esp_random()%COLS));
    if (!bodyContains(idx)) {
      sFood=idx;
      drawCell(sFood,COL_FOOD);
      sFoodBlink=false; sFoodLast=millis();
      return;
    }
  }
}
static void tickFood(uint32_t now) {
  if (now-sFoodLast>=300) {
    sFoodLast=now; sFoodBlink=!sFoodBlink;
    drawCell(sFood, sFoodBlink?COL_FOOD2:COL_FOOD);
  }
}

// ============================================================
//  Level
// ============================================================
static void updateLevel() {
  int lv=1+sScore/5;
  if (lv>sLevel) {
    sLevel=lv;
    sTickMs=(uint32_t)max(55, 120-(sLevel-1)*8);
    overlayC("LEVEL UP!","");
    delay(400);
    uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
    drawCourtFrame(); redrawFull(); placeFood(); drawHUD();
  }
}

// ============================================================
//  Grid init
// ============================================================
static void initGrid() {
  int scoreH=20;
  gX0=PAD; gY0=bodyY0()+scoreH;
  gW=SW()-2*PAD; gH=bodyH()-scoreH-4;
  COLS=gW/CELL; ROWS=gH/CELL;
  if (COLS<10) COLS=10;
  if (ROWS<10) ROWS=10;
  gW=COLS*CELL; gH=ROWS*CELL;
  gX0=(SW()-gW)/2;
  gY0=gY0+((bodyY0()+bodyH()-gY0-gH)/2);
}

// ============================================================
//  Input
// ============================================================
static void applyDir() { if(!isOpp(sDir,sNextDir)) sDir=sNextDir; }
static void setDirJoy() {
  if (!joyEnabled()) return;
  JoyDir d=joyReadDir();
  if(d==JOY_UP)    sNextDir=D_UP;
  if(d==JOY_DOWN)  sNextDir=D_DOWN;
  if(d==JOY_LEFT)  sNextDir=D_LEFT;
  if(d==JOY_RIGHT) sNextDir=D_RIGHT;
}
static void setDirTap(int x,int y) {
  if (y>=footerY()||x<gX0||x>=gX0+gW||y<gY0||y>=gY0+gH) return;
  int dx=x-(gX0+gW/2), dy=y-(gY0+gH/2);
  if (abs(dx)>abs(dy)) sNextDir=(dx<0)?D_LEFT:D_RIGHT;
  else                 sNextDir=(dy<0)?D_UP  :D_DOWN;
}

// ============================================================
//  Reset
// ============================================================
static void resetSnake() {
  sPaused=false; sOver=false; sOverDrawn=false;
  sScore=0; sLevel=1; sTickMs=120;
  sDir=D_RIGHT; sNextDir=D_RIGHT;
  sAccum=0; sLastMs=millis();

  int sr=ROWS/2, sc=COLS/2;
  sLen=0; sHead=0; sTail=0;

  auto push=[&](uint16_t idx){
    if(sLen==0){sHead=0;sTail=0;sBody[0]=idx;sLen=1;return;}
    sHead=(sHead+1)%sBodyCap;
    sBody[sHead]=idx; sLen++;
  };
  push(idxRC(sr,sc-1));
  push(idxRC(sr,sc));
  push(idxRC(sr,sc+1));
  push(idxRC(sr,sc+2));

  uiShowScreenTitle("SNAKE");
  uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
  drawCourtFrame(); redrawFull(); placeFood(); drawHUD();
  footerTxt("Restart|Pause|Tap:Steer");
}

// ============================================================
//  Step
// ============================================================
static void stepSnake() {
  applyDir();
  uint16_t hIdx=sBody[sHead];
  int hr,hc; rcOf(hIdx,hr,hc);
  int nr=hr,nc=hc;
  switch(sDir){
    case D_UP:    nr--; break;
    case D_RIGHT: nc++; break;
    case D_DOWN:  nr++; break;
    case D_LEFT:  nc--; break;
  }
  if(nr<0||nr>=ROWS||nc<0||nc>=COLS){ sOver=true; return; }

  uint16_t nIdx=idxRC(nr,nc);
  bool grow=(nIdx==sFood);
  uint16_t tailIdx=sBody[sTail];

  if(bodyContains(nIdx)){
    if(!(!grow && nIdx==tailIdx)){ sOver=true; return; }
  }

  // Advance
  sHead=(sHead+1)%sBodyCap;
  sBody[sHead]=nIdx; sLen++;

  // Repaint old head as body
  if(sLen>=2){
    int prev=(sHead-1+sBodyCap)%sBodyCap;
    drawCell(sBody[prev], COL_BODY);
  }
  // Draw new head
  drawCell(nIdx, COL_HEAD);

  if(grow){
    sScore++;
    if(sScore>sHi){sHi=sScore;storageSetI32("snake_hi",sHi);}
    updateLevel(); placeFood(); drawHUD();
    return;
  }

  // Remove tail
  uint16_t oldTail=sBody[sTail];
  sTail=(sTail+1)%sBodyCap; sLen--;
  if(oldTail!=sFood) eraseCell(oldTail);

  // Paint new tail tip in dim colour
  if(sLen>=2) drawCell(sBody[sTail], COL_TAIL);
}

// ============================================================
//  Public API
// ============================================================
void snakeEnter() {
  storageBegin(); sHi=storageGetI32("snake_hi",0);
  initGrid();
  int cap=COLS*ROWS; if(cap<100) cap=100;
  if(sBody==nullptr||sBodyCap!=cap){
    if(sBody){free(sBody);sBody=nullptr;}
    sBody=(uint16_t*)malloc(sizeof(uint16_t)*cap);
    sBodyCap=(sBody!=nullptr)?cap:0;
  }
  if(!sBody||sBodyCap<50){
    uiShowScreenTitle("SNAKE");
    uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
    overlayC("NO MEM","malloc failed");
    sOver=true; sOverDrawn=true; return;
  }
  resetSnake();
}

void snakeLoop(uint32_t now) {
  setDirJoy();

  if(sOver){
    if(!sOverDrawn){
      char b[28]; snprintf(b,sizeof(b),"Score %d  Hi %d",sScore,sHi);
      overlayC("GAME OVER",b); sOverDrawn=true;
    }
    return;
  }

  tickFood(now);

  if(sLastMs==0) sLastMs=now;
  uint32_t dt=now-sLastMs; sLastMs=now;
  if(dt>300) dt=300;
  if(sPaused) return;

  sAccum+=dt;
  while(sAccum>=sTickMs){
    sAccum-=sTickMs;
    stepSnake();
    if(sOver){ sOverDrawn=false; break; }
  }
}

void snakeOnTouch(const TouchEvent& ev) {
  if(ev.type==TE_LONGPRESS) return;
  if(sOver){ if(ev.type==TE_TAP) resetSnake(); return; }
  if(ev.type==TE_TAP){
    if(ev.y>=footerY()){
      if(ev.x<SW()/3){resetSnake();return;}
      if(ev.x<2*SW()/3){sPaused=!sPaused;drawHUD();return;}
    }
    if(!sPaused) setDirTap(ev.x,ev.y);
    return;
  }
  if(!sPaused){
    if(ev.type==TE_SWIPE_LEFT)  sNextDir=D_LEFT;
    if(ev.type==TE_SWIPE_RIGHT) sNextDir=D_RIGHT;
    if(ev.type==TE_SWIPE_UP)    sNextDir=D_UP;
    if(ev.type==TE_SWIPE_DOWN)  sNextDir=D_DOWN;
  }
}

void snakeOnEncoderDelta(int d){ (void)d; }
void snakeOnEncoderClick(){
  if(sOver){resetSnake();return;}
  sPaused=!sPaused; drawHUD();
  if(sPaused) overlayC("PAUSED","Click/Tap to resume");
  else{
    uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
    drawCourtFrame(); redrawFull(); placeFood(); drawHUD();
  }
}
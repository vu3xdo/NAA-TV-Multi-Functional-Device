#include "game_breakout.h"
#include "ui.h"
#include "joystick.h"
#include "storage.h"
#include <Arduino.h>

// ============================================================
//  BREAKOUT — Precise physics, glitch-free rendering
//
//  ROOT-CAUSE FIXES vs previous version:
//
//  1. Ball position stored as float (fbx, fby) — eliminates
//     integer-truncation drift in sub-stepping.  The old int
//     bx/by accumulated rounding error each sub-step.
//
//  2. hitPaddle condition was wrong:
//     OLD: by <= pY+PADDLE_H+BALL_R  (fires even below paddle)
//     NEW: by+BALL_R >= pY && by-BALL_R <= pY+PADDLE_H
//          AND ball moving downward (bvy>0)
//          AND ball horizontally overlaps paddle
//     Ball is snapped above paddle surface to prevent double-hit.
//     A "hitPaddleGuard" flag prevents a second hit the same frame.
//
//  3. Sub-step loop no longer exits early on brick hit.
//     After reflection, remaining sub-steps continue with the
//     new velocity so the ball keeps moving naturally.
//     startLevel() / life-lost paths set a "done" flag cleanly.
//
//  4. launchBall ensures |bvx| <= 0.65*speed so ball is never
//     horizontal (ensures it always reaches bricks).
//
//  5. eraseBall uses fillRect of exact bounding box — no arc stubs.
//     erasePaddle erases a rect 2px wider than paddle.
//     Both erase before single drawCourt() call per frame.
// ============================================================

static inline int SW()     { return (int)uiTft().width(); }
static inline int SH()     { return (int)uiTft().height(); }
static inline int bodyY0() { return uiBodyY0(); }
static inline int bodyH()  { return uiBodyH(); }
static inline int footerY(){ return SH()-uiFooterH(); }
static inline int clampi(int v,int lo,int hi){ return v<lo?lo:v>hi?hi:v; }

// ---- Bricks ----
static const uint16_t BRICK_COLORS[5]={ 0xF800,0xFD20,0xFFE0,0x07E0,0x07FF };
static const int      BRICK_PTS[5]   ={ 5,4,3,2,1 };
static const int PAD=14, BR_COLS=8, BR_ROWS=5, BRICK_H=11;
static bool bricks[BR_ROWS][BR_COLS];
static int  brickW, brX0, brY0;

// ---- Ball — stored as float for precision ----
static const int BALL_R = 4;
static float fbx, fby;          // authoritative float positions
static float bvx, bvy;          // velocity
static int   ibx, iby;          // integer render positions (from float)
static int   lbx, lby;          // last rendered integer positions
static bool  sBallLive;
static bool  hitPaddleGuard;    // prevents double-bounce same frame

// ---- Paddle ----
static int paddleW;
static const int PADDLE_H = 7;
static int px, lpx, pyVal;
static inline int pyGet() { return pyVal; }

// ---- State ----
static int   score, hiScore, lives, level;
static bool  sOver, sOverShown;
static uint32_t sLast;
static bool  sLevelDone;   // set by hitBricks when all bricks cleared

// ---- Court geometry ----
static int CX0, CY0, CW, CH;

// ============================================================
//  Drawing helpers
// ============================================================
static void drawCourt() {
  uiTft().drawRect(CX0, CY0, CW, CH, uiColorDivider());
}

static void drawHUD() {
  auto& T=uiTft();
  // Score strip: clear exactly 12px (text is 8px at y=bodyY0+5, ends at +13;
  // fill covers +2 to +14).  Court border is at bodyY0+PAD=bodyY0+14 — repaired
  // immediately after so it can never be erased by this fill.
  T.fillRect(PAD, bodyY0()+2, SW()-2*PAD, 12, uiColorBg());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(), uiColorBg());
  T.setCursor(PAD, bodyY0()+5);
  T.print("Score:"); T.print(score);
  T.print(" Hi:"); T.print(hiScore);
  T.setTextColor(0xFFE0, uiColorBg());
  char b[10]; snprintf(b,sizeof(b)," Lv%d",level); T.print(b);
  T.setTextColor(0xF800, uiColorBg());
  for (int i=0;i<lives;i++) T.print(" *");
  // Repair court top-border that fillRect may have cleared.
  drawCourt();
}

static void drawBricksFull() {
  auto& T=uiTft();
  for (int r=0;r<BR_ROWS;r++) {
    for (int c=0;c<BR_COLS;c++) {
      int x=brX0+c*brickW, y=brY0+r*(BRICK_H+3);
      if (bricks[r][c]) {
        T.fillRect(x+1, y, brickW-3, BRICK_H, BRICK_COLORS[r]);
        T.drawFastHLine(x+2, y+2, brickW-6, 0xFFFF);
      } else {
        T.fillRect(x, y, brickW-1, BRICK_H+2, uiColorBg());
      }
    }
  }
  drawCourt();
}

static void eraseBrick(int r, int c) {
  int x=brX0+c*brickW, y=brY0+r*(BRICK_H+3);
  uiTft().fillRect(x, y, brickW-1, BRICK_H+2, uiColorBg());
  // Don't call drawCourt here — done in render pass
}

// eraseBall: fillRect of full bounding square — NO arc stubs
static void eraseBall(int x, int y) {
  int sz = BALL_R+1;
  uiTft().fillRect(x-sz, y-sz, sz*2+1, sz*2+1, uiColorBg());
}
static void drawBall(int x, int y) {
  uiTft().fillCircle(x, y, BALL_R, 0xFFFF);
  uiTft().drawCircle(x, y, BALL_R, uiColorAccent());
}

static void erasePaddle(int x) {
  uiTft().fillRect(x-2, pyGet()-1, paddleW+4, PADDLE_H+2, uiColorBg());
}
static void drawPaddle(int x) {
  uiTft().fillRoundRect(x, pyGet(), paddleW, PADDLE_H, 3, uiColorAccent());
  uiTft().drawFastHLine(x+2, pyGet()+1, paddleW-4, 0xFFFF);
}

static void overlay(const char* msg) {
  auto& T=uiTft();
  int y=bodyY0()+bodyH()/2-14;
  T.fillRoundRect(28,y,SW()-56,34,6,uiColorBg());
  T.drawRoundRect(28,y,SW()-56,34,6,uiColorAccent());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorAccent(),uiColorBg());
  T.setCursor(40,y+10); T.print(msg);
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(40,y+22); T.print("Tap/Enc to restart");
}

static void footer(const char* s) {
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
//  Level management
// ============================================================
static void startLevel() {
  sBallLive=false; sLevelDone=false; hitPaddleGuard=false;
  paddleW=clampi(56-(level-1)*5, 28, 56);

  CX0=PAD; CY0=bodyY0()+PAD; CW=SW()-2*PAD; CH=bodyH()-2*PAD;
  brickW=(CW-4)/BR_COLS;
  brX0=CX0+2;
  brY0=CY0+20;
  pyVal=bodyY0()+bodyH()-24;

  px=CX0+CW/2-paddleW/2; lpx=px;

  // Ball sits on paddle centre
  fbx=(float)(px+paddleW/2);
  fby=(float)(pyGet()-BALL_R-2);
  ibx=(int)fbx; iby=(int)fby;
  lbx=ibx; lby=iby;
  bvx=0; bvy=0;

  for (int r=0;r<BR_ROWS;r++)
    for (int c=0;c<BR_COLS;c++)
      bricks[r][c]=true;

  uiShowScreenTitle("BREAKOUT");
  uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
  drawCourt(); drawBricksFull(); drawHUD();
  drawPaddle(px); drawBall(ibx,iby);
  footer("Move:Touch/Joy  Launch:Tap  Enc:Nudge");
}

static void resetGame() {
  sOver=false; sOverShown=false;
  score=0; lives=3; level=1;
  startLevel();
}

// ============================================================
//  Launch ball — guaranteed non-horizontal angle
// ============================================================
static void launchBall() {
  float spd = 2.5f + (level-1)*0.35f;
  // xFrac in range -0.6..+0.6 (avoids near-horizontal)
  float xFrac = ((float)(int)(esp_random()%121)-60.0f)/100.0f;
  bvx = spd * xFrac;
  bvy = -sqrtf(spd*spd - bvx*bvx);  // always has enough upward component
  sBallLive=true;
  hitPaddleGuard=false;
}

void breakoutEnter() { storageBegin(); hiScore=storageGetI32("brk_hi",0); resetGame(); }
bool breakoutIsGameOver() { return sOver; }

// ============================================================
//  Paddle input
// ============================================================
static void paddleFromInput() {
  if (joyEnabled()) {
    int n=joyNormX();
    if (abs(n)>10) px+=clampi(n/8, -12, 12);   // smoother joystick
  }
  px=clampi(px, CX0+1, CX0+CW-1-paddleW);
}

// ============================================================
//  Physics — all work on float fbx/fby
// ============================================================

// Wall + floor bounce. Returns true if ball was lost (life gone or game over).
static bool bounceWalls() {
  float minX=(float)(CX0+BALL_R+1);
  float maxX=(float)(CX0+CW-BALL_R-1);
  float minY=(float)(CY0+BALL_R+1);
  float maxY=(float)(CY0+CH-BALL_R); // floor = bottom of court

  if (fbx<=minX) { fbx=minX; bvx=fabsf(bvx); }
  if (fbx>=maxX) { fbx=maxX; bvx=-fabsf(bvx); }
  if (fby<=minY) { fby=minY; bvy=fabsf(bvy); }

  if (fby>=maxY) {
    // Ball missed paddle — lost a life
    lives--;
    drawHUD();
    if (lives<=0) { sOver=true; sOverShown=false; return true; }
    sBallLive=false;
    bvx=0; bvy=0;
    fbx=(float)(px+paddleW/2);
    fby=(float)(pyGet()-BALL_R-2);
    ibx=(int)fbx; iby=(int)fby;
    lbx=ibx; lby=iby;
    overlay("LIFE LOST!");
    delay(600);
    uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
    drawCourt(); drawBricksFull(); drawHUD();
    drawPaddle(px); drawBall(ibx,iby);
    hitPaddleGuard=false;
    return true;
  }
  return false;
}

// Paddle collision — uses float positions, snaps ball, reflects angle.
// Guard flag ensures only one bounce per physics update.
static void hitPaddle() {
  if (hitPaddleGuard) return;
  if (bvy <= 0) return;   // ball moving up — can't hit top of paddle

  float pTop=(float)pyGet();
  float pBot=(float)(pyGet()+PADDLE_H);
  float pLeft=(float)px;
  float pRight=(float)(px+paddleW);

  // Ball circle must overlap paddle rect (add BALL_R tolerance)
  bool xOvlap = (fbx+BALL_R >= pLeft-1 && fbx-BALL_R <= pRight+1);
  bool yOvlap = (fby+BALL_R >= pTop    && fby-BALL_R <= pBot);

  if (xOvlap && yOvlap) {
    // Snap ball just above paddle surface
    fby = pTop - (float)BALL_R - 0.5f;
    // Angle: position on paddle maps to -1..+1
    float cx = (float)(px+paddleW/2);
    float off = (fbx-cx) / (paddleW/2.0f);
    off = off<-1.0f?-1.0f:off>1.0f?1.0f:off;
    float spd = sqrtf(bvx*bvx+bvy*bvy);
    spd = spd+0.15f; if(spd>7.0f) spd=7.0f;
    bvx = off * spd * 0.8f;
    bvy = -sqrtf(spd*spd - bvx*bvx);
    hitPaddleGuard=true;
  }
}

// Brick collision — uses float ball position.
// Checks ball AABB against each brick that could be touching.
// Returns true if at least one brick was hit (velocity already reflected).
static bool hitBricks() {
  // Only test bricks in rows the ball can reach
  float ballLeft  = fbx - BALL_R;
  float ballRight = fbx + BALL_R;
  float ballTop   = fby - BALL_R;
  float ballBot   = fby + BALL_R;

  int rowMin = (int)((ballTop - brY0) / (BRICK_H+3));
  int rowMax = (int)((ballBot - brY0) / (BRICK_H+3));
  int colMin = (int)((ballLeft  - brX0) / brickW);
  int colMax = (int)((ballRight - brX0) / brickW);

  rowMin=rowMin<0?0:rowMin; rowMax=rowMax>=BR_ROWS?BR_ROWS-1:rowMax;
  colMin=colMin<0?0:colMin; colMax=colMax>=BR_COLS?BR_COLS-1:colMax;

  bool anyHit=false;

  for (int r=rowMin; r<=rowMax; r++) {
    for (int c=colMin; c<=colMax; c++) {
      if (!bricks[r][c]) continue;

      float bx0=(float)(brX0+c*brickW);
      float by0=(float)(brY0+r*(BRICK_H+3));
      float bx1=bx0+(float)(brickW-1);
      float by1=by0+(float)BRICK_H;

      // Ball AABB vs brick AABB
      if (ballRight<bx0||ballLeft>bx1||ballBot<by0||ballTop>by1) continue;

      // Hit! Determine which axis has smallest penetration
      float ovX=min(ballRight-bx0, bx1-ballLeft);
      float ovY=min(ballBot -by0,  by1-ballTop);

      bricks[r][c]=false;
      score+=BRICK_PTS[r];
      if (score>hiScore){ hiScore=score; storageSetI32("brk_hi",hiScore); }
      eraseBrick(r,c);

      if (ovX<ovY) bvx=-bvx;   // side hit — reflect X
      else         bvy=-bvy;   // top/bottom  — reflect Y

      anyHit=true;
      // Check win
      bool any=false;
      for (int rr=0;rr<BR_ROWS&&!any;rr++)
        for (int cc=0;cc<BR_COLS&&!any;cc++)
          if (bricks[rr][cc]) any=true;
      if (!any) { sLevelDone=true; return true; }
      // Only break one brick per physics step for correctness
      goto doneChecking;
    }
  }
  doneChecking:
  if (anyHit) drawHUD();
  return anyHit;
}

// ============================================================
//  Main loop
// ============================================================
void breakoutLoop(uint32_t now) {
  if (sOver) {
    if (!sOverShown) { overlay("GAME OVER"); sOverShown=true; }
    return;
  }
  if (sLast && (now-sLast)<14) return;
  sLast=now;

  paddleFromInput();

  if (!sBallLive) {
    // Ball tracks paddle centre
    float nbx=(float)(px+paddleW/2);
    float nby=(float)(pyGet()-BALL_R-2);
    bool moved=(nbx!=fbx||nby!=fby);
    bool padMov=(px!=lpx);

    if (padMov) erasePaddle(lpx);
    if (moved)  eraseBall(lbx,lby);
    if (padMov||moved) {
      fbx=nbx; fby=nby;
      ibx=(int)fbx; iby=(int)fby;
      lbx=ibx; lby=iby;
      drawCourt();
    }
    drawPaddle(px); drawBall(ibx,iby);
    lpx=px;
    return;
  }

  // ---- Sub-step physics ----
  // Move by 1-pixel increments to prevent tunnelling.
  // After EACH brick reflection, continue sub-steps (don't exit early).
  // hitPaddleGuard resets at start of each full frame.
  hitPaddleGuard=false;
  sLevelDone=false;

  float mag=sqrtf(bvx*bvx+bvy*bvy);
  int steps=(int)(mag+0.5f)+1; if(steps>10) steps=10;
  float sx=bvx/steps, sy=bvy/steps;

  bool lifeLost=false;
  for (int s=0; s<steps && !lifeLost && !sLevelDone; s++) {
    fbx+=sx; fby+=sy;
    if (bounceWalls()) { lifeLost=true; break; }
    if (!sBallLive) { lifeLost=true; break; }
    hitPaddle();
    hitBricks();   // reflects velocity; sets sLevelDone if all bricks gone
  }

  if (lifeLost) return;   // screen already redrawn inside bounceWalls

  if (sLevelDone) {
    // All bricks cleared
    level++;
    overlay("LEVEL CLEAR!");
    delay(700);
    startLevel();
    return;
  }

  // Update integer render position from float
  ibx=(int)(fbx+0.5f); iby=(int)(fby+0.5f);

  // ---- Render: erase → repair court → draw ----
  eraseBall(lbx, lby);
  if (px!=lpx) erasePaddle(lpx);
  drawCourt();      // single repair per frame
  drawPaddle(px);
  drawBall(ibx, iby);
  lbx=ibx; lby=iby; lpx=px;
}

// ============================================================
//  Input
// ============================================================
void breakoutOnTouch(const TouchEvent& ev) {
  if (sOver) { if (ev.type==TE_TAP) resetGame(); return; }
  if (ev.type==TE_TAP) { if (!sBallLive) launchBall(); return; }
  if (ev.type==TE_DOWN||ev.type==TE_DRAG) {
    if (ev.y>=bodyY0()&&ev.y<footerY())
      px=clampi(ev.x-paddleW/2, CX0+1, CX0+CW-1-paddleW);
  }
}
void breakoutOnEncoderDelta(int d) {
  if (!d) return;
  px=clampi(px+d*6, CX0+1, CX0+CW-1-paddleW);
}
void breakoutOnEncoderClick() {
  if (sOver)      { resetGame(); return; }
  if (!sBallLive) launchBall();
}
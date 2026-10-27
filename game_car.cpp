#include "game_car.h"
#include "ui.h"
#include "joystick.h"
#include "storage.h"
#include <Arduino.h>

// ============================================================
//  CAR DODGE — Glitch-free, smooth gameplay
//
//  ROOT-CAUSE FIXES vs previous version:
//  1. drawCar now clips drawing to [RY0, RY0+RH) so nothing
//     bleeds below road bottom.
//  2. eraseSpriteRect does NOT clip at road bottom, so any
//     pixel drawn near the bottom line gets erased.  The road
//     border is redrawn once per frame to repair it.
//  3. Enemy is deactivated as soon as its TOP edge leaves the
//     road bottom (y >= RY0+RH) — no long wait with invisible
//     ghost pixels lingering.
//  4. Spawn gap reduced from 50-120 to 10-40 px so enemies
//     flow continuously and game never feels "empty".
// ============================================================

static inline int SW()     { return (int)uiTft().width(); }
static inline int SH()     { return (int)uiTft().height(); }
static inline int bodyY0() { return uiBodyY0(); }
static inline int bodyH()  { return uiBodyH(); }
static inline int clampi(int v,int lo,int hi){ return v<lo?lo:v>hi?hi:v; }

// ---- Layout ----
static int RX0, RY0, RW, RH, LW;
static const int PAD = 14;
static int laneX(int l)  { return RX0 + l * LW; }
static int laneCX(int l) { return laneX(l) + LW/2; }

static const uint16_t COL_BORDER = 0x4208;
static const uint16_t COL_DASH   = 0x2104;

// ---- Player ----
static const int PW = 18, PH = 26;
static int  pLane, pX, pY, lpX, lpY;
static int  lives, score, hiScore, speed;
static bool shield;
static int  shieldTicks;
static bool sGameOver;
static uint32_t sLast;
static const int ANIM_STEPS = 6;
static int targetLane, animStep, lastJoy;
static const int JOY_DEAD = 20;
static int dashY;

// ---- Enemies ----
static const int MAX_ENM = 3;
struct Enemy { int x,y,w,h,lane,lx,ly; bool active,passed; };
static Enemy E[MAX_ENM];
static const uint16_t ECOL[3] = { 0xF800, 0xFD20, 0xF81F };

// ---- Coins ----
static const int MAX_COIN = 2;
struct Coin { int x,y,lx,ly; bool active; };
static Coin C[MAX_COIN];

// ============================================================
//  drawCar — clips to road vertical bounds
//  Adafruit GFX clips to screen edges, but NOT to RY0+RH.
//  We must limit drawing ourselves so nothing bleeds out.
// ============================================================
static void drawCar(int x, int y, int w, int h,
                    uint16_t body, uint16_t glass) {
  // Compute visible Y strip within road
  int ytop = (y < RY0)      ? RY0      : y;
  int ybot = (y+h > RY0+RH) ? RY0+RH  : y+h;
  if (ytop >= ybot) return;   // fully outside road — draw nothing

  auto& T = uiTft();

  // Always draw the body strip (clipped to road)
  T.fillRect(x, ytop, w, ybot-ytop, body);

  // Interior details (windshield, rear window, wheels, outline) are ONLY
  // drawn when the car is COMPLETELY inside the road.  This prevents any
  // unclipped fillRect/drawRoundRect call from bleeding into the HUD strip
  // (which sits immediately above RY0) or below the road bottom.
  if (y >= RY0 && y+h <= RY0+RH) {
    T.fillRect(x+2, y+4,       w-4, h/3,  glass);  // windshield
    T.fillRect(x+2, y+h-h/4-3, w-4, h/4, glass);  // rear window
    T.drawRoundRect(x, y, w, h, 3, body);           // body outline
    T.fillRect(x-1,   y+2,    3, 5, 0x18C6);        // wheel TL
    T.fillRect(x+w-2, y+2,    3, 5, 0x18C6);        // wheel TR
    T.fillRect(x-1,   y+h-7,  3, 5, 0x18C6);        // wheel BL
    T.fillRect(x+w-2, y+h-7,  3, 5, 0x18C6);        // wheel BR
  }
}

// ---- eraseSpriteRect — clips at footer top to protect footer ----
static void eraseSpriteRect(int x, int y, int w, int h) {
  int ex=x-2, ey=y-1, ew=w+4, eh=h+2;
  // Clip top: never corrupt HUD strip
  if (ey < RY0) { eh -= RY0-ey; ey=RY0; }
  // Clip bottom: never write into the footer area
  // We still allow past RY0+RH to clean sprite bleed, but stop at footer
  int footerTop = SH()-uiFooterH();
  if (ey+eh > footerTop) eh = footerTop-ey;
  if (ew<=0 || eh<=0) return;
  uiTft().fillRect(ex, ey, ew, eh, uiColorBg());
}

static void drawRoad() {
  auto& T = uiTft();
  T.drawRect(RX0, RY0, RW, RH, COL_BORDER);
  T.drawFastVLine(laneX(1), RY0, RH, COL_BORDER);
  T.drawFastVLine(laneX(2), RY0, RH, COL_BORDER);
}

static void drawDash(int y, uint16_t col) {
  for (int l=1; l<=2; l++) {
    int x=laneX(l)-1, dy=y, dh=10;
    if (dy < RY0)       { dh -= RY0-dy; dy=RY0; }
    if (dy+dh > RY0+RH) { dh = RY0+RH-dy; }
    if (dh>0) uiTft().fillRect(x, dy, 2, dh, col);
  }
}

static void drawPlayerCar() {
  uint16_t body  = shield ? 0x07FF : uiColorAccent();
  uint16_t glass = shield ? 0xFFFF : 0x001F;
  drawCar(pX, pY, PW, PH, body, glass);
  if (shield) uiTft().drawRoundRect(pX-3,pY-3,PW+6,PH+6,5,0x07FF);
}

// erasePlayerCar — always uses the SHIELD-SIZED bounding box.
// The shield drawRoundRect extends 3px beyond the car on every side.
// Using a 5px margin (shield 3px + erase 2px margin) guarantees the
// ring is erased even when shield was active the previous frame.
static void erasePlayerCar(int ex, int ey) {
  // Box covers shield ring: car origin -5px, size +10px each axis
  eraseSpriteRect(ex-5, ey-5, PW+10, PH+10);
}

static void drawCoin(int x, int y) {
  // Draw whenever coin CENTRE is inside the road (RY0+6 guards the top so
  // the circle top pixel stays below the road border at RY0).
  // No hard bottom guard — eraseSpriteRect cleans any overflow and
  // drawRoad() repaints the border each frame.  This prevents the abrupt
  // "coin cuts off 7 pixels early" visual that the tighter guard caused.
  int cy = y+4;
  if (cy < RY0+6 || cy >= RY0+RH) return;
  uiTft().fillCircle(x+4, cy, 5, 0xFFE0);
  uiTft().drawCircle(x+4, cy, 4, 0xFD20);
}

static void drawHUD() {
  auto& T=uiTft();
  // Score strip: clear exactly 12px (text is 8px at y=bodyY0+5, ends at +13;
  // fill covers +2 to +14).  Road border is at bodyY0+PAD=bodyY0+14 — repaired
  // immediately after so it can never be erased by this fill.
  T.fillRect(PAD, bodyY0()+2, SW()-2*PAD, 12, uiColorBg());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(), uiColorBg());
  T.setCursor(PAD, bodyY0()+5);
  T.print("Score:"); T.print(score);
  T.print(" Hi:");  T.print(hiScore);
  T.setTextColor(0xF800, uiColorBg());
  for (int i=0;i<lives;i++) T.print(" *");
  if (shield) { T.setTextColor(0x07FF,uiColorBg()); T.print(" [S]"); }
  // Repair road top-border that the fillRect may have cleared.
  T.drawFastHLine(RX0, RY0, RW, COL_BORDER);
}

static void footer(const char* s) {
  auto& T=uiTft();
  int fy=SH()-uiFooterH();
  T.fillRect(0,fy,SW(),uiFooterH(),uiColorBg());
  T.drawFastHLine(0,fy,SW(),uiColorDivider());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(8,fy+uiFooterH()/2+4);
  T.print(s?s:"");
}

static void overlayMsg(const char* msg, const char* sub) {
  auto& T=uiTft();
  int y=bodyY0()+bodyH()/2-18;
  T.fillRoundRect(20,y,SW()-40,42,6,uiColorBg());
  T.drawRoundRect(20,y,SW()-40,42,6,uiColorAccent());
  T.setFont(nullptr); T.setTextSize(1);
  T.setTextColor(uiColorAccent(),uiColorBg());
  T.setCursor(30,y+12); T.print(msg);
  T.setTextColor(uiColorSub(),uiColorBg());
  T.setCursor(30,y+26); T.print(sub);
}

// ============================================================
//  Spawn
//  gap = 10..39 px — enemies enter road quickly, continuous flow
// ============================================================
static void spawnEnemy(int slot) {
  int lane=(int)(esp_random()%3);
  // Try to avoid very-close enemy in same lane
  for (int t=0; t<6; t++) {
    bool ok=true;
    for (int k=0;k<MAX_ENM;k++) {
      if (k==slot) continue;
      if (E[k].active && E[k].lane==lane && E[k].y<RY0+60) { ok=false; break; }
    }
    if (ok) break;
    lane=(lane+1)%3;
  }
  const int ew=16, eh=22;
  // Small gap so enemies appear promptly
  int gap = 10 + (int)(esp_random()%30);
  int ex  = laneX(lane)+(LW-ew)/2;
  int ey  = RY0-eh-gap;
  E[slot]={ ex,ey,ew,eh,lane,ex,ey,true,false };
}

static void spawnCoin(int slot) {
  int lane=(int)(esp_random()%3);
  int cx=laneCX(lane)-4;
  int cy=RY0-12-(int)(esp_random()%30);
  C[slot]={cx,cy,cx,cy,true};
}

// ============================================================
//  Lane animation
// ============================================================
static void moveLane(int dir) {
  int nt=clampi(pLane+dir,0,2);
  if (nt==targetLane && animStep>0) return;
  targetLane=nt; animStep=ANIM_STEPS;
}
static void animateLane() {
  if (animStep<=0) { pLane=targetLane; pX=laneX(pLane)+(LW-PW)/2; return; }
  int tx=laneX(targetLane)+(LW-PW)/2;
  pX+=(tx-pX+animStep/2)/animStep;
  animStep--;
  if (animStep==0) { pLane=targetLane; pX=tx; }
}

// ============================================================
//  Reset
// ============================================================
static void resetGame() {
  RX0=PAD; RY0=bodyY0()+PAD; RW=SW()-2*PAD; RH=bodyH()-2*PAD; LW=RW/3;

  sGameOver=false; score=0; speed=3; lives=3;
  shield=false; shieldTicks=0;
  pLane=1; animStep=0; targetLane=1; lastJoy=0;
  pX=laneX(1)+(LW-PW)/2;
  pY=RY0+RH-PH-6;
  lpX=pX; lpY=pY;
  dashY=RY0+10;

  for (int i=0;i<MAX_ENM; i++) E[i].active=false;
  for (int i=0;i<MAX_COIN;i++) C[i].active=false;

  // First enemy appears immediately (5px above road)
  spawnEnemy(0);
  E[0].y=RY0-E[0].h-5; E[0].ly=E[0].y;
  spawnCoin(0);

  uiShowScreenTitle("CAR DODGE");
  uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
  drawRoad();
  drawHUD();
  drawDash(dashY, COL_DASH);
  drawPlayerCar();
  for (int i=0;i<MAX_ENM; i++) if(E[i].active) drawCar(E[i].x,E[i].y,E[i].w,E[i].h,ECOL[i],0x4208);
  for (int i=0;i<MAX_COIN;i++) if(C[i].active) drawCoin(C[i].x,C[i].y);
  footer("Left/Right:Steer  Enc:Steer  Long:Menu");
}

void carEnter() { storageBegin(); hiScore=storageGetI32("car_hi",0); resetGame(); }
bool carIsGameOver() { return sGameOver; }

// ============================================================
//  Main loop
// ============================================================
void carLoop(uint32_t now) {
  if (sGameOver) return;
  if (sLast && (now-sLast)<25) return;
  sLast=now;

  // Input
  int ji=0;
  if (joyEnabled()) {
    int nx=joyNormX();
    if (nx<=-JOY_DEAD) ji=-1;
    else if (nx>=JOY_DEAD) ji=+1;
  }
  if (ji!=0 && ji!=lastJoy) moveLane(ji);
  lastJoy=ji;
  animateLane();

  // Shield countdown
  if (shield && --shieldTicks<=0) { shield=false; drawHUD(); }

  // ---- Move enemies ----
  // Deactivate when TOP of enemy reaches road bottom (y >= RY0+RH)
  // This is the key fix: no waiting for y+h+4, which caused ghost strips
  for (int i=0;i<MAX_ENM;i++) {
    if (!E[i].active) continue;
    E[i].ly=E[i].y; E[i].lx=E[i].x;
    E[i].y+=speed;

    // Mark passed once enemy centre has crossed player
    if (E[i].y+E[i].h/2 > pY+PH) E[i].passed=true;

    // Deactivate as soon as TOP edge exits road bottom — no lingering ghosts
    if (E[i].y >= RY0+RH) {
      // Erase the LAST RENDERED position NOW, before clearing active.
      // The render pass skips inactive slots, so without this explicit
      // erase the car sprite would stay painted on screen forever.
      eraseSpriteRect(E[i].lx, E[i].ly, E[i].w, E[i].h);
      E[i].active=false;
      score++;
      if (score>hiScore) { hiScore=score; storageSetI32("car_hi",hiScore); }
      drawHUD();
      if ((score%10)==0 && speed<8) speed++;
      spawnEnemy(i);
      // Possibly activate extra slot
      int cnt=0; for(int k=0;k<MAX_ENM;k++) if(E[k].active) cnt++;
      if (cnt<score/20+1 && cnt<MAX_ENM)
        for (int k=0;k<MAX_ENM;k++) if(!E[k].active){ spawnEnemy(k); break; }
    }
  }

  // ---- Move coins ----
  for (int i=0;i<MAX_COIN;i++) {
    if (!C[i].active) continue;
    C[i].ly=C[i].y; C[i].lx=C[i].x;
    C[i].y+=speed-1;

    // Coin scrolled off road bottom — erase at LAST DRAWN position, then respawn.
    // Clip erase rect at footer top so it never corrupts the footer strip.
    if (C[i].y >= RY0+RH) {
      int era_y = C[i].ly-1, era_h = 12;
      int footerTop = SH()-uiFooterH();
      if (era_y+era_h > footerTop) era_h = footerTop-era_y;
      if (era_h > 0) uiTft().fillRect(C[i].lx-1, era_y, 12, era_h, uiColorBg());
      spawnCoin(i);
      continue;
    }

    // Pickup check
    if (C[i].y+9>=pY && C[i].y<=pY+PH && C[i].x+9>=pX && C[i].x<=pX+PW) {
      // Erase at the LAST DRAWN position (ly = y before this frame's move).
      // Using C[i].y (post-move) was wrong: it misses the top rows of the
      // previously drawn circle, leaving a ghost strip on screen.
      int era_y = C[i].ly-1, era_h = 12;
      int footerTop = SH()-uiFooterH();
      if (era_y+era_h > footerTop) era_h = footerTop-era_y;
      if (era_h > 0) uiTft().fillRect(C[i].x-1, era_y, 12, era_h, uiColorBg());
      C[i].active=false;
      score+=3;
      if ((score/8)%2==1) { shield=true; shieldTicks=120; }
      drawHUD();
      spawnCoin(i);
    }
  }

  // ---- Dash ----
  int prevDash=dashY;
  dashY+=speed;
  if (dashY>RY0+RH-10) dashY=RY0+10;

  // ---- Collision (enemies not passed) ----
  bool hit=false;
  for (int i=0;i<MAX_ENM;i++) {
    if (!E[i].active||E[i].passed) continue;
    const int M=3;
    if (pX+PW-M<E[i].x+M || pX+M>E[i].x+E[i].w-M ||
        pY+PH-M<E[i].y+M || pY+M>E[i].y+E[i].h-M) continue;
    if (shield) {
      shield=false; shieldTicks=0;
      // Erase the enemy at its LAST RENDERED position before respawning.
      // spawnEnemy() resets lx/ly to above-screen, so without this erase
      // the old sprite stays painted on screen permanently.
      eraseSpriteRect(E[i].lx, E[i].ly, E[i].w, E[i].h);
      drawRoad();   // repair any border pixels erased above
      spawnEnemy(i);
      drawHUD();
    } else { hit=true; break; }
  }

  if (hit) {
    lives--;
    drawHUD();
    if (lives<=0) {
      sGameOver=true;
      char buf[28]; snprintf(buf,sizeof(buf),"Score:%d  Hi:%d",score,hiScore);
      overlayMsg("CRASH! GAME OVER",buf);
      return;
    }
    overlayMsg("CRASH! -1 LIFE","Resuming...");
    delay(600);
    // Full-screen repaint after life lost: clear body, redraw everything
    // including coins and dash — previously coins were missing after recovery.
    uiTft().fillRect(0,bodyY0(),SW(),bodyH(),uiColorBg());
    for (int i=0;i<MAX_ENM;i++) if(E[i].active) spawnEnemy(i);
    drawRoad();
    drawHUD();
    drawDash(dashY, COL_DASH);
    for (int i=0;i<MAX_COIN;i++) if(C[i].active) drawCoin(C[i].x,C[i].y);
    drawPlayerCar();
    // Reset last-positions so next-frame erase targets the just-drawn spots.
    lpX=pX; lpY=pY;
    for (int i=0;i<MAX_ENM; i++) { E[i].lx=E[i].x; E[i].ly=E[i].y; }
    for (int i=0;i<MAX_COIN;i++) { C[i].lx=C[i].x; C[i].ly=C[i].y; }
    return;
  }

  // ============================================================
  //  RENDER PASS
  //  Order: erase all old → repair road once → draw all new
  //  eraseSpriteRect clips at footer top so sprites near road
  //  bottom never overwrite the footer strip.
  //  drawRoad() at step 5 restores the full road border.
  //
  //  Anti-flicker: player is only erased/redrawn when its
  //  position or shield state changed, OR when an enemy/coin
  //  erase rect overlaps the player's bounding box.
  // ============================================================

  // Determine if the player rect was (or will be) corrupted this frame.
  static bool s_lShield = false;
  bool playerDirty = (lpX != pX || lpY != pY || s_lShield != shield);

  // 1. Erase old player position (only when dirty).
  if (playerDirty) erasePlayerCar(lpX, lpY);

  // 2. Erase old dash segment
  drawDash(prevDash, uiColorBg());

  // 3. Erase old enemy positions; flag player dirty if erase overlaps it.
  for (int i=0;i<MAX_ENM;i++) {
    if (!E[i].active) continue;
    eraseSpriteRect(E[i].lx, E[i].ly, E[i].w, E[i].h);
    if (!playerDirty) {
      // erase box: E[i].lx-2 .. E[i].lx+E[i].w+2, E[i].ly-1 .. E[i].ly+E[i].h+1
      bool xOk = (E[i].lx-2 < lpX+PW) && (E[i].lx+E[i].w+2 > lpX);
      bool yOk = (E[i].ly-1 < lpY+PH) && (E[i].ly+E[i].h+1 > lpY);
      if (xOk && yOk) playerDirty = true;
    }
  }

  // 4. Erase old coins; flag player dirty if erase overlaps it.
  for (int i=0;i<MAX_COIN;i++) {
    if (!C[i].active) continue;
    eraseSpriteRect(C[i].lx-1, C[i].ly-1, 12, 12);
    if (!playerDirty) {
      // erase box: C[i].lx-3 .. C[i].lx+12, C[i].ly-2 .. C[i].ly+12
      bool xOk = (C[i].lx-3 < lpX+PW) && (C[i].lx+12 > lpX);
      bool yOk = (C[i].ly-2 < lpY+PH) && (C[i].ly+12 > lpY);
      if (xOk && yOk) playerDirty = true;
    }
  }

  // 5. Restore road border once, after all erases
  drawRoad();

  // 6. Draw new dash
  drawDash(dashY, COL_DASH);

  // 7. Draw enemies (behind player)
  for (int i=0;i<MAX_ENM;i++)
    if (E[i].active) drawCar(E[i].x,E[i].y,E[i].w,E[i].h,ECOL[i],0x4208);

  // 8. Draw coins
  for (int i=0;i<MAX_COIN;i++)
    if (C[i].active) drawCoin(C[i].x,C[i].y);

  // 9. Draw player on top (only when dirty)
  if (playerDirty) drawPlayerCar();
  s_lShield = shield;

  // 10. Save positions for next frame
  lpX=pX; lpY=pY;
  for (int i=0;i<MAX_ENM; i++) { E[i].lx=E[i].x; E[i].ly=E[i].y; }
  for (int i=0;i<MAX_COIN;i++) { C[i].lx=C[i].x; C[i].ly=C[i].y; }
}

// ============================================================
//  Input
// ============================================================
void carOnTouch(const TouchEvent& ev) {
  if (sGameOver) { if (ev.type==TE_TAP) resetGame(); return; }
  if (ev.type==TE_TAP||ev.type==TE_DOWN) {
    if (ev.y>=bodyY0()&&ev.y<SH()-uiFooterH()) {
      if (ev.x<SW()/2) moveLane(-1); else moveLane(+1);
    }
  }
  if (ev.type==TE_SWIPE_LEFT)  moveLane(-1);
  if (ev.type==TE_SWIPE_RIGHT) moveLane(+1);
}
void carOnEncoderDelta(int d) { if(d>0) moveLane(+1); else if(d<0) moveLane(-1); }
void carOnEncoderClick()      { if(sGameOver) resetGame(); }
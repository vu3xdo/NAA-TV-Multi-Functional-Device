
#include "game_dino.h"
#include "ui.h"
#include "joystick.h"
#include "storage.h"
#include <Arduino.h>

// Pro Dino Runner: double-jump, duck, speed milestones, cloud parallax,
// ground cactus + flying bird obstacles, hi-score, score multiplier.

static inline int SW()     { return (int)uiTft().width(); }
static inline int SH()     { return (int)uiTft().height(); }
static inline int bodyY0() { return uiBodyY0(); }
static inline int bodyH()  { return uiBodyH(); }

// ---- Ground Y ----
static int groundY() { return bodyY0() + bodyH() - 30; }

static void footer(const char* s) {
  auto& tft = uiTft();
  int fy = SH() - uiFooterH();
  tft.fillRect(0, fy, SW(), uiFooterH(), uiColorBg());
  tft.drawFastHLine(0, fy, SW(), uiColorDivider());
  tft.setFont(nullptr); tft.setTextSize(1);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(8, fy + uiFooterH() / 2 + 4);
  tft.print(s ? s : "");
}

// ---- State ----
static bool  sGameOver = false;
static uint32_t sLast  = 0;
static int   score     = 0;
static int   hiScore   = 0;

// Dino
static const int dW_RUN = 14, dH_RUN = 18;
static const int dW_DUK = 20, dH_DUK = 11;  // duck shape wider, shorter
static int   dX = 38, dY = 0;
static int   dW = dW_RUN, dH = dH_RUN;
static int   vY = 0;
static bool  onGround  = true;
static int   jumpCount = 0;           // 0=ground,1=first jump,2=double jump
static bool  isDucking = false;

// Previous positions (for erase)
static int ldX = 0, ldY = 0, ldW = 0, ldH = 0;

// ---- Obstacle 1 (cactus/bird) ----
static int o1X = 0, o1Y = 0, o1W = 0, o1H = 0;
static int lo1X = 0, lo1Y = 0, lo1W = 0, lo1H = 0;
static bool o1Bird = false;

// ---- Obstacle 2 (second harder one) ----
static int  o2X = 0, o2Y = 0, o2W = 0, o2H = 0;
static int  lo2X = 0, lo2Y = 0, lo2W = 0, lo2H = 0;
static bool o2Active = false;

// ---- Clouds (4 layers for parallax) ----
struct Cloud { int x, y, w, speed; };
static Cloud clouds[4];

// ---- Speed ----
static float oSpeed = 3.0f;

static const int JUMP_VY  = -13;
static const int GRAVITY  =  1;

// ---- Colours ----
static const uint16_t COL_DINO   = 0x07FF;  // cyan
static const uint16_t COL_CACTUS = 0x07E0;  // green
static const uint16_t COL_BIRD   = 0xFD20;  // orange
static const uint16_t COL_CLOUD  = 0x7BEF;  // light grey
static const uint16_t COL_GROUND = 0x4208;  // dark grey

// ---- Draw/erase ----
static void restoreGround() {
  uiTft().drawFastHLine(10, groundY(), SW() - 20, COL_GROUND);
}

static void eraseRect(int x, int y, int w, int h) {
  // Clamp to body area
  int top    = bodyY0();
  int bottom = bodyY0() + bodyH();
  int ey = y - 1, eh = h + 2;
  if (ey < top) { eh -= top - ey; ey = top; }
  if (ey + eh > bottom) eh = bottom - ey;
  if (eh <= 0) return;
  uiTft().fillRect(x - 1, ey, w + 2, eh, uiColorBg());
}

static bool legFlip = false;

static void drawDino() {
  auto& tft = uiTft();
  if (isDucking) {
    tft.fillRect(dX, dY, dW, dH, COL_DINO);
    // eye
    tft.fillRect(dX + dW - 4, dY + 2, 2, 2, uiColorBg());
  } else {
    tft.fillRect(dX, dY, dW, dH, COL_DINO);
    // eye
    tft.fillRect(dX + dW - 3, dY + 3, 2, 2, uiColorBg());
    // legs animation when on ground
    if (onGround) legFlip = !legFlip;
    int legOff = legFlip ? 2 : -2;
    tft.fillRect(dX + 3 + legOff, dY + dH, 3, 4, COL_DINO);
    tft.fillRect(dX + 8 - legOff, dY + dH, 3, 4, COL_DINO);
  }
}

static void drawCactus(int x, int y, int w, int h) {
  auto& tft = uiTft();
  tft.fillRect(x + w / 3, y, w / 3, h, COL_CACTUS);
  tft.fillRect(x, y + h / 3, w, h / 4, COL_CACTUS);
  tft.fillRect(x, y + h / 6, w / 4, h / 3, COL_CACTUS);
  tft.fillRect(x + 3 * w / 4, y + h / 6, w / 4, h / 3, COL_CACTUS);
}

static void drawBird(int x, int y, int w, int h) {
  auto& tft = uiTft();
  // Body — use fillCircle (Adafruit GFX has no fillEllipse)
  int r = min(w, h) / 2;
  tft.fillCircle(x + w / 2, y + h / 2, r, COL_BIRD);
  // Wings: animated up/down
  static bool wingUp = false; wingUp = !wingUp;
  int wingY = wingUp ? y : y + h / 2;
  tft.drawLine(x,         wingY + h / 4, x + w / 2,     y + h / 2, COL_BIRD);
  tft.drawLine(x + w - 1, wingY + h / 4, x + w / 2 - 1, y + h / 2, COL_BIRD);
  // Beak
  tft.drawLine(x + w, y + h / 2 - 1, x + w + 4, y + h / 2, COL_BIRD);
}

static void drawObs1() {
  if (o1X + o1W < 0 || o1X > SW()) return;  // off-screen
  if (o1Bird) drawBird(o1X, o1Y, o1W, o1H);
  else        drawCactus(o1X, o1Y, o1W, o1H);
}
static void drawObs2() {
  if (!o2Active) return;
  if (o2X + o2W < 0 || o2X > SW()) return;
  drawCactus(o2X, o2Y, o2W, o2H);
}

static void drawCloud(int i) {
  uiTft().fillRoundRect(clouds[i].x, clouds[i].y, clouds[i].w, 8, 3, COL_CLOUD);
}
static void eraseCloud(int i) {
  uiTft().fillRect(clouds[i].x - clouds[i].speed - 1, clouds[i].y - 1, clouds[i].w + clouds[i].speed + 2, 10, uiColorBg());
}

static void drawScore() {
  auto& tft = uiTft();
  // Snake-style: 16-px cleared strip with PAD side margins.
  const int PAD = 6;
  tft.fillRect(PAD, bodyY0()+2, SW()-2*PAD, 16, uiColorBg());
  tft.setFont(nullptr); tft.setTextSize(1);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(PAD, bodyY0()+5);
  tft.print("Score:"); tft.print(score);
  tft.print(" Hi:"); tft.print(hiScore);
  int spLv = (int)(oSpeed - 3) / 1 + 1;
  tft.setTextColor(0xFFE0, uiColorBg());
  char buf[8]; snprintf(buf, sizeof(buf), " x%d", max(1, spLv));
  tft.print(buf);
}

static void overlay(const char* msg) {
  auto& tft = uiTft();
  int y = bodyY0() + bodyH() / 2 - 14;
  tft.fillRoundRect(28, y, SW() - 56, 38, 6, uiColorBg());
  tft.drawRoundRect(28, y, SW() - 56, 38, 6, uiColorAccent());
  tft.setFont(nullptr); tft.setTextSize(1);
  tft.setTextColor(0xF800, uiColorBg());
  tft.setCursor(40, y + 10); tft.print(msg);
  tft.setTextColor(uiColorSub(), uiColorBg());
  tft.setCursor(40, y + 24);
  char buf[28]; snprintf(buf, sizeof(buf), "Hi:%d  Tap to restart", hiScore);
  tft.print(buf);
}

// ---- Spawn helpers ----
static void spawnObs1Far() {
  o1X = SW() + 60 + (int)(esp_random() % 80);
  o1Bird = (score >= 10) && ((esp_random() % 3) == 0);
  if (o1Bird) {
    o1W = 18; o1H = 12;
    // Bird flies at a random height: just above dino head or mid-air
    int minBirdY = groundY() - dH_RUN - o1H - 20;
    int maxBirdY = groundY() - dH_RUN - o1H - 4;
    o1Y = minBirdY + (int)(esp_random() % max(1, maxBirdY - minBirdY));
  } else {
    o1W = 10 + (int)(esp_random() % 8);
    o1H = 14 + (int)(esp_random() % 10);
    o1Y = groundY() - o1H;
  }
  lo1X = o1X; lo1Y = o1Y; lo1W = o1W; lo1H = o1H;
}

static void spawnObs2() {
  o2X = o1X + 70 + (int)(esp_random() % 60);
  o2W = 8 + (int)(esp_random() % 6);
  o2H = 12 + (int)(esp_random() % 8);
  o2Y = groundY() - o2H;
  lo2X = o2X; lo2Y = o2Y; lo2W = o2W; lo2H = o2H;
}

static void initClouds() {
  for (int i = 0; i < 4; i++) {
    clouds[i].x     = (int)(esp_random() % SW());
    clouds[i].y     = bodyY0() + 12 + (int)(esp_random() % 28);
    clouds[i].w     = 24 + (int)(esp_random() % 30);
    clouds[i].speed = 1 + i % 2;
  }
}

static void jump() {
  if (jumpCount >= 2) return;
  jumpCount++;
  vY = (jumpCount == 2) ? JUMP_VY + 3 : JUMP_VY;
  isDucking = false;
  dW = dW_RUN; dH = dH_RUN;
}

static void duck(bool doIt) {
  if (doIt && !onGround) return;  // can't duck mid-air
  isDucking = doIt;
  if (isDucking) { dW = dW_DUK; dH = dH_DUK; dY = groundY() - dH; }
  else           { dW = dW_RUN; dH = dH_RUN; }
}

void dinoEnter() {
  storageBegin();
  hiScore = storageGetI32("dino_hi", 0);

  sGameOver = false;
  score     = 0;
  oSpeed    = 3.0f;
  legFlip   = false;
  duck(false);
  dX = 38;
  dY = groundY() - dH;
  vY = 0;
  onGround  = true;
  jumpCount = 0;
  o2Active  = false;

  spawnObs1Far();
  initClouds();

  ldX = dX; ldY = dY; ldW = dW; ldH = dH;

  uiShowScreenTitle("DINO RUNNER");
  uiTft().fillRect(0, bodyY0(), SW(), bodyH(), uiColorBg());
  restoreGround();
  for (int i = 0; i < 4; i++) drawCloud(i);
  drawScore();
  drawDino();
  drawObs1();
  drawObs2();
  footer("Tap/Up:Jump  Down:Duck  Long:Menu");
}

bool dinoIsGameOver() { return sGameOver; }

void dinoLoop(uint32_t now) {
  if (sGameOver) return;

  if (sLast && (now - sLast) < 20) return;
  sLast = now;

  // Speed ramp every 8 obstacles cleared
  if ((score % 8) == 0 && score > 0) oSpeed = min(8.0f, 3.0f + score / 8 * 0.5f);

  // Joystick input
  if (joyEnabled()) {
    JoyDir dir = joyReadDir();
    if (dir == JOY_UP)             jump();
    if (dir == JOY_DOWN && onGround) duck(true);
    else if (isDucking && dir != JOY_DOWN) duck(false);
  }

  // Physics
  if (!isDucking) {
    vY += GRAVITY;
    dY += vY;
    int gFloor = groundY() - dH;
    if (dY >= gFloor) {
      dY = gFloor;
      vY = 0;
      onGround  = true;
      jumpCount = 0;
    } else {
      onGround = false;
    }
  } else {
    dY = groundY() - dH;
    vY = 0;
    onGround  = true;
    jumpCount = 0;
  }

  // Move obstacles
  int ispeed = (int)oSpeed;
  o1X -= ispeed;
  if (o2Active) o2X -= (int)(oSpeed * 0.85f + 0.5f);

  // Move clouds (parallax)
  for (int i = 0; i < 4; i++) {
    clouds[i].x -= clouds[i].speed;
    if (clouds[i].x + clouds[i].w < 0) {
      clouds[i].x = SW() + 10;
      clouds[i].y = bodyY0() + 12 + (int)(esp_random() % 28);
      clouds[i].w = 24 + (int)(esp_random() % 30);
    }
  }

  // Respawn obs1?
  bool needRespawn1 = (o1X < -o1W - 4);

  // Collision check (with shrunk hitbox for fairness)
  auto collide = [&](int ox, int oy, int ow, int oh) -> bool {
    const int dMargin = 3;
    return !(dX + dW - dMargin < ox + dMargin ||
             dX + dMargin > ox + ow - dMargin ||
             dY + dH - dMargin < oy + dMargin ||
             dY + dMargin > oy + oh - dMargin);
  };
  bool hit = collide(o1X, o1Y, o1W, o1H) ||
             (o2Active && collide(o2X, o2Y, o2W, o2H));

  if (hit) {
    sGameOver = true;
    if (score > hiScore) { hiScore = score; storageSetI32("dino_hi", hiScore); }
    // +8 width covers the 4-px bird beak that extends beyond o1W
    eraseRect(ldX, ldY, ldW + 3, ldH + 4);
    eraseRect(lo1X, lo1Y, lo1W + 8, lo1H + 4);
    drawDino();
    restoreGround();
    overlay("GAME OVER");
    return;
  }

  // Erases (using last positions)
  eraseRect(ldX, ldY, ldW + 3, ldH + 4);  // +3/+4 covers legs
  // obs1: +8 width covers the 4-px bird beak (drawLine to x+w+4) that
  // extends beyond the declared o1W bounding box; +4 height for wing.
  eraseRect(lo1X, lo1Y, lo1W + 8, lo1H + 4);
  if (o2Active) eraseRect(lo2X, lo2Y, lo2W + 2, lo2H + 2);
  for (int i = 0; i < 4; i++) eraseCloud(i);

  // Ground line (restore after erases)
  restoreGround();

  // Respawn after erase
  if (needRespawn1) {
    spawnObs1Far();
    score++;
    if (score > hiScore) { hiScore = score; storageSetI32("dino_hi", hiScore); }
    drawScore();
    if (score == 15) { o2Active = true; spawnObs2(); }
    // Extra second obstacle at score 30
    if (score == 30 && o2Active) { spawnObs2(); }
  }

  // Draw everything
  for (int i = 0; i < 4; i++) drawCloud(i);
  drawDino();
  drawObs1();
  drawObs2();

  // Save last positions
  ldX = dX; ldY = dY; ldW = dW; ldH = dH;
  lo1X = o1X; lo1Y = o1Y; lo1W = o1W; lo1H = o1H;
  if (o2Active) { lo2X = o2X; lo2Y = o2Y; lo2W = o2W; lo2H = o2H; }
}

void dinoOnTouch(const TouchEvent& ev) {
  if (ev.type == TE_TAP) {
    if (sGameOver) { dinoEnter(); return; }
    jump();
  }
  if (ev.type == TE_DOWN && !sGameOver) {
    if (ev.y > bodyY0() + bodyH() / 2) duck(true);
    else jump();
  }
  if (ev.type == TE_UP && !sGameOver) { duck(false); }
}

void dinoOnEncoderDelta(int d) { (void)d; }
void dinoOnEncoderClick() {
  if (sGameOver) { dinoEnter(); return; }
  jump();
}

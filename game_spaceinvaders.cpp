#include "game_spaceinvaders.h"
#include "ui.h"
#include "joystick.h"
#include "games.h"
#include <Arduino.h>

// ─── Arcade Space Invaders ────────────────────────────────────────────────────
// Enemies fall from top, player shoots them from the bottom.
// Key design rules for clean rendering:
//   * Always erase at OLD position, then update coords, then draw at NEW.
//   * All sprite erase rects are WIDER than the sprite width (+4px) to
//     account for zigzag horizontal drift within one tick.
//   * Play area is strictly between gameTop() and gameBot() — nothing
//     draws outside those bounds.

#define SHIP_W    22
#define SHIP_H    10
#define BULLET_W   2
#define BULLET_H   7
#define INV_W     14
#define INV_H     10
#define INV_ERASE_W  (INV_W + 6)   // wider erase to cover zigzag drift
#define INV_ERASE_H  (INV_H + 4)
#define MAX_ENEMIES   10
#define MAX_PBULLETS   8
#define MAX_EBULLETS   3

// HUD occupies 14px at top of body; game starts below it
static inline int hudH()    { return 14; }
static inline int gameTop() { return uiBodyY0() + hudH(); }
static inline int gameBot() { return uiBodyY0() + uiBodyH() - 2; }
static inline int SW()      { return uiTft().width(); }

// ─── Sprite structs ───────────────────────────────────────────────────────────
struct PBullet { int16_t x, y, py; bool active; };   // py = previous y
struct EBullet { int16_t x, y, py; bool active; };
struct Enemy {
    int16_t x,  y;     // current
    int16_t px, py;    // previous (for clean erase)
    int16_t speedY;
    int16_t zigDir;
    uint32_t nextShot;
    bool  active;
    uint8_t type;       // 0=normal 1=fast 2=zigzag 3=boss
};

static PBullet pbullets[MAX_PBULLETS];
static EBullet ebullets[MAX_EBULLETS];
static Enemy   enemies[MAX_ENEMIES];

static int16_t shipX = 0, shipY = 0;
static int     score = 0, lives = 3, wave = 1;
static int     killCount = 0, waveKillTarget = 10;
static bool    gameOver = false;
static int     maxAlive = 4;
static uint32_t spawnInterval  = 1800;
static uint32_t lastSpawn      = 0;
static uint32_t lastJoyMove    = 0;
static uint32_t lastJoyShoot   = 0;
static uint32_t lastEnemyTick  = 0;   // rate-limit enemy movement
static uint32_t lastEBulletTick = 0;  // rate-limit enemy bullet movement

// Movement periods (ms). Adjust these to tune game feel.
#define ENEMY_TICK_MS    40u   // enemies move every 40ms (~25fps)
#define EBULLET_TICK_MS  18u   // enemy bullets every 18ms (~55fps)
#define PBULLET_SPEED     4    // player bullet pixels per frame (every loop tick)

// ─── Colour by type ───────────────────────────────────────────────────────────
static uint16_t eColor(uint8_t t) {
    switch (t) {
        case 1: return ILI9341_ORANGE;
        case 2: return ILI9341_CYAN;
        case 3: return ILI9341_MAGENTA;
        default:return ILI9341_RED;
    }
}

// ─── HUD ──────────────────────────────────────────────────────────────────────
static void drawHUD() {
    auto& tft = uiTft();
    // Fill the HUD strip (does NOT touch the header above uiBodyY0())
    tft.fillRect(0, uiBodyY0(), SW(), hudH(), ILI9341_BLACK);
    // Divider line between HUD and play field
    tft.drawFastHLine(0, uiBodyY0() + hudH() - 1, SW(), uiColorDivider());

    tft.setFont(nullptr);
    tft.setTextSize(1);

    // Score (left)
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(3, uiBodyY0() + 4);
    tft.print("S:"); tft.setTextColor(ILI9341_WHITE); tft.print(score);

    // Wave (centre)
    tft.setTextColor(0xFD20);
    tft.setCursor(SW() / 2 - 20, uiBodyY0() + 4);
    tft.print("W:"); tft.print(wave);

    // Lives + kill progress (right)
    char buf[16];
    // lives shown as "^" symbols
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(SW() - 68, uiBodyY0() + 4);
    for (int i = 0; i < lives && i < 3; i++) tft.print("^");

    // kill count
    snprintf(buf, sizeof(buf), " %d/%d", killCount, waveKillTarget);
    tft.setTextColor(0x7BEF);
    tft.print(buf);
}

static void drawFooter() {
    auto& tft = uiTft();
    int fy = uiTft().height() - uiFooterH();
    tft.fillRect(0, fy, SW(), uiFooterH(), uiColorBg());
    tft.drawFastHLine(0, fy, SW(), uiColorDivider());
    tft.setFont(nullptr); tft.setTextSize(1);
    tft.setTextColor(uiColorSub());
    tft.setCursor(6, fy + uiFooterH() / 2 + 3);
    tft.print("Touch:Move  Enc/JoyUp:Fire  LongPress:Menu");
}

// ─── Ship ─────────────────────────────────────────────────────────────────────
static void eraseShip() {
    uiTft().fillRect(shipX - 1, shipY, SHIP_W + 3, SHIP_H + 3, ILI9341_BLACK);
}
static void drawShip() {
    auto& tft = uiTft();
    tft.fillRect(shipX,      shipY + 6, SHIP_W, 4,  ILI9341_GREEN);   // base
    tft.fillRect(shipX + 4, shipY + 3, 14,     3,   ILI9341_GREEN);   // body
    tft.fillRect(shipX + 10,shipY,     2,      3,   ILI9341_GREEN);   // barrel
    tft.fillRect(shipX + 1, shipY + 10,4,      2,   0x07E0);          // left thruster
    tft.fillRect(shipX + 17,shipY + 10,4,      2,   0x07E0);          // right thruster
}

// ─── Player bullets ───────────────────────────────────────────────────────────
static void fireBullet() {
    for (int i = 0; i < MAX_PBULLETS; i++) {
        if (!pbullets[i].active) {
            pbullets[i].active = true;
            pbullets[i].x  = shipX + SHIP_W / 2 - BULLET_W / 2;
            pbullets[i].y  = shipY - BULLET_H; // start above ship barrel
            pbullets[i].py = pbullets[i].y;    // py = first drawn pos
            // Draw immediately so bullet appears without waiting for next loop tick
            uiTft().fillRect(pbullets[i].x, pbullets[i].y, BULLET_W, BULLET_H, ILI9341_YELLOW);
            return;
        }
    }
}
static void erasePBullet(int i) {
    // Erase 1px taller to cover any partial overlap at boundary
    uiTft().fillRect(pbullets[i].x, pbullets[i].py, BULLET_W, BULLET_H + 1, ILI9341_BLACK);
}
static void drawPBullet(int i) {
    uiTft().fillRect(pbullets[i].x, pbullets[i].y, BULLET_W, BULLET_H, ILI9341_YELLOW);
}

// ─── Enemy bullets ────────────────────────────────────────────────────────────
static void eraseEBullet(int i) {
    uiTft().fillRect(ebullets[i].x, ebullets[i].py, BULLET_W, BULLET_H + 1, ILI9341_BLACK);
}
static void drawEBullet(int i) {
    uiTft().fillRect(ebullets[i].x, ebullets[i].y, BULLET_W, BULLET_H, 0xF800);
}

// ─── Enemy ────────────────────────────────────────────────────────────────────
// Always erase using PREVIOUS px,py. Draw using current x,y.
static void eraseEnemy(int i) {
    int ew = (enemies[i].type == 3) ? INV_W + 6 + 6 : INV_ERASE_W;
    // Erase a band covering old pos + drift margin. Also clear antennae (2px above)
    int top = enemies[i].py - 3;
    if (top < gameTop()) top = gameTop();
    uiTft().fillRect(enemies[i].px - 3, top, ew + 6, INV_ERASE_H + 5, ILI9341_BLACK);
}
static void drawEnemy(int i) {
    if (!enemies[i].active) return;
    auto& tft = uiTft();
    int x = enemies[i].x, y = enemies[i].y;
    // Clip: don't draw above HUD divider
    if (y < gameTop()) return;
    uint16_t col = eColor(enemies[i].type);
    if (enemies[i].type == 3) {
        // Boss: wide (20px)
        tft.fillRect(x,     y + 2, 20, 6,  col);
        tft.fillRect(x + 4, y,     12, 2,  col);
        tft.fillRect(x + 2, y + 8, 16, 2,  col);
        tft.fillRect(x + 4, y + 3,  3, 3,  ILI9341_BLACK);
        tft.fillRect(x + 13,y + 3,  3, 3,  ILI9341_BLACK);
    } else {
        // Normal/Fast/Zigzag: 14px wide, pixel-art style
        tft.fillRect(x + 3, y - 2,  2, 2,  col); // left antenna
        tft.fillRect(x + 9, y - 2,  2, 2,  col); // right antenna
        tft.fillRect(x + 2, y,     10, 1,  col);
        tft.fillRect(x + 1, y + 1, 12, 1,  col);
        tft.fillRect(x,     y + 2, 14, 2,  col);
        // Eyes
        tft.fillRect(x + 3, y + 2,  2, 2,  ILI9341_BLACK);
        tft.fillRect(x + 9, y + 2,  2, 2,  ILI9341_BLACK);
        tft.fillRect(x + 2, y + 4, 10, 2,  col);
        tft.fillRect(x,     y + 6,  4, 2,  col);
        tft.fillRect(x + 10,y + 6,  4, 2,  col);
    }
}

// ─── Spawning ─────────────────────────────────────────────────────────────────
static int activeEnemyCount() {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].active) n++;
    return n;
}

static void spawnEnemy() {
    if (activeEnemyCount() >= maxAlive) return;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) continue;
        // Random X but keep inside play area
        enemies[i].x  = 6 + (int)(esp_random() % (SW() - INV_W - 12));
        enemies[i].px = enemies[i].x;
        // Start just below the HUD divider so it's always visible from top
        enemies[i].y  = gameTop() + 2;
        enemies[i].py = enemies[i].y;

        int roll = (int)(esp_random() % 100);
        int bChance = min(5 * (wave - 1), 15);
        int fChance = min(8 * (wave - 1), 35);
        if      (roll < bChance)          enemies[i].type = 3;
        else if (roll < bChance + fChance)enemies[i].type = 1;
        else if (roll < bChance+fChance+30)enemies[i].type = 2;
        else                               enemies[i].type = 0;

        int base = 1 + wave / 2;
        if (enemies[i].type == 1) base += 2;
        if (enemies[i].type == 3) base = max(1, base - 1);
        enemies[i].speedY = (int16_t)min(base, 5);
        enemies[i].zigDir = (esp_random() & 1) ? 1 : -1;
        enemies[i].nextShot = millis() + 1500 + (esp_random() % 3000);
        enemies[i].active = true;
        drawEnemy(i);
        return;
    }
}

// ─── Wave / lives ─────────────────────────────────────────────────────────────
static void showGameOver() {
    auto& tft = uiTft();
    int bw = 160, bh = 56;
    int bx = (SW() - bw) / 2;
    int by = gameTop() + (gameBot() - gameTop()) / 2 - bh / 2;
    tft.fillRoundRect(bx, by, bw, bh, 6, ILI9341_BLACK);
    tft.drawRoundRect(bx, by, bw, bh, 6, ILI9341_RED);
    tft.setFont(nullptr); tft.setTextSize(2);
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(bx + 14, by + 5);  tft.print("GAME OVER");
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(bx + 14, by + 34); tft.print("Score: "); tft.print(score);
    tft.setTextColor(uiColorSub());
    tft.setCursor(bx + 14, by + 44); tft.print("Tap to retry");
}

static void clearField() {
    uiTft().fillRect(0, gameTop(), SW(), gameBot() - gameTop() + 2, ILI9341_BLACK);
}

static void loseLife() {
    lives--;
    drawHUD();
    if (lives <= 0) {
        gameOver = true;
        showGameOver();
    } else {
        clearField();
        for (int i = 0; i < MAX_ENEMIES;  i++) enemies[i].active  = false;
        for (int i = 0; i < MAX_PBULLETS; i++) pbullets[i].active = false;
        for (int i = 0; i < MAX_EBULLETS; i++) ebullets[i].active = false;
        drawShip();
        lastSpawn = millis();
    }
}

static void advanceWave() {
    wave++;
    killCount = 0;
    waveKillTarget  = 10 + (wave - 1) * 3;
    maxAlive        = min(4 + wave, MAX_ENEMIES);
    spawnInterval   = (uint32_t)max(400, 1800 - (wave - 1) * 150);

    for (int i = 0; i < MAX_ENEMIES;  i++) enemies[i].active  = false;
    for (int i = 0; i < MAX_PBULLETS; i++) pbullets[i].active = false;
    for (int i = 0; i < MAX_EBULLETS; i++) ebullets[i].active = false;

    clearField();
    drawHUD();

    // Brief wave banner
    auto& tft = uiTft();
    int bw = 160, bh = 40;
    int bx = (SW() - bw) / 2;
    int by = gameTop() + (gameBot() - gameTop()) / 2 - bh / 2;
    tft.fillRoundRect(bx, by, bw, bh, 5, 0x0010);
    tft.drawRoundRect(bx, by, bw, bh, 5, ILI9341_CYAN);
    tft.setFont(nullptr); tft.setTextSize(2);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(bx + 12, by + 6); tft.print("WAVE "); tft.print(wave);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(bx + 12, by + 28); tft.print("Kill:"); tft.print(waveKillTarget);
    delay(1000);

    clearField();
    drawShip();
    lastSpawn = millis();
}

// ─── Public API ───────────────────────────────────────────────────────────────
void spaceinvadersEnter() {
    score = 0; lives = 3; wave = 1;
    killCount = 0; waveKillTarget = 10;
    maxAlive = 4; spawnInterval = 1800;
    gameOver = false;
    shipX = SW() / 2 - SHIP_W / 2;
    shipY = gameBot() - SHIP_H - 2;
    lastSpawn = lastJoyMove = lastJoyShoot = 0;
    lastEnemyTick = lastEBulletTick = 0;

    for (int i = 0; i < MAX_PBULLETS; i++) pbullets[i].active = false;
    for (int i = 0; i < MAX_EBULLETS; i++) ebullets[i].active = false;
    for (int i = 0; i < MAX_ENEMIES;  i++) enemies[i].active  = false;

    uiShowScreenTitle("INVADERS");
    uiTft().fillRect(0, uiBodyY0(), SW(), uiBodyH(), ILI9341_BLACK);
    drawHUD();
    drawFooter();
    drawShip();
    lastSpawn = millis();
}

void spaceinvadersLoop(uint32_t now) {
    if (gameOver) return;

    // ── Joystick ─────────────────────────────────────────────────
    if (joyEnabled()) {
        int nx = joyNormX(), ny = joyNormY();
        if (now - lastJoyMove > 16) {
            if (nx < -20) {
                eraseShip(); shipX -= 3;
                if (shipX < 0) shipX = 0;
                drawShip();
            } else if (nx > 20) {
                eraseShip(); shipX += 3;
                if (shipX > SW() - SHIP_W - 2) shipX = SW() - SHIP_W - 2;
                drawShip();
            }
            lastJoyMove = now;
        }
    }
    
    // Auto-fire continuously
    if (now - lastJoyShoot > 250) {
        lastJoyShoot = now; 
        fireBullet();
    }

    // ── Player bullets (no timer — every frame, fast & responsive) ──
    for (int i = 0; i < MAX_PBULLETS; i++) {
        if (!pbullets[i].active) continue;
        erasePBullet(i);          // erase at py (last DRAWN position)
        pbullets[i].y -= PBULLET_SPEED;
        // Deactivate if above play area — py already erased above
        if (pbullets[i].y < gameTop()) {
            pbullets[i].active = false;
            continue;
        }
        bool hit = false;
        for (int k = 0; k < MAX_ENEMIES && !hit; k++) {
            if (!enemies[k].active) continue;
            int ew = (enemies[k].type == 3) ? 20 : INV_W;
            if (pbullets[i].x + BULLET_W >= enemies[k].x &&
                pbullets[i].x            <= enemies[k].x + ew &&
                pbullets[i].y + BULLET_H >= enemies[k].y &&
                pbullets[i].y            <= enemies[k].y + INV_H) {
                enemies[k].px = enemies[k].x;
                enemies[k].py = enemies[k].y;
                eraseEnemy(k);
                enemies[k].active = false;
                pbullets[i].active = false;
                int pts = (enemies[k].type == 3) ? 50 :
                          (enemies[k].type == 1) ? 20 :
                          (enemies[k].type == 2) ? 15 : 10;
                score += pts * wave;
                killCount++;
                drawHUD();
                hit = true;
                if (killCount >= waveKillTarget) { advanceWave(); return; }
            }
        }
        if (!hit) {
            drawPBullet(i);
            pbullets[i].py = pbullets[i].y; // update py AFTER drawing
        }
    }

    // ── Enemy bullets (rate-limited to EBULLET_TICK_MS) ───────────
    if (now - lastEBulletTick >= EBULLET_TICK_MS) {
        lastEBulletTick = now;
        for (int i = 0; i < MAX_EBULLETS; i++) {
            if (!ebullets[i].active) continue;
            eraseEBullet(i);            // erase at py (last drawn pos)
            ebullets[i].y += 3;
            if (ebullets[i].y > gameBot()) { ebullets[i].active = false; continue; }
            // Hit ship?
            if (ebullets[i].x + BULLET_W >= shipX &&
                ebullets[i].x            <= shipX + SHIP_W &&
                ebullets[i].y + BULLET_H >= shipY &&
                ebullets[i].y            <= shipY + SHIP_H) {
                ebullets[i].active = false;
                eraseShip();
                loseLife();
                return;
            }
            drawEBullet(i);
            ebullets[i].py = ebullets[i].y; // update py AFTER drawing
        }
    }

    // ── Move enemies (rate-limited to ENEMY_TICK_MS) ───────────────
    if (now - lastEnemyTick >= ENEMY_TICK_MS) {
        lastEnemyTick = now;
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (!enemies[i].active) continue;

            // Save previous position, erase there
            enemies[i].px = enemies[i].x;
            enemies[i].py = enemies[i].y;
            eraseEnemy(i);

            // Move downward
            enemies[i].y += enemies[i].speedY;
            if (enemies[i].type == 2) { // zigzag horizontal
                enemies[i].x += enemies[i].zigDir * 2;
                if (enemies[i].x <= 2 || enemies[i].x >= SW() - INV_W - 2)
                    enemies[i].zigDir = -enemies[i].zigDir;
            }

            // Reached ship row?
            if (enemies[i].y + INV_H >= shipY) {
                enemies[i].active = false;
                eraseShip();
                loseLife();
                return;
            }
            // Left play area?
            if (enemies[i].y > gameBot()) { enemies[i].active = false; continue; }

            drawEnemy(i);

            // Enemy shooting (wave 3+)
            if (wave >= 3 && now >= enemies[i].nextShot) {
                for (int j = 0; j < MAX_EBULLETS; j++) {
                    if (!ebullets[j].active) {
                        int ew = (enemies[i].type == 3) ? 20 : INV_W;
                        ebullets[j].active = true;
                        ebullets[j].x  = enemies[i].x + ew / 2;
                        ebullets[j].y  = enemies[i].y + INV_H;
                        ebullets[j].py = ebullets[j].y;
                        drawEBullet(j);
                        enemies[i].nextShot = now + 1500 + (esp_random() % 2500);
                        break;
                    }
                }
            }
        }
    }

    // ── Spawn new enemies ──────────────────────────────────────────
    if (now - lastSpawn > spawnInterval) {
        lastSpawn = now;
        spawnEnemy();
    }
}

void spaceinvadersOnTouch(const TouchEvent& ev) {
    if (gameOver) { if (ev.type == TE_TAP) spaceinvadersEnter(); return; }
    if (ev.type == TE_DOWN || ev.type == TE_DRAG) {
        if (ev.y >= uiBodyY0() && ev.y < uiBodyY0() + uiBodyH()) {
            eraseShip();
            shipX = ev.x - SHIP_W / 2;
            if (shipX < 0) shipX = 0;
            if (shipX > SW() - SHIP_W - 2) shipX = SW() - SHIP_W - 2;
            drawShip();
        }
    }
    if (ev.type == TE_TAP) fireBullet();
}

void spaceinvadersOnEncoderDelta(int d) {
    if (!d || gameOver) return;
    eraseShip();
    shipX += d * 6;
    if (shipX < 0) shipX = 0;
    if (shipX > SW() - SHIP_W - 2) shipX = SW() - SHIP_W - 2;
    drawShip();
}

void spaceinvadersOnEncoderClick() {
    if (gameOver) { spaceinvadersEnter(); return; }
    fireBullet();
}

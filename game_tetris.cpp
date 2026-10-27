#include "game_tetris.h"
#include "ui.h"
#include "joystick.h"
#include "games.h"
#include <Arduino.h>

// ─── Board config ────────────────────────────────────────────────────────────
static const int T_ROWS = 20;
static const int T_COLS = 10;
static int T_CELL = 9; // px per cell — computed dynamically in tetrisEnter()

// ─── Piece definitions ───────────────────────────────────────────────────────
// [piece][rotation][block] = {col_offset, row_offset}
static const int8_t PIECES[7][4][4][2] = {
    // I
    { {{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}} },
    // J
    { {{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{1,2},{0,2}} },
    // L
    { {{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}} },
    // O
    { {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}} },
    // S
    { {{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}} },
    // T
    { {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}} },
    // Z
    { {{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}} }
};

// Piece colours (index 0 = empty/black)
static const uint16_t PIECE_COLORS[8] = {
    ILI9341_BLACK,
    ILI9341_CYAN,      // I
    ILI9341_BLUE,      // J
    ILI9341_ORANGE,    // L
    ILI9341_YELLOW,    // O
    ILI9341_GREEN,     // S
    ILI9341_PURPLE,    // T
    ILI9341_RED        // Z
};

// ─── Game state ───────────────────────────────────────────────────────────────
static uint8_t  t_board[T_ROWS][T_COLS];
static bool     t_over      = false;
static uint32_t t_lastFall  = 0;
static uint32_t t_fallSpeed = 500;
static int      t_score     = 0;
static int      t_lines     = 0;
static int      t_level     = 1;

static int  t_px, t_py;   // current piece position (col, row)
static int  t_piece, t_rot;
static int  t_next;        // next piece index
static int  t_ghostY    = -1;
static int  t_hold      = -1;
static bool t_canHold   = true;

// Joystick repeat timers
static uint32_t t_joyHTime = 0;
static uint32_t t_joyVTime = 0;

// ─── Layout helpers ───────────────────────────────────────────────────────────
static int s_boardTop = 0; // computed in tetrisEnter() to align board bottom with body bottom

static inline int boardLeft()  { return (uiTft().width() - T_COLS * T_CELL) / 2; }
static inline int boardTop()   { return s_boardTop; }
static inline int boardRight() { return boardLeft() + T_COLS * T_CELL; }
// Left panel: score/lines/level
static inline int panelX()     { return 2; }
static inline int panelY()     { return s_boardTop; }
static inline int panelW()     { return boardLeft() - 4; }
// Right panel: next piece preview
static inline int nextX()      { return boardRight() + 4; }
static inline int nextY()      { return s_boardTop; }
static inline int nextPanelW() { return uiTft().width() - boardRight() - 4; }

// ─── Draw helpers ─────────────────────────────────────────────────────────────
static void drawCell(int row, int col, uint16_t color) {
    int x = boardLeft() + col * T_CELL;
    int y = boardTop()  + row * T_CELL;
    auto& tft = uiTft();
    if (color == ILI9341_BLACK) {
        tft.fillRect(x, y, T_CELL, T_CELL, ILI9341_BLACK);
    } else {
        tft.fillRect(x + 1, y + 1, T_CELL - 2, T_CELL - 2, color);
        // Lighter top/left edge highlight for 3D effect
        tft.drawFastHLine(x + 1, y + 1, T_CELL - 2, (uint16_t)(color | 0x8410));
        tft.drawFastVLine(x + 1, y + 1, T_CELL - 2, (uint16_t)(color | 0x8410));
    }
}

static void drawBoard() {
    auto& tft = uiTft();
    // Border
    tft.drawRect(boardLeft() - 1, boardTop() - 1, T_COLS * T_CELL + 2, T_ROWS * T_CELL + 2, uiColorDivider());
    // Cells
    for (int r = 0; r < T_ROWS; r++) {
        for (int c = 0; c < T_COLS; c++) {
            if (t_board[r][c]) drawCell(r, c, PIECE_COLORS[t_board[r][c]]);
            else drawCell(r, c, ILI9341_BLACK);
        }
    }
}

static void erasePiece() {
    for (int i = 0; i < 4; i++) {
        int c = t_px + PIECES[t_piece][t_rot][i][0];
        int r = t_py + PIECES[t_piece][t_rot][i][1];
        if (r >= 0 && r < T_ROWS && c >= 0 && c < T_COLS) {
            uint16_t boardColor = t_board[r][c] ? PIECE_COLORS[t_board[r][c]] : ILI9341_BLACK;
            drawCell(r, c, boardColor);
        }
    }
}

static void drawPiece() {
    for (int i = 0; i < 4; i++) {
        int c = t_px + PIECES[t_piece][t_rot][i][0];
        int r = t_py + PIECES[t_piece][t_rot][i][1];
        if (r >= 0 && r < T_ROWS && c >= 0 && c < T_COLS)
            drawCell(r, c, PIECE_COLORS[t_piece + 1]);
    }
}

// Ghost piece (drop shadow)
static void drawGhost() {
    int gy = t_py;
    while (true) {
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            int c = t_px + PIECES[t_piece][t_rot][i][0];
            int r = gy + 1 + PIECES[t_piece][t_rot][i][1];
            if (r >= T_ROWS || (r >= 0 && t_board[r][c] != 0)) { ok = false; break; }
        }
        if (!ok) break;
        gy++;
    }
    t_ghostY = gy;
    if (t_ghostY == t_py || t_ghostY == -1) return; // already at bottom
    
    auto& tft = uiTft();
    for (int i = 0; i < 4; i++) {
        int c = t_px + PIECES[t_piece][t_rot][i][0];
        int r = t_ghostY + PIECES[t_piece][t_rot][i][1];
        if (r >= 0 && r < T_ROWS && c >= 0 && c < T_COLS && t_board[r][c] == 0) {
            int x = boardLeft() + c * T_CELL;
            int y = boardTop()  + r * T_CELL;
            tft.drawRect(x + 1, y + 1, T_CELL - 2, T_CELL - 2, 0x4208); // very dark grey outline
        }
    }
}

static void eraseGhost() {
    if (t_ghostY == -1 || t_ghostY == t_py) return;
    for (int i = 0; i < 4; i++) {
        int c = t_px + PIECES[t_piece][t_rot][i][0];
        int r = t_ghostY + PIECES[t_piece][t_rot][i][1];
        if (r >= 0 && r < T_ROWS && c >= 0 && c < T_COLS && t_board[r][c] == 0) {
            drawCell(r, c, ILI9341_BLACK);
        }
    }
    t_ghostY = -1;
}

static void drawNextPreview() {
    auto& tft = uiTft();
    int pw = nextPanelW();
    // Clear the whole right panel
    tft.fillRect(nextX(), nextY(), pw, T_ROWS * T_CELL, ILI9341_BLACK);

    tft.setFont(nullptr);
    tft.setTextSize(1);
    tft.setTextColor(uiColorSub());
    tft.setCursor(nextX(), nextY());
    tft.print("NEXT");

    // Preview box: 4 cells wide + padding
    int bw = T_CELL * 4 + 4;
    int bh = T_CELL * 4 + 4;
    // Keep box inside screen
    if (nextX() + bw > uiTft().width() - 1) bw = uiTft().width() - 1 - nextX();
    tft.drawRect(nextX(), nextY() + 10, bw, bh, uiColorDivider());
    tft.fillRect(nextX() + 1, nextY() + 11, bw - 2, bh - 2, ILI9341_BLACK);

    uint16_t color = PIECE_COLORS[t_next + 1];
    // Draw 4 cells centred inside the box
    int cx = nextX() + 2;
    int cy = nextY() + 12;
    for (int i = 0; i < 4; i++) {
        int c = PIECES[t_next][0][i][0];
        int r = PIECES[t_next][0][i][1];
        int px = cx + c * T_CELL;
        int py = cy + r * T_CELL;
        // Guard: don't draw outside box
        if (px + T_CELL - 1 < nextX() + bw && py + T_CELL - 1 < nextY() + 10 + bh) {
            tft.fillRect(px + 1, py + 1, T_CELL - 2, T_CELL - 2, color);
        }
    }
}

static void drawScorePanel() {
    auto& tft = uiTft();
    int x = panelX(), y = panelY();
    int pw = panelW();
    if (pw < 2) return; // nothing to draw
    tft.fillRect(x, y, pw, T_ROWS * T_CELL, ILI9341_BLACK);
    tft.setFont(nullptr);
    tft.setTextSize(1);

    tft.setTextColor(uiColorSub());
    tft.setCursor(x, y);      tft.print("SCR");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(x, y + 10); tft.print(t_score);

    tft.setTextColor(uiColorSub());
    tft.setCursor(x, y + 26);  tft.print("LN");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(x, y + 36);  tft.print(t_lines);

    tft.setTextColor(uiColorSub());
    tft.setCursor(x, y + 52);  tft.print("LVL");
    uint16_t lvlColor = (t_level < 3) ? ILI9341_GREEN : (t_level < 6) ? ILI9341_YELLOW : ILI9341_RED;
    tft.setTextColor(lvlColor);
    tft.setCursor(x, y + 62);  tft.print(t_level);

    // Draw Hold
    tft.setTextColor(uiColorSub());
    tft.setCursor(x, y + 84); tft.print("HOLD");
    int bw = T_CELL * 4 + 4;
    int bh = T_CELL * 4 + 4;
    if (x + bw > boardLeft() - 1) bw = boardLeft() - 1 - x;
    tft.drawRoundRect(x, y + 94, bw, bh, 3, uiColorDivider());
    
    if (t_hold != -1) {
        uint16_t color = PIECE_COLORS[t_hold + 1];
        int cx = x + 2;
        int cy = y + 96;
        for (int i = 0; i < 4; i++) {
            int c = PIECES[t_hold][0][i][0];
            int r = PIECES[t_hold][0][i][1];
            int px = cx + c * T_CELL;
            int py = cy + r * T_CELL;
            if (px + T_CELL - 1 < x + bw && py + T_CELL - 1 < y + 94 + bh) {
                uiTft().fillRect(px + 1, py + 1, T_CELL - 2, T_CELL - 2, color);
            }
        }
    }
}

static void drawGameOverOverlay() {
    auto& tft = uiTft();
    int bw = 120, bh = 44;
    int bx = uiTft().width() / 2 - bw / 2;
    int by = boardTop() + (T_ROWS * T_CELL) / 2 - bh / 2;
    tft.fillRoundRect(bx, by, bw, bh, 5, ILI9341_BLACK);
    tft.drawRoundRect(bx, by, bw, bh, 5, ILI9341_RED);
    tft.setFont(nullptr);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(bx + 12, by + 5);
    tft.print("GAME");
    tft.setCursor(bx + 12, by + 23);
    tft.print("OVER");
    tft.setTextSize(1);
    tft.setTextColor(uiColorSub());
    tft.setCursor(bx + 8, by - 10);
    tft.print("Enc/Tap to retry");
}

static void drawFooter() {
    auto& tft = uiTft();
    // Use the ACTUAL screen footer position, not body+bodyH which can differ
    int fy = uiTft().height() - uiFooterH();
    tft.fillRect(0, fy, uiTft().width(), uiFooterH(), uiColorBg());
    tft.drawFastHLine(0, fy, uiTft().width(), uiColorDivider());
    tft.setFont(nullptr);
    tft.setTextSize(1);
    tft.setTextColor(uiColorSub());
    tft.setCursor(4, fy + uiFooterH() / 2 + 3);
    tft.print("Enc:Move  Tap:Rot  JoyD:SoftDrop  Enc:Drop");
}

// ─── Game logic ───────────────────────────────────────────────────────────────
static bool checkCollision(int px, int py, int rot) {
    for (int i = 0; i < 4; i++) {
        int c = px + PIECES[t_piece][rot][i][0];
        int r = py + PIECES[t_piece][rot][i][1];
        if (c < 0 || c >= T_COLS || r >= T_ROWS) return true;
        if (r >= 0 && t_board[r][c] != 0) return true;
    }
    return false;
}

static void lockPiece() {
    for (int i = 0; i < 4; i++) {
        int c = t_px + PIECES[t_piece][t_rot][i][0];
        int r = t_py + PIECES[t_piece][t_rot][i][1];
        if (r >= 0 && r < T_ROWS && c >= 0 && c < T_COLS)
            t_board[r][c] = (uint8_t)(t_piece + 1);
    }

    // Clear full lines
    int cleared = 0;
    for (int r = T_ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < T_COLS; c++) {
            if (!t_board[r][c]) { full = false; break; }
        }
        if (full) {
            cleared++;
            auto& tft = uiTft();
            tft.fillRect(boardLeft() - 1, boardTop() + r * T_CELL, T_COLS * T_CELL + 2, T_CELL, ILI9341_WHITE);
            delay(40);
            
            for (int k = r; k > 0; k--)
                for (int c = 0; c < T_COLS; c++)
                    t_board[k][c] = t_board[k - 1][c];
            for (int c = 0; c < T_COLS; c++) t_board[0][c] = 0;
            r++; // re-check same row
            drawBoard();
        }
    }
    if (cleared > 0) {
        const int pts[] = {0, 100, 300, 500, 800};
        t_score += pts[min(cleared, 4)] * t_level;
        t_lines += cleared;
        t_level = t_lines / 10 + 1;
        t_fallSpeed = (uint32_t)max(80, 500 - (t_level - 1) * 40);
        drawScorePanel();
    }
    t_canHold = true;
}

static void spawnNewPiece() {
    t_piece = t_next;
    t_next  = esp_random() % 7;
    t_rot   = 0;
    t_px    = T_COLS / 2 - 2;
    t_py    = -1;
    drawNextPreview();
    drawScorePanel();
    if (checkCollision(t_px, t_py, t_rot)) {
        t_over = true;
        drawGameOverOverlay();
    } else {
        t_ghostY = -1;
        drawGhost();
        drawPiece();
    }
}

static void doRotate() {
    int nextRot = (t_rot + 1) % 4;
    // Wall kick: try centre, then shift right, left
    if (!checkCollision(t_px, t_py, nextRot)) {
        erasePiece(); eraseGhost();
        t_rot = nextRot;
        drawGhost(); drawPiece();
    } else if (!checkCollision(t_px + 1, t_py, nextRot)) {
        erasePiece(); eraseGhost();
        t_rot = nextRot; t_px++;
        drawGhost(); drawPiece();
    } else if (!checkCollision(t_px - 1, t_py, nextRot)) {
        erasePiece(); eraseGhost();
        t_rot = nextRot; t_px--;
        drawGhost(); drawPiece();
    }
}

static void doHardDrop() {
    erasePiece(); eraseGhost();
    while (!checkCollision(t_px, t_py + 1, t_rot)) t_py++;
    drawPiece();
    lockPiece();
    spawnNewPiece();
}

static void doHold() {
    if (!t_canHold) return;
    erasePiece(); eraseGhost();
    int oldPiece = t_piece;
    if (t_hold == -1) {
        t_hold = oldPiece;
        t_canHold = false;
        spawnNewPiece(); 
    } else {
        t_piece = t_hold;
        t_hold = oldPiece;
        t_rot = 0;
        t_px = T_COLS / 2 - 2;
        t_py = -1;
        t_canHold = false;
        drawScorePanel();
        if (checkCollision(t_px, t_py, t_rot)) {
            t_over = true;
            drawGameOverOverlay();
        } else {
            t_ghostY = -1;
            drawGhost(); 
            drawPiece();
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void tetrisEnter() {
    // Compute cell size so the board fits exactly within the body area
    // bodyH - 6px padding = usable height; divide by T_ROWS; clamp min 7
    T_CELL = (uiBodyH() - 6) / T_ROWS;
    if (T_CELL < 7) T_CELL = 7;
    if (T_CELL > 12) T_CELL = 12;

    // Align board's BOTTOM border flush with the body's bottom edge (no gap).
    // Board border bottom = s_boardTop + T_ROWS*T_CELL + 1
    // Body bottom        = uiBodyY0() + uiBodyH() - 2  (2px margin above footer)
    s_boardTop = uiBodyY0() + uiBodyH() - 2 - T_ROWS * T_CELL - 1;
    // Clamp: never go above the very top of the body
    if (s_boardTop < uiBodyY0() + 1) s_boardTop = uiBodyY0() + 1;

    for (int r = 0; r < T_ROWS; r++)
        for (int c = 0; c < T_COLS; c++)
            t_board[r][c] = 0;

    t_score     = 0;
    t_lines     = 0;
    t_level     = 1;
    t_over      = false;
    t_fallSpeed = 500;
    t_lastFall  = millis();
    t_joyHTime  = 0;
    t_joyVTime  = 0;
    t_next      = esp_random() % 7;
    t_hold      = -1;
    t_canHold   = true;
    t_ghostY    = -1;

    uiShowScreenTitle("TETRIS");
    uiTft().fillRect(0, uiBodyY0(), uiTft().width(), uiBodyH(), ILI9341_BLACK);
    drawBoard();
    drawFooter();
    spawnNewPiece();
}

void tetrisLoop(uint32_t now) {
    if (t_over) return;

    // Joystick horizontal
    if (joyEnabled()) {
        int nx = joyNormX();
        int ny = joyNormY();

        if (now - t_joyHTime > 130) {
            if (nx < -25) {
                if (!checkCollision(t_px - 1, t_py, t_rot)) {
                    erasePiece(); eraseGhost();
                    t_px--;
                    drawGhost(); drawPiece();
                }
                t_joyHTime = now;
            } else if (nx > 25) {
                if (!checkCollision(t_px + 1, t_py, t_rot)) {
                    erasePiece(); eraseGhost();
                    t_px++;
                    drawGhost(); drawPiece();
                }
                t_joyHTime = now;
            }
        }

        // Joystick down = soft drop (x3 speed); up = rotate
        if (now - t_joyVTime > 120) {
            if (ny < -25) {
                doRotate();
                t_joyVTime = now;
            } else if (ny > 25) {
                // Accelerate fall
                t_lastFall = now - t_fallSpeed;
                t_joyVTime = now;
            }
        }
    }

    // Gravity
    if (now - t_lastFall > t_fallSpeed) {
        t_lastFall = now;
        if (!checkCollision(t_px, t_py + 1, t_rot)) {
            erasePiece(); eraseGhost();
            t_py++;
            drawGhost(); drawPiece();
        } else {
            drawPiece();
            lockPiece();
            spawnNewPiece();
        }
    }
}

void tetrisOnTouch(const TouchEvent& ev) {
    if (t_over && ev.type == TE_TAP) { tetrisEnter(); return; }
    if (t_over) return;
    if (ev.type == TE_TAP) {
        doRotate();
    } else if (ev.type == TE_SWIPE_DOWN) {
        doHardDrop();
    } else if (ev.type == TE_SWIPE_UP) {
        doHold();
    } else if (ev.type == TE_SWIPE_LEFT) {
        if (!checkCollision(t_px - 1, t_py, t_rot)) {
            erasePiece(); eraseGhost(); t_px--;
            drawGhost(); drawPiece();
        }
    } else if (ev.type == TE_SWIPE_RIGHT) {
        if (!checkCollision(t_px + 1, t_py, t_rot)) {
            erasePiece(); eraseGhost(); t_px++;
            drawGhost(); drawPiece();
        }
    }
}

void tetrisOnEncoderDelta(int d) {
    if (d == 0 || t_over) return;
    if (!checkCollision(t_px + d, t_py, t_rot)) {
        erasePiece(); eraseGhost();
        t_px += d;
        drawGhost(); drawPiece();
    }
}

void tetrisOnEncoderClick() {
    if (t_over) { tetrisEnter(); return; }
    doHardDrop();
}

#include "game_painter.h"
#include "ui.h"
#include "joystick.h"
#include "games.h"
#include <Arduino.h>

// ─── Pixel Art Painter ───────────────────────────────────────────────────────
// 20×18 grid, cells sized to fit display body area.
// Palette shown as a row at the bottom of the body.
// Controls:
//   Joystick     : Move cursor
//   Encoder click: Place pixel
//   Encoder turn : Change colour
//   Touch        : Draw (tap or drag)
//   Long press   : Back to menu (handled by main loop)

static const int GRID_COLS = 20;
static const int GRID_ROWS = 18;

static uint8_t p_grid[GRID_ROWS][GRID_COLS]; // colour index per cell
static int     p_color = 2; // current selected colour (not 0=black bg)
static int     p_curX  = 10;
static int     p_curY  = 9;

// Palette of 14 colours (index 0 = eraser/background)
static const uint16_t P_COLORS[14] = {
    ILI9341_BLACK,
    ILI9341_WHITE,
    ILI9341_RED,
    ILI9341_GREEN,
    ILI9341_BLUE,
    ILI9341_YELLOW,
    ILI9341_CYAN,
    ILI9341_MAGENTA,
    ILI9341_ORANGE,
    0xF81F, // hot pink
    ILI9341_PURPLE,
    ILI9341_DARKGREY,
    0x07FF, // sky blue
    0xFFE0, // light yellow
};
static const int P_COLOR_COUNT = 14;

// Computed at enter() time
static int g_cellW = 0;
static int g_cellH = 0;
static int g_gridX = 0;
static int g_gridY = 0;
static int g_palY  = 0;    // Y of the palette row
static int g_palH  = 16;   // height of palette strip
static int g_footerY = 0;

static uint32_t lastJoyMove = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────
static void drawCell(int col, int row) {
    auto& tft = uiTft();
    int x = g_gridX + col * g_cellW;
    int y = g_gridY + row * g_cellH;
    uint16_t fill = P_COLORS[p_grid[row][col]];
    // Fill cell (leave 1px for grid line on right and bottom)
    tft.fillRect(x, y, g_cellW - 1, g_cellH - 1, fill);
    // Grid dividers
    tft.drawFastVLine(x + g_cellW - 1, y, g_cellH, 0x2104);
    tft.drawFastHLine(x, y + g_cellH - 1, g_cellW, 0x2104);
}

static void drawGrid() {
    auto& tft = uiTft();
    // Background
    tft.fillRect(g_gridX - 1, g_gridY - 1,
                 GRID_COLS * g_cellW + 2, GRID_ROWS * g_cellH + 2,
                 ILI9341_DARKGREY);
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            drawCell(c, r);
}

static void drawCursor(bool erase) {
    auto& tft = uiTft();
    int x = g_gridX + p_curX * g_cellW;
    int y = g_gridY + p_curY * g_cellH;
    uint16_t col = erase ? 0x2104 /* dark grey */ : ILI9341_WHITE;
    tft.drawRect(x, y, g_cellW - 1, g_cellH - 1, col);
    if (!erase) {
        // inner highlight
        if (g_cellW > 4 && g_cellH > 4)
            tft.drawRect(x + 1, y + 1, g_cellW - 3, g_cellH - 3, col);
    }
}

static void drawPalette() {
    auto& tft = uiTft();
    int sw = uiTft().width();
    tft.fillRect(0, g_palY, sw, g_palH, 0x1082);
    // Each colour slot: exact division to prevent rounding errors
    float slotWf = (float)sw / P_COLOR_COUNT;
    for (int i = 0; i < P_COLOR_COUNT; i++) {
        int bx = (int)(i * slotWf);
        int bw = (int)((i + 1) * slotWf) - bx;
        tft.fillRect(bx, g_palY, bw, g_palH, P_COLORS[i]);
        if (i == p_color) {
            tft.drawRect(bx, g_palY, bw, g_palH, ILI9341_WHITE);
        }
        // Eraser: index 0 = black = draw X
        if (i == 0) {
            tft.drawLine(bx + 2, g_palY + 2, bx + bw - 3, g_palY + g_palH - 3, ILI9341_WHITE);
            tft.drawLine(bx + bw - 3, g_palY + 2, bx + 2, g_palY + g_palH - 3, ILI9341_WHITE);
        }
    }
}

static void drawFooter() {
    auto& tft = uiTft();
    tft.fillRect(0, g_footerY, uiTft().width(), uiFooterH(), uiColorBg());
    tft.drawFastHLine(0, g_footerY, uiTft().width(), uiColorDivider());
    tft.setFont(nullptr);
    tft.setTextSize(1);
    tft.setTextColor(uiColorSub());
    tft.setCursor(4, g_footerY + uiFooterH() / 2 + 3);
    tft.print("Joy:Cursor  Enc:Color  Click:Paint  Touch:Draw");
}

// ─── Setters ─────────────────────────────────────────────────────────────────
static void paintAt(int col, int row) {
    if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return;
    p_grid[row][col] = (uint8_t)p_color;
    drawCell(col, row);
    // Redraw cursor if it's over the just-painted cell
    if (col == p_curX && row == p_curY) drawCursor(false);
}

static void moveCursor(int dx, int dy) {
    drawCursor(true);
    drawCell(p_curX, p_curY); // restore cell underneath
    p_curX = (p_curX + dx + GRID_COLS) % GRID_COLS;
    p_curY = (p_curY + dy + GRID_ROWS) % GRID_ROWS;
    drawCursor(false);
}

// ─── Public API ───────────────────────────────────────────────────────────────
void painterEnter() {
    auto& tft = uiTft();
    int sw = tft.width();
    int bY = uiBodyY0();
    int bH = uiBodyH();

    // Reserve 18px for palette strip + 2px gap
    g_palH  = 18;
    int gridAreaH = bH - g_palH - 6;
    // Calculate cell size independently for W and H to fill the full area
    // This is the key fix: don't force square — touch grid must exactly match pixels
    g_cellW = sw / GRID_COLS;           // e.g. 320/20 = 16px wide
    g_cellH = gridAreaH / GRID_ROWS;    // e.g. 170/18 = ~9px tall
    // Floor to at least 4 so it's visible
    if (g_cellW < 4) g_cellW = 4;
    if (g_cellH < 4) g_cellH = 4;

    int totalW = GRID_COLS * g_cellW;
    int totalH = GRID_ROWS * g_cellH;
    // Center horizontally; grid starts at body top
    g_gridX   = (sw - totalW) / 2;
    g_gridY   = bY + 2;
    g_palY    = g_gridY + totalH + 4;
    g_footerY = uiTft().height() - uiFooterH();

    p_color = 2; // start with red
    p_curX  = GRID_COLS / 2;
    p_curY  = GRID_ROWS / 2;

    // Clear grid
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            p_grid[r][c] = 0;

    lastJoyMove = 0;

    uiShowScreenTitle("PIXEL ART");
    tft.fillRect(0, bY, sw, bH, ILI9341_BLACK);
    drawGrid();
    drawPalette();
    drawCursor(false);
    drawFooter();
}

void painterLoop(uint32_t now) {
    if (!joyEnabled()) return;
    if (now - lastJoyMove < 160) return;

    int nx = joyNormX();
    int ny = joyNormY();
    if (abs(nx) > 30 || abs(ny) > 30) {
        int dx = (nx < -30) ? -1 : (nx > 30) ? 1 : 0;
        int dy = (ny < -30) ? -1 : (ny > 30) ? 1 : 0;
        moveCursor(dx, dy);
        lastJoyMove = now;
    }
}

void painterOnTouch(const TouchEvent& ev) {
    if (ev.type == TE_DOWN || ev.type == TE_DRAG || ev.type == TE_TAP) {
        int sw = uiTft().width();

        // Check palette area — use float math to match drawPalette's float slots
        if (ev.y >= g_palY && ev.y < g_palY + g_palH) {
            float slotWf = (float)sw / P_COLOR_COUNT;
            int idx = (int)(ev.x / slotWf);
            if (idx < 0) idx = 0;
            if (idx >= P_COLOR_COUNT) idx = P_COLOR_COUNT - 1;
            p_color = idx;
            drawPalette();
            return;
        }

        // Check grid area — use same integer math as drawCell
        int gridRight  = g_gridX + GRID_COLS * g_cellW;
        int gridBottom = g_gridY + GRID_ROWS * g_cellH;
        if (ev.x >= g_gridX && ev.x < gridRight &&
            ev.y >= g_gridY && ev.y < gridBottom) {
            // Exact cell index = (touch - gridOrigin) / cellSize
            int col = (ev.x - g_gridX) / g_cellW;
            int row = (ev.y - g_gridY) / g_cellH;
            if (col < 0) col = 0;
            if (col >= GRID_COLS) col = GRID_COLS - 1;
            if (row < 0) row = 0;
            if (row >= GRID_ROWS) row = GRID_ROWS - 1;
            // Move cursor
            if (col != p_curX || row != p_curY) {
                drawCursor(true);
                drawCell(p_curX, p_curY);
                p_curX = col;
                p_curY = row;
            }
            paintAt(col, row);
        }
    }
}

void painterOnEncoderDelta(int d) {
    if (d == 0) return;
    p_color = (p_color + d + P_COLOR_COUNT) % P_COLOR_COUNT;
    drawPalette();
}

void painterOnEncoderClick() {
    paintAt(p_curX, p_curY);
}

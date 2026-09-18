/* video_render.cpp – FreeBSD style
 *
 * The PORTABLE half of the video engine: framebuffer + text grid state and
 * every drawing/compositing routine. No pico or hardware headers — this TU
 * must compile on a desktop host unchanged, so a simulator (USim video
 * window) renders with bit-identical code to the firmware. The hardware
 * scan-out half (HSTX, ping-pong DMA, vsync) lives in video.cpp and touches
 * this TU only through video_render.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "video.h"
#include "video_render.h"
#include "font_spleen_8x16.h"

/* ───────────────────────── compile-time parameters ──────────────────────── */

/* Screen geometry (FB_WIDTH/FB_HEIGHT/CHAR_W/CHAR_H/TEXT_COLS/TEXT_ROWS) is the
 * single source of truth in video.h, so consumers cannot drift from it. */

#define TRANSPARENT_BG  0x00

/* Force -O3 on hot rendering functions regardless of the build type. Only GCC
 * implements the optimize function attribute; clang-based toolchains (ATfE)
 * reject it, so there the macro is empty and the build's own level applies. */
#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMIZE_O3 __attribute__((optimize("O3")))
#else
#define OPTIMIZE_O3
#endif

/* GPIO_HSTX_BASE / GPIO_HSTX_COUNT come from the shared pin map. */

/* ──────────────────────────── framebuffer (SRAM) ──────────────────────── */

// RGB332 framebuffer in SRAM. DMA reads directly from here.
uint8_t video_framebuffer[FB_HEIGHT][FB_WIDTH] __attribute__((aligned(4)));

/* ─────────────────────────── text layer (SRAM) ──────────────────────────── */

static uint8_t text_cells[TEXT_ROWS][TEXT_COLS];
static uint8_t text_attrs[TEXT_ROWS][TEXT_COLS];
#define DIRTY_WORDS ((TEXT_COLS + 31) / 32)  /* ceil(cols/32) */
static uint32_t dirty_rows[TEXT_ROWS][DIRTY_WORDS];

/* ──────────────────────────── RGB332 helpers ────────────────────────────── */

/* RGB332: RRRGGGBB — 3 bits red, 3 bits green, 2 bits blue */
static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0x07) << 5) | ((g & 0x07) << 2) | (b & 0x03);
}

static inline uint8_t attr_to_fg(uint8_t attr)
{
    uint8_t r = (attr >> 7) & 1;
    uint8_t g = (attr >> 6) & 1;
    uint8_t b = (attr >> 5) & 1;
    return rgb332(r ? 7 : 0, g ? 7 : 0, b ? 3 : 0);
}

static inline uint8_t attr_to_bg(uint8_t attr)
{
    uint8_t r = (attr >> 3) & 1;
    uint8_t g = (attr >> 2) & 1;
    uint8_t b = (attr >> 1) & 1;

    if (attr & 0x01) return TRANSPARENT_BG;

    return rgb332(r ? 2 : 0, g ? 2 : 0, b ? 1 : 0);
}

/* ─────────────────────────────── 8×16 font ─────────────────────────────── */

static inline uint8_t font_row(uint8_t ch, int row)
{
    return font_spleen_8x16[ch][row & (CHAR_H - 1)];
}

/* ────────────────────── text cell rendering (blitting) ──────────────────── */

static void OPTIMIZE_O3
blit_char_cell(int col, int row)
{
    uint8_t ch   = text_cells[row][col];
    uint8_t attr = text_attrs[row][col];

    uint8_t fg = attr_to_fg(attr);
    uint8_t bg = attr_to_bg(attr);
    bool transparent = (bg == TRANSPARENT_BG);

    int x = col * CHAR_W;
    int y = row * CHAR_H;

    for (int gy = 0; gy < CHAR_H; gy++) {
        uint8_t mask = font_row(ch, gy);
        uint8_t *line = &video_framebuffer[y + gy][x];

        for (int gx = 0; gx < CHAR_W; gx++) {
            bool is_fg = (mask & (0x80 >> gx)) != 0;

            if (is_fg) {
                line[gx] = fg;
            } else if (!transparent) {
                line[gx] = bg;
            }
        }
    }
}

static void render_dirty_cells(void)
{
    for (int row = 0; row < TEXT_ROWS; row++) {
        {
            uint32_t any = 0;
            for (int w = 0; w < DIRTY_WORDS; w++) any |= dirty_rows[row][w];
            if (!any) continue;
        }

        for (int col = 0; col < TEXT_COLS; col++) {
            int word = col >> 5;
            uint32_t bit = 1u << (col & 31);
            if (dirty_rows[row][word] & bit) {
                blit_char_cell(col, row);
                dirty_rows[row][word] &= ~bit;
            }
        }
    }
}

/* ───────────────────────── public API (text layer) ──────────────────────── */

void vidterm_putch(int col, int row, uint8_t ch, uint8_t attr)
{
    if (col < 0 || col >= TEXT_COLS || row < 0 || row >= TEXT_ROWS) return;

    text_cells[row][col] = ch;
    text_attrs[row][col] = attr;
    dirty_rows[row][col >> 5] |= (1u << (col & 31));
}

void vidterm_puts(int col, int row, const char *s, uint8_t attr)
{
    while (*s && row < TEXT_ROWS) {
        vidterm_putch(col, row, (uint8_t)*s++, attr);
        if (++col >= TEXT_COLS) { col = 0; row++; }
    }
}

void vidterm_clear(uint8_t ch, uint8_t attr)
{
    memset(text_cells, ch,   sizeof(text_cells));
    memset(text_attrs, attr, sizeof(text_attrs));
    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int w = 0; w < DIRTY_WORDS; w++)
            dirty_rows[r][w] = 0xFFFFFFFF;
        /* Mask off unused bits in the last word */
        if (TEXT_COLS & 31)
            dirty_rows[r][DIRTY_WORDS - 1] = (1u << (TEXT_COLS & 31)) - 1;
    }
}

void vidterm_refresh(void)
{
    render_dirty_cells();
}

/* ───────────────────────── public API (graphics layer) ──────────────────── */

static inline void plot_unlocked(int x, int y, uint8_t colour)
{
    if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT)
        video_framebuffer[y][x] = colour;
}

void vidterm_plot(int x, int y, uint8_t colour)
{
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    video_framebuffer[y][x] = colour;
}

void vidterm_fill_rect(int x, int y, int w, int h, uint8_t colour)
{
    for (int row = y; row < y + h && row < FB_HEIGHT; row++) {
        if (row < 0) continue;
        int x0 = (x < 0) ? 0 : x;
        int x1 = (x + w > FB_WIDTH) ? FB_WIDTH : x + w;
        for (int i = x0; i < x1; i++) {
            video_framebuffer[row][i] = colour;
        }
    }
}

void vidterm_hline(int x, int y, int len, uint8_t colour)
{
    if (y < 0 || y >= FB_HEIGHT) return;
    int x0 = (x < 0) ? 0 : x;
    int x1 = (x + len > FB_WIDTH) ? FB_WIDTH : x + len;
    for (int i = x0; i < x1; i++) {
        video_framebuffer[y][i] = colour;
    }
}

void vidterm_vline(int x, int y, int len, uint8_t colour)
{
    if (x < 0 || x >= FB_WIDTH) return;
    int y0 = (y < 0) ? 0 : y;
    int y1 = (y + len > FB_HEIGHT) ? FB_HEIGHT : y + len;
    for (int i = y0; i < y1; i++) {
        video_framebuffer[i][x] = colour;
    }
}

void vidterm_clear_graphics(uint8_t colour)
{
    memset(video_framebuffer, colour, sizeof(video_framebuffer));
}

void vidterm_circle(int cx, int cy, int radius, uint8_t colour)
{
    int x = 0, y = radius;
    int d = 3 - 2 * radius;

    while (x <= y) {
        plot_unlocked(cx + x, cy + y, colour);
        plot_unlocked(cx - x, cy + y, colour);
        plot_unlocked(cx + x, cy - y, colour);
        plot_unlocked(cx - x, cy - y, colour);
        plot_unlocked(cx + y, cy + x, colour);
        plot_unlocked(cx - y, cy + x, colour);
        plot_unlocked(cx + y, cy - x, colour);
        plot_unlocked(cx - y, cy - x, colour);

        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* ─────────────────────────────── bring-up ───────────────────────────────── */

void video_render_init(void)
{
    memset(video_framebuffer, 0, sizeof(video_framebuffer));
    memset(text_cells, 0, sizeof(text_cells));
    memset(text_attrs, 0, sizeof(text_attrs));
    memset(dirty_rows, 0, sizeof(dirty_rows));
}

/* ────────────────────────────── test screen ─────────────────────────────── */

#ifdef VIDEO_DEMO
void video_demo(void)
{
    // Blue background with a 1-pixel white border.
    vidterm_clear_graphics(rgb332(0, 0, 3));
    uint8_t white = rgb332(7, 7, 3);
    vidterm_hline(0, 0, FB_WIDTH, white);
    vidterm_hline(0, FB_HEIGHT - 1, FB_WIDTH, white);
    vidterm_vline(0, 0, FB_HEIGHT, white);
    vidterm_vline(FB_WIDTH - 1, 0, FB_HEIGHT, white);

    // Text: row numbers, a column ruler, and the full character set.
    vidterm_clear(' ', 0xE1);  /* white on transparent */

    char buf[4];
    for (int r = 0; r < TEXT_ROWS; r++) {
        snprintf(buf, sizeof(buf), "%2d", r);
        vidterm_puts(1, r, buf, 0xE1);
    }
    for (int c = 0; c < TEXT_COLS; c++)
        vidterm_putch(c, 1, (c % 10 == 0) ? '|' : '-', 0xC1);  /* yellow */
    for (int c = 0; c < TEXT_COLS; c += 10) {
        snprintf(buf, sizeof(buf), "%d", c);
        vidterm_puts(c, 0, buf, 0xC1);
    }
    int col = 4, row = 3;
    for (int ch = 0x20; ch < 0x100; ch++) {
        vidterm_putch(col, row, (uint8_t)ch, 0xE1);
        col += 2;  /* double-spaced */
        if (col >= TEXT_COLS - 1) {
            col = 4;
            row += 2;
            if (row >= TEXT_ROWS) break;
        }
    }
    vidterm_refresh();

    // Diagonal corner-to-corner lines over the graphics layer.
    uint8_t red = rgb332(7, 0, 0), green = rgb332(0, 7, 0);
    for (int i = 0; i < FB_HEIGHT; i++) {
        int x1 = (i * (FB_WIDTH - 1)) / (FB_HEIGHT - 1);
        int x2 = (FB_WIDTH - 1) - x1;
        vidterm_plot(x1, i, red);
        vidterm_plot(x2, i, green);
    }
}
#endif  /* VIDEO_DEMO */

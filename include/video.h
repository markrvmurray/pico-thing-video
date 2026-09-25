/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Video engine for pico-thing-video — an HSTX->DVI text+graphics output,
 * spliced from ../pico-vidterm. Drives 640x480-class DVI on GP12-19, with an
 * RGB332 framebuffer in SRAM composited with an 8x16 text layer.
 *
 * The output runs autonomously off a ping-pong DMA pair fed by a per-scanline
 * IRQ (DMA_IRQ_2 — the bus owns DMA_IRQ_0/1). video_init() brings up the HSTX
 * encoder + DMA and must be called from the core that will service the video
 * IRQ (Core 1 in this project).
 */

#pragma once

#include <stdint.h>

/* ── Screen geometry (single source of truth) ────────────────────────────────
 * The active resolution and text-cell size. These describe the video *mode*,
 * not the board — both the SolderParty and Pimoroni mappings scan out the same
 * 1024x480 RGB332 buffer; only the DVI lane->pin routing differs (that lives in
 * video.cpp's board-gated hstx_dvi_init()). Consumers that map pixels to text
 * cells (e.g. the USB-host mouse cursor) include this header so they cannot
 * drift from the framebuffer.
 *
 * 1024 active: 1056 overflows SRAM by ~6 KB, so the dropped 32 columns go into
 * the H back porch, leaving the 60 Hz horizontal total unchanged. */
#define FB_WIDTH       1024
#define FB_HEIGHT      480

#define CHAR_W           8
#define CHAR_H          16
#define TEXT_COLS       (FB_WIDTH / CHAR_W)   /* 128 */
#define TEXT_ROWS       (FB_HEIGHT / CHAR_H)  /*  30 */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the HSTX DVI encoder + ping-pong DMA and start scanning out the
 * framebuffer. Call once, from the core that owns the video IRQ (Core 1). */
void video_init(void);

#ifdef VIDEO_DEMO
/* Draw the built-in test screen (border, ruler, full character set, diagonals).
 * Test-only: gated by VIDEO_DEMO (default on). Draws once and returns; the DMA
 * keeps scanning the result out. */
void video_demo(void);
#endif

/* ── Text layer ─────────────────────────────────────────────────────────── */
void vidterm_putch(int col, int row, uint8_t ch, uint8_t attr);
void vidterm_puts(int col, int row, const char *s, uint8_t attr);
/* Returns TRUE if it painted the panel itself, in which case nothing is dirty
 * and no other fill is needed. A whole-screen write costs about 16 ms against
 * the scan-out DMA, so a second one for the same pixels is most of a frame
 * thrown away. */
bool vidterm_clear(uint8_t ch, uint8_t attr);
void vidterm_refresh(void);   /* blit dirty text cells into the framebuffer */
void vidterm_scroll_up(uint8_t fill_ch, uint8_t fill_attr);

/* Region variants, for text windows: the same operations bounded to a
 * rectangle of the grid instead of the whole panel. The full-screen calls
 * above remain, because the root window is the whole panel and doing it in
 * one memmove is cheaper than doing it a row at a time. */
void vidterm_clear_region(int col, int row, int cols, int rows,
                          uint8_t ch, uint8_t attr);

/* Move one row of cells between the live grid and a parked window's backing
 * store, characters and attributes interleaved. */
/* Bound the GUEST's drawing primitives to a rectangle. Text, cursor and
 * pointer are unaffected -- they are the firmware's own and stay screen-wide.
 * There is one graphics plane shared by every window, so this bounds where a
 * primitive lands, not which surface it lands on. */
void vidterm_set_clip(int x, int y, int w, int h);

/* Where the guest's 0,0 sits, in panel pixels. Separate from the clip because
 * a window whose protection is off still measures from its own corner while
 * its pixels are allowed outside it. */
void vidterm_set_origin(int x, int y);

/* Text cells blitted: cumulative, and the worst single refresh. A
 * whole-screen repaint is 3840 and nothing else approaches it, so this says
 * whether a slow render call was the text layer or a drawing primitive --
 * which the render timer alone cannot distinguish. */
/* SRAM crossbar arbitration: true = scan-out DMA outranks the cores (the
 * bring-up default), false = equal. A runtime toggle because it is an
 * experiment on a real display, and putting it back must not need a reflash. */
void video_set_dma_priority(bool high);
bool video_get_dma_priority(void);

void vidterm_blit_stats(uint32_t *total, uint32_t *worst);
void vidterm_blit_reset(void);

void vidterm_read_row(int col, int row, int cols, uint8_t *dst);
void vidterm_write_row(int col, int row, int cols, const uint8_t *src);
void vidterm_scroll_region(int col, int row, int cols, int rows,
                           uint8_t fill_ch, uint8_t fill_attr); /* text grid up one row */

/* Text cursor. A blinking block, drawn by inverting the cell it sits on, so
 * the character underneath stays readable. It is composited during scan-out
 * and never written into the framebuffer, so moving it cannot disturb text or
 * graphics and it needs no save-under. */
void vidterm_cursor_set(int col, int row);
void vidterm_cursor_enable(bool on);
/* Mouse pointer. An arrow sprite with a transparency mask, drawn by the same
 * save-under machinery as the cursor. Position is in PIXELS; the firmware
 * tracks it from USB so it stays smooth whatever the guest is doing, and the
 * guest can place it too (the PutGC graphics command). */
void vidterm_pointer_set(int x, int y);
void vidterm_pointer_enable(bool on);

/* Lift the cursor out of the framebuffer before anything else draws. */
void vidterm_cursor_hide(void);
/* Draw, move or blink it as required; call from the service loop, never from
 * an interrupt. Cheap when nothing has changed. */
void vidterm_cursor_service(void);
void vidterm_present(void);   /* wait for vsync */

/* ── Graphics layer (RGB332) ────────────────────────────────────────────── */
void vidterm_plot(int x, int y, uint8_t colour);
void vidterm_line(int x0, int y0, int x1, int y1, uint8_t colour);
void vidterm_fill_rect(int x, int y, int w, int h, uint8_t colour);
void vidterm_hline(int x, int y, int len, uint8_t colour);
void vidterm_vline(int x, int y, int len, uint8_t colour);
void vidterm_circle(int cx, int cy, int radius, uint8_t colour);
void vidterm_ellipse(int cx, int cy, int rx, int ry, uint8_t colour);
void vidterm_fill_ellipse(int cx, int cy, int rx, int ry, uint8_t colour);

/* Flood fill from a seed, bounded by pixels differing from the seed's own
 * colour and by the clip rectangle. The seed stack is finite; see the note in
 * video_render.cpp for what happens when it runs out, and vidterm_fill_stats()
 * for whether it ever has. */
/* Raster mix mode for the GUEST's drawing: 0 replace, 1 AND, 2 OR, 3 XOR.
 * Combines against the graphics plane rather than the framebuffer, so an
 * operation means the same whatever text is on screen. Text is not affected
 * by it. */
void vidterm_set_logic(uint8_t mode);

void vidterm_flood_fill(int x, int y, uint8_t colour);
void vidterm_fill_stats(uint32_t *overflows);
void vidterm_clear_graphics(uint8_t colour);

#ifdef __cplusplus
}
#endif

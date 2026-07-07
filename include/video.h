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
void vidterm_clear(uint8_t ch, uint8_t attr);
void vidterm_refresh(void);   /* blit dirty text cells into the framebuffer */
void vidterm_present(void);   /* wait for vsync */

/* ── Graphics layer (RGB332) ────────────────────────────────────────────── */
void vidterm_plot(int x, int y, uint8_t colour);
void vidterm_fill_rect(int x, int y, int w, int h, uint8_t colour);
void vidterm_hline(int x, int y, int len, uint8_t colour);
void vidterm_vline(int x, int y, int len, uint8_t colour);
void vidterm_circle(int cx, int cy, int radius, uint8_t colour);
void vidterm_clear_graphics(uint8_t colour);

#ifdef __cplusplus
}
#endif

/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal seam between the PORTABLE renderer (video_render.cpp — pure
 * memory operations on the framebuffer and text grid, no pico headers) and
 * its consumers: the hardware scan-out (video.cpp, HSTX + ping-pong DMA) on
 * the firmware, or a host-side display (e.g. a USim video window) in a
 * simulator build. Public drawing API stays in video.h.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "video.h"	/* FB_WIDTH/FB_HEIGHT and the vidterm_* API */

#ifdef __cplusplus
extern "C" {
#endif

/* The RGB332 framebuffer the renderer draws into and the scan-out reads.
 * Owned by video_render.cpp. */
extern uint8_t video_framebuffer[FB_HEIGHT][FB_WIDTH];

/* Clear the framebuffer and text grid to power-on state. */
void video_render_init(void);

/* ── Cursor ──────────────────────────────────────────────────────────────
 * The cursor is drawn INTO the framebuffer, keeping a copy of the pixels it
 * covers so they can be put back when it moves or blinks off.
 *
 * It deliberately does NOT composite during scan-out. The scanline interrupt
 * has only the duration of the in-flight DMA transfer to re-arm the finished
 * channel; copying a line there overruns that budget and the display loses
 * sync entirely. Everything here therefore runs from the service loop, on the
 * same core as glyph rendering, which also means the two cannot interleave.
 *
 * Ordering rule: the cursor must be hidden before anything else draws, or the
 * saved pixels go stale and putting them back would paint over new content.
 * vidterm_cursor_hide() is called at the start of each burst of console work;
 * vidterm_cursor_service() puts it back afterwards.
 */
extern int               video_cursor_col;
extern int               video_cursor_row;
extern bool              video_cursor_enabled;   /* a console owns the screen */
extern volatile uint32_t video_frames;           /* blink timebase, one per frame */

#define VIDEO_CURSOR_BLINK_FRAMES 30u            /* ~0.5s each way at 60Hz */

#ifdef __cplusplus
}
#endif

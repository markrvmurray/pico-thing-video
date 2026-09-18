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

#include "video.h"	/* FB_WIDTH/FB_HEIGHT and the vidterm_* API */

#ifdef __cplusplus
extern "C" {
#endif

/* The RGB332 framebuffer the renderer draws into and the scan-out reads.
 * Owned by video_render.cpp. */
extern uint8_t video_framebuffer[FB_HEIGHT][FB_WIDTH];

/* Clear the framebuffer and text grid to power-on state. */
void video_render_init(void);

#ifdef __cplusplus
}
#endif

/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Storage for the GRAPHICS SHADOW: what the framebuffer would hold if no text
 * had ever been drawn on it.
 *
 * It exists so text can have a transparent background. Transparent means the
 * blitter does not paint a cell's background -- but then nothing ever erases,
 * and an overwritten glyph leaves its pixels behind. To erase transparently
 * the renderer has to know what was underneath, and the only way to know is
 * to have kept it.
 *
 * A second full plane does not fit in SRAM: the framebuffer already takes
 * 480 KB of 520. It lives in PSRAM, where 8 MB makes the size irrelevant --
 * but PSRAM is roughly 53 MB/s, so a whole-plane write costs about 9 ms and
 * cannot be done casually. video_render.cpp therefore keeps most of the plane
 * IMPLICIT (see the uniform state there) and touches this buffer only for the
 * rows that genuinely differ from a flat colour.
 *
 * An interface rather than an array because the firmware's copy is behind the
 * QMI and cannot compile on a host, exactly as the parked window screens are.
 * If the storage is unavailable the renderer keeps its opaque behaviour --
 * text hides graphics, as it always did, rather than ghosting.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "video.h"	/* FB_WIDTH, FB_HEIGHT */

#define GFX_SHADOW_BYTES	((size_t)FB_WIDTH * (size_t)FB_HEIGHT)

/* The plane, or NULL when there is none. One row is FB_WIDTH bytes, laid out
 * exactly like the framebuffer so a row copies straight across. */
uint8_t *gfx_shadow_base(void);

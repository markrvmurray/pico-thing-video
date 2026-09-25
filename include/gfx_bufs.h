/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Storage for the guest's GET/PUT BUFFERS: off-screen bitmaps it defines,
 * loads, copies rectangles into and out of, and uses as fill patterns and
 * fonts.
 *
 * They live in PSRAM for the same reason the parked windows and the graphics
 * shadow do -- SRAM has none to spare -- and it suits them: a buffer is
 * touched when the guest asks, never per frame. The pool is generous because
 * PSRAM is: a font is 4 KB, a sprite a few hundred bytes, and a screen-sized
 * block under half a megabyte, against megabytes free.
 *
 * An interface rather than an array because the firmware's copy is behind the
 * QMI and cannot compile on a host. With no storage the guest's buffer
 * commands are accepted and do nothing, which is how every unimplemented
 * sub-code already behaves: unanswered rather than corrupting.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Two megabytes. Not a measured need -- no guest asks for buffers yet -- but
 * a bound, because an allocator without one eventually meets a guest without
 * one. Raising it costs nothing but the line. */
#define GFX_BUFPOOL_BYTES	(2u * 1024u * 1024u)

/* The pool, or NULL when there is none. */
uint8_t *gfx_bufpool_base(void);

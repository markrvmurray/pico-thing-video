/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Backing store for INACTIVE text windows.
 *
 * The split this exists to serve: there is ONE graphics plane and ONE live
 * text layer, both in SRAM, and neither is per-window. Graphics is global and
 * persists across every window switch; only the TEXT layer multiplexes. So a
 * window is a text screen that swaps into the single live layer, and this is
 * where the ones that are not currently on screen are parked.
 *
 * It is an interface rather than an array because the storage underneath it
 * is a hardware question that has already changed once. SRAM cannot hold more
 * than one spare screen -- the framebuffer takes 480 KB of the 520 KB and the
 * measured free space below the stacks is 9,588 bytes against a 7,680-byte
 * screen -- so the windows live in PSRAM, and the window model above must not
 * care. If PSRAM is ever absent this degrades to no spare screens rather than
 * to corruption.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "video.h"	/* TEXT_COLS, TEXT_ROWS */

/* One parked screen: a character cell and an attribute cell per position.
 * Sized to the whole panel because a switched shell is full-screen; a smaller
 * window simply leaves the tail of its slot unused, which costs PSRAM we have
 * in abundance and saves an allocator we would otherwise have to trust. */
#define WIN_STORE_SLOT_BYTES	((unsigned)(TEXT_COLS * TEXT_ROWS) * 2u)

/* Probe the store and report what is usable. Returns false when no parked
 * screen is available, which is not a failure to handle but a capability to
 * report: the root window still works and behaves exactly as it does today. */
bool win_store_init(void);

/* How many parked screens are actually usable, PROVEN by the probe rather
 * than claimed by a board header. Zero is a valid answer. */
unsigned win_store_slots(void);

/* Slot i, or NULL if i is beyond what the probe proved. The pointer is into
 * the UNCACHED alias: the XIP cache is not write-through for PSRAM and the
 * per-core caches are not coherent, so a cached write from one core is not
 * reliably visible to the other. */
uint8_t *win_store_slot(unsigned i);

void win_store_print_status(void);

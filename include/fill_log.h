/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Storage for the FLOOD-FILL SPAN LOG: the runs a patterned or mix-mode FFill
 * has claimed, so they can be painted properly afterwards.
 *
 * A scanline flood fill finds its way by colour -- a pixel equal to the target
 * has not been filled yet. A pattern or a mix mode breaks that: a pixel it has
 * just filled can still equal the target (white AND a black pattern pixel, on
 * black), so it re-seeds its own work, overflows its seed stack and quits with
 * most of the region untouched (VID-034). So such a fill runs twice: first
 * SOLID, which is guaranteed to terminate, logging every span it claims; then
 * over exactly those spans with the pattern and mix mode applied.
 *
 * In PSRAM because a region's span count can reach hundreds of thousands in
 * the worst case and SRAM has about a kilobyte spare. One word per span.
 * Bounded like the seed stack: past the end, spans are still filled solidly
 * but not patterned, and the overflow is COUNTED rather than silent.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define FILL_LOG_SPANS	65536u
#define FILL_LOG_BYTES	(FILL_LOG_SPANS * 4u)

/* The log, or NULL when there is no storage. */
uint32_t *fill_log_base(void);

/* THE SEED STACK, beside the log and for the same reason. A scanline fill
 * stacks work waiting to be done, and how much depends on the picture, not
 * on anything the fill controls: a busy region -- text, a map, a dithered
 * image -- keeps many runs pending at once. It lived in SRAM, bounded at 96
 * entries because SRAM is full, and a busy but ordinary region overflowed it
 * by hundreds, dropping the seeds and leaving holes (VID-037).
 *
 * One word an entry. 262,144 of them is more than the RUNS a 1024x480 panel
 * can hold (512 a row at most, 245,760 in all), so overflowing it needs the
 * same run to be stacked many times over -- not a picture anyone draws. It
 * is still bounded, and still counted, because a bound nobody can reach is
 * no reason to write one that can overrun. 1 MB of the 8 MB part. */
#define FILL_SEEDS		262144u
#define FILL_SEEDS_BYTES	(FILL_SEEDS * 4u)

/* The stack, or NULL when there is no storage. */
uint32_t *fill_seeds_base(void);

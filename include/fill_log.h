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

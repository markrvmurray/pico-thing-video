/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Storage for the COMMAND TRACE: the last bytes of the guest's command stream
 * as the renderer consumed them, each with its role in the framing.
 *
 * It exists because a wrong picture has three causes that look identical on
 * the panel -- the guest asked for it, a byte was lost so a coordinate was
 * read from the wrong place, or the renderer drew it wrong -- and nothing on
 * this side could tell them apart. One wgfx page cost three diagnostic rounds
 * and two wrong firmware diagnoses before the NitrOS-9 side settled it by
 * re-running the program in the simulator and analysing pixels. The byte
 * stream answers that question directly, without needing the guest's source.
 *
 * In PSRAM because SRAM has about a kilobyte to spare (VID-029) and a useful
 * depth is tens of kilobytes: a whole wgfx run, not one scene. The cost is one
 * uncached two-byte write per consumed byte, ~50 ns against a renderer that
 * spends microseconds per byte. With no PSRAM the trace is simply absent and
 * says so; the link works exactly as before.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Entries, a power of two. Each is two bytes: the command byte, then its
 * class (VID_BYTE_* in vid_link.h). 64 KB of an 8 MB part. */
#define CMD_TRACE_ENTRIES	32768u
#define CMD_TRACE_BYTES		(CMD_TRACE_ENTRIES * 2u)

/* The ring, or NULL when there is no storage. */
uint8_t *cmd_trace_base(void);

/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Time the PSRAM access patterns the renderer uses, and verify the data.
 *
 * Two jobs. It measures whether a byte costs its QSPI preamble (it does --
 * that is why block blits copy rows rather than pixels), and it is the only
 * check that PSRAM is FUNCTIONING at the current clock rather than merely
 * reporting the right size. That distinction has bitten once: timing derived
 * before main() raised clk_sys left the part driven at 160 MHz against a
 * 133 MHz maximum, while detection in the same hook reported 8 MB correctly.
 *
 * DESTRUCTIVE: it writes over the graphics shadow. That is deliberate -- a
 * scratch region big enough to be worth timing does not exist anywhere else,
 * and the shadow's contents are reconstructible by redrawing. It says so on
 * the console when it finishes.
 */

#pragma once

/* Time and verify; prints its own report. Safe to call with no PSRAM (says
 * so and returns). */
void psram_bench(void);

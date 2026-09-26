/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ON-HARDWARE SELF-TESTS: the checks the simulator cannot make.
 *
 * usim compiles the portable renderer bit-identically and tests it hard, but
 * it has no PSRAM, no QMI, no clock tree and no wall clock that means
 * anything. So an entire class of fault is invisible to it: a part that is
 * configured and inert, addressing that aliases, timing derived against a
 * clock that later moved, and every performance regression. Those have to be
 * tested where they live.
 *
 * Operator-invoked, never automatic. They are destructive (PSRAM is the only
 * scratch region big enough to be worth testing, and it holds the graphics
 * shadow, the guest's buffers and the parked windows) and some take a
 * noticeable fraction of a second, so nothing here may run at boot.
 *
 * COMPILED IN BY DEFAULT, including Release. They are flash-resident and cost
 * no SRAM -- they scratch in PSRAM and the framebuffer, which matters because
 * the device-role build has about 648 bytes of SRAM left. Making them
 * debug-only was considered and rejected: the images that get flashed are
 * Release/ATfE, so a debug-only self-test would live exclusively in a binary
 * nobody runs. Configure -DSELFTEST=OFF to drop them.
 *
 * TIMING vs CORRECTNESS. Correctness results mean the same thing at any
 * optimisation level. Timing results do not: a -O0 build measures a binary
 * that will never be flashed. So timings are always REPORTED and only
 * ASSERTED against a threshold when __OPTIMIZE__ says the figure is
 * comparable, and the report says which happened. A pass that was never
 * really checked is worse than no test.
 */

#pragma once

/* Run every group; prints its own report and a final verdict. */
void selftest_run(void);

/* bus_engine.h -- the MC6809E bus peripheral: PIO/DMA, the bus ISRs, Core 1.
 *
 * The hard-real-time half. Core 1 owes the video scan-out a DMA re-arm inside
 * a ~5 us blanking interval while serving bus interrupts, so nothing unbounded
 * may be added behind this interface.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef BUS_ENGINE_H
#define BUS_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the host-facing discrete signals, the bus PIO/DMA engine and a
 * cleared register window, then launch Core 1. Not idempotent -- it claims DMA
 * channels and loads PIO programs -- which is why a power cycle reboots rather
 * than trying to stop and restart the engine in place. */
void bus_engine_start(void);

/* True once Core 1 has finished bring-up and is serving. */
bool bus_engine_serving(void);

/* Core 1's loop counter; boot waits on it to confirm the drain loop runs. */
uint32_t bus_engine_loop_count(void);

/* Push a {location, value} write into the ring, so a console modify reaches
 * Core 1's dispatch exactly as a guest bus write would. */
void bus_engine_inject_write(uint8_t loc, uint8_t val);

/* Core 1 counters and the per-offset guest-read tally, for the status line.
 * The counters stay private: the console gets a report, not the variables. */
void bus_engine_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* BUS_ENGINE_H */

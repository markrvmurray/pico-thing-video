/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Main-power sensing. The board's main 5V supply reaches ADC6 (GPIO 46)
 * through a divide-by-2 resistor pair, so the rail is sampled directly and
 * the peripheral can follow the host system's power state automatically:
 * come alive when the 5V rail appears, quiesce (bus + video; the console
 * stays up) when it disappears. There is no power *control* here — unlike
 * the main pico, this board only observes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Claim ADC6 and seed the debounced state with an immediate burst of
 * samples, so power_is_on() is valid as soon as this returns. */
void power_init(void);

/* Rate-limited sample + debounce; call freely from the Core 0 loop (it
 * no-ops until the next sample is due). */
void power_task(void);

/* Debounced state of the main 5V rail. */
bool power_is_on(void);

/* Most recent 5V-rail sample, in millivolts (divider already undone). */
uint32_t power_millivolts(void);

/* Most recent die-temperature sample: raw ADC counts, and tenths of a degree
 * Celsius. Sampled on the same pass as the rail, from the internal sensor.
 *
 * TREAT ABSOLUTE VALUES WITH SUSPICION until the board is calibrated. Both
 * channels are measured against ADC_AVDD, which the code assumes is nominal;
 * the die figure amplifies any error in that assumption several-fold. The
 * main pico also found both channels shift while it serves its bus -- the
 * rail high, the die reading far too cold -- which has not yet been
 * characterised on this board. */
uint16_t power_die_raw(void);
int32_t  power_die_decicelsius(void);

#ifdef __cplusplus
}
#endif

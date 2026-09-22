/* adc_convert.h -- raw ADC counts to volts and degrees.
 *
 * PORTABLE: pure integer arithmetic, no pico headers, so the host test tree
 * compiles it directly. It lives apart from power.cpp because the supply
 * conversion gates whether this board serves the bus and video at all, and a
 * wrong answer here does not look like a fault -- it looks like a flat supply.
 * That is worth a regression test, and a test needs the arithmetic reachable
 * without the SDK.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ADC_CONVERT_H
#define ADC_CONVERT_H

#include <stdint.h>

/* ---- Per-board calibration ----------------------------------------------
 *
 * Both readings are measured against ADC_AVDD -- nominally 3.3 V, in practice
 * whatever this board delivers. NOMINAL VALUES: measure and correct them
 * before believing an absolute figure.
 *
 * The rail reading scales with the reference, so an error there is a
 * proportional error. The DIE reading does not: it is a small difference
 * between two close voltages divided by a 1.721 mV/degree slope, so a
 * reference error is hugely amplified -- 1.6% low shifts the die temperature
 * about 7 degrees. Reading high on the rail and cold on the die TOGETHER is
 * the signature of a reference left at its nominal value. (The main pico
 * measured 3.2464 V on its own board.)
 *
 * The divider ratio is per-board too: two main-pico boards measured 1.9407
 * and 2.0001 against a nominal 2. */
/* MEASURED on this board, 2026-09-22, against a metered 5.0000 V rail and
 * 2.4983 V at the divider midpoint, with clk_adc at its correct 48 MHz.
 *
 * ADC_AVDD was not probed directly -- it is derived, which needs no test
 * point: V(pin) * 4095 / counts. It comes out 13 mV below the 3V3 rail
 * (metered 3.3165 V), which is what a filter feeding ADC_AVDD would do.
 *
 * Calibrated, the rail reads 4.999 V against a metered 5.0000. Uncalibrated it
 * read 4.992 V, so the correction is worth 0.14% -- small, and worth having
 * chased mainly because the hunt found a real fault. */
#define ADC_VREF_MV	3303u	/* derived: 2.4983 V at 3097 counts */
#define DIVIDER_X1000	2001u	/* measured: 5.0000 / 2.4983 */
#define ADC_FULL_SCALE	4095u	/* 12-bit conversion */

#ifdef __cplusplus
extern "C" {
#endif

/* Supply rail in millivolts, with the divider undone.
 *
 * The 64-bit intermediate is NOT optional: the numerator is raw * 6,600,000
 * with the constants above, which passes 2^32 at about 650 counts -- a fifth
 * of a healthy rail. In 32 bits it wraps to a small, plausible-looking number
 * rather than anything obviously wrong, and because this reading gates
 * serving, the symptom is a dead peripheral whose console insists the supply
 * is absent. */
static inline uint32_t
adc_rail_mv(uint16_t raw)
{
	return (uint32_t)(((uint64_t)raw * DIVIDER_X1000 * ADC_VREF_MV) /
	    (1000u * (uint64_t)ADC_FULL_SCALE));
}

/* Die temperature in TENTHS of a degree Celsius.
 *
 * Vbe is 0.706 V at 27 C with a slope of -1.721 mV per degree -- the RP2350's
 * own figures (datasheet 12.4.6), not carried over from the RP2040. Integer
 * arithmetic because nothing else in this firmware needs float printf. */
static inline int32_t
adc_die_decicelsius(uint16_t raw)
{
	const int32_t uv = (int32_t)(((uint64_t)raw * (uint64_t)ADC_VREF_MV *
	    1000u) / ADC_FULL_SCALE);
	return 270 - ((uv - 706000) * 10) / 1721;
}

#ifdef __cplusplus
}
#endif

#endif /* ADC_CONVERT_H */

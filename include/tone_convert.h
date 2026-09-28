/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest's Tone command ($1B $27) carries CoCo 3 SS.Tone values unchanged,
 * so a window driver can forward the SetStat as it stands. These turn them
 * into what the PWM stage takes. Pure integer arithmetic in a header, so the
 * host tests check them (tests/test_tone_convert.cpp).
 */

#pragma once

#include <stdint.h>

/* PITCH, 1-4095, higher is higher -- CoCo 3 relative pitch, not Hz. Taken
 * from the CoCo 3 sound driver (level2/coco3/modules/snddrv_cc3.asm): it
 * loads Y = 4096 - pitch and waits Y passes of an 8-cycle loop per half
 * period, at NitrOS-9 Level 2's 1.7897725 MHz, so
 *
 *   f = 1789772.5 / (16 * (4096 - pitch))  =  3579545 / (32 * (4096 - pitch))
 *
 * rounded to the nearest Hz. Loop overhead is ignored, so this is within a
 * few percent of a real CoCo 3. Pitch 4000 is the CoCo 3 bell, 1165 Hz.
 * Outside 1-4095 the answer is 0: no tone (the CoCo 3 driver rejects those).
 * The top of the range is far above hearing -- pitch 4095 is 111,861 Hz --
 * and the PWM stage turns anything above 20 kHz into silence. */
static inline uint32_t
tone_pitch_to_hz(uint32_t pitch)
{
	if (pitch < 1u || pitch > 4095u)
		return 0u;
	const uint32_t n = 4096u - pitch;

	return (3579545u + 16u * n) / (32u * n);
}

/* DURATION in 1/60 s ticks, 0-255, as SS.Tone counts it; 0 means stop now. */
static inline uint32_t
tone_ticks_to_ms(uint32_t ticks)
{
	return (ticks * 1000u + 30u) / 60u;
}

/* VOLUME, 0-63 (SS.Tone keeps six bits), to the PWM stage's percent. Only
 * monotonic matters: the CoCo drives a DAC's amplitude, the Pico a PWM duty,
 * and neither's exact loudness is part of the contract. */
static inline uint32_t
tone_volume_to_pct(uint32_t volume)
{
	if (volume > 63u)
		volume = 63u;
	return (volume * 100u + 31u) / 63u;
}

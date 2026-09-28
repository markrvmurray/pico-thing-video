/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for the Tone conversions (include/tone_convert.h): CoCo 3
 * SS.Tone pitch, duration and volume to what the PWM stage takes.
 */

#include <catch2/catch_test_macros.hpp>

#include "tone_convert.h"

TEST_CASE("tone: pitch matches the CoCo 3 sound driver's timing", "[tone]")
{
	// f = 1789772.5 / (16 * (4096 - pitch)), from snddrv_cc3.asm.
	CHECK(tone_pitch_to_hz(4000) == 1165u);		// the CoCo 3 bell
	CHECK(tone_pitch_to_hz(1) == 27u);		// the bottom
	CHECK(tone_pitch_to_hz(4095) == 111861u);	// the top, ultrasonic
	CHECK(tone_pitch_to_hz(2048) == 55u);		// 111861 / 2048 = 54.6
	CHECK(tone_pitch_to_hz(3584) == 218u);		// 111861 / 512 = 218.48
}

TEST_CASE("tone: pitch rises with the value, strictly where hertz can", "[tone]")
{
	// "Higher is higher" is the one property a tune depends on. Integer
	// hertz flatten the bottom of the range, where hundreds of neighbouring
	// pitches lie within 1 Hz of each other, so it is non-decreasing
	// everywhere, and strictly increasing from pitch 3800 up, where every
	// step is more than a hertz (111861/296 - 111861/297 = 1.27 Hz).
	uint32_t prev = tone_pitch_to_hz(1);
	for (uint32_t p = 2; p <= 4095; p++) {
		const uint32_t f = tone_pitch_to_hz(p);
		REQUIRE(f >= prev);
		if (p > 3800)
			REQUIRE(f > prev);
		prev = f;
	}
}

TEST_CASE("tone: pitch outside 1-4095 is no tone", "[tone]")
{
	CHECK(tone_pitch_to_hz(0) == 0u);
	CHECK(tone_pitch_to_hz(4096) == 0u);
	CHECK(tone_pitch_to_hz(65535) == 0u);
}

TEST_CASE("tone: duration is 1/60 s ticks", "[tone]")
{
	CHECK(tone_ticks_to_ms(0) == 0u);
	CHECK(tone_ticks_to_ms(1) == 17u);
	CHECK(tone_ticks_to_ms(60) == 1000u);
	CHECK(tone_ticks_to_ms(255) == 4250u);
}

TEST_CASE("tone: volume is monotonic, 0 silent, 63 loudest", "[tone]")
{
	CHECK(tone_volume_to_pct(0) == 0u);
	CHECK(tone_volume_to_pct(63) == 100u);
	CHECK(tone_volume_to_pct(1) > 0u);		// quiet is not silent
	CHECK(tone_volume_to_pct(255) == 100u);		// clamped
	for (uint32_t v = 1; v <= 63; v++)
		REQUIRE(tone_volume_to_pct(v) >= tone_volume_to_pct(v - 1));
}

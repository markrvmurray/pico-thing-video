/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for the ADC conversions (include/adc_convert.h).
 *
 * The supply conversion decides whether this board serves the bus and video
 * at all, and it failed once in a way that looked nothing like a fault: a
 * 32-bit overflow turned a healthy 5 V rail into 0.80 V, the power-follow
 * concluded the supply was absent, and the peripheral sat in standby with no
 * video and a console calmly reporting a flat rail. These cases exist so that
 * cannot recur silently.
 */

#include <catch2/catch_test_macros.hpp>

#include "adc_convert.h"

TEST_CASE("rail: a healthy 5V supply reads as such", "[adc]")
{
	// 2.5 V at the pin through a divide-by-two, against a 3.3 V reference:
	// 2.5/3.3 * 4095 = 3102 counts.
	CHECK(adc_rail_mv(3102) > 4900u);
	CHECK(adc_rail_mv(3102) < 5100u);
}

TEST_CASE("rail: no overflow anywhere in the input range", "[adc]")
{
	// THE REGRESSION. raw * 6,600,000 passes 2^32 at about 650 counts, so
	// a 32-bit intermediate wraps for four fifths of the usable range --
	// and wraps to small, plausible values rather than to nonsense.
	// Monotonic across every possible count is the property that catches
	// it: a wrap always breaks the ordering.
	uint32_t prev = adc_rail_mv(0);
	for (uint32_t raw = 1; raw <= ADC_FULL_SCALE; raw++) {
		const uint32_t mv = adc_rail_mv((uint16_t)raw);
		CHECK(mv >= prev);
		prev = mv;
	}
	// Full scale is the reference seen through the divider. DERIVED from
	// the constants, not written out: these are per-board calibration
	// figures and a test that hard-codes today's values fails the next
	// time someone measures the board, which teaches people to edit tests
	// rather than to believe them.
	CHECK(adc_rail_mv((uint16_t)ADC_FULL_SCALE) ==
	    (ADC_VREF_MV * DIVIDER_X1000) / 1000u);
	CHECK(adc_rail_mv(0) == 0u);
}

TEST_CASE("rail: the thresholds the power-follow trips on", "[adc]")
{
	// 4.0 V on and 3.5 V off, as power.cpp uses them. Find the count each
	// threshold falls on rather than asserting one, so this keeps testing
	// the same property after the board is recalibrated: one count below
	// must read under the threshold and one count above must not.
	auto boundary = [](uint32_t want_mv) {
		for (uint32_t raw = 0; raw <= ADC_FULL_SCALE; raw++)
			if (adc_rail_mv((uint16_t)raw) >= want_mv)
				return raw;
		return (uint32_t)ADC_FULL_SCALE + 1u;
	};

	for (const uint32_t mv : { 4000u, 3500u }) {
		const uint32_t edge = boundary(mv);
		REQUIRE(edge <= ADC_FULL_SCALE);        // reachable at all
		CHECK(adc_rail_mv((uint16_t)edge) >= mv);
		REQUIRE(edge > 0u);
		CHECK(adc_rail_mv((uint16_t)(edge - 1u)) < mv);
	}

	// And the thresholds sit where a 5 V system expects them: comfortably
	// inside the range, not up against either end.
	CHECK(boundary(4000u) > 100u);
	CHECK(boundary(4000u) < ADC_FULL_SCALE - 100u);
}

TEST_CASE("die: room temperature is plausible", "[adc]")
{
	// 27 C is by definition the count where Vbe reads 0.706 V, whatever
	// the reference happens to be calibrated to.
	const uint16_t at27 = (uint16_t)((706000ull * ADC_FULL_SCALE) /
	    ((uint64_t)ADC_VREF_MV * 1000u));
	CHECK(adc_die_decicelsius(at27) > 260);
	CHECK(adc_die_decicelsius(at27) < 280);
}

TEST_CASE("die: hotter die means fewer counts", "[adc]")
{
	// Vbe falls as the die warms, so the sense is inverted. Getting this
	// backwards would report cooling under load, which is the one error
	// that would actively mislead a thermal soak.
	CHECK(adc_die_decicelsius(800) > adc_die_decicelsius(900));
	CHECK(adc_die_decicelsius(859) > 300);   // the reading seen on hardware
	CHECK(adc_die_decicelsius(859) < 400);
}

TEST_CASE("die: the slope matches the datasheet figure", "[adc]")
{
	// 1.721 mV per degree: one degree is 1721 uV, which at 3.3 V over 4095
	// counts is about 2.136 counts. Ten degrees should be ~21 counts.
	const uint16_t at27 = (uint16_t)((706000ull * ADC_FULL_SCALE) /
	    ((uint64_t)ADC_VREF_MV * 1000u));
	const int32_t a = adc_die_decicelsius(at27);
	const int32_t b = adc_die_decicelsius((uint16_t)(at27 - 21));
	CHECK(b - a > 90);       // ~10.0 degrees, in tenths
	CHECK(b - a < 110);
}

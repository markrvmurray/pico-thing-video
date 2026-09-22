/* power.cpp – FreeBSD style */

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "adc_convert.h"
#include "pico_thing_video.h"
#include "power.h"

/* The main 5V rail reaches ADC6 through a divide-by-2 resistor pair, so a
 * healthy rail reads ~2.5 V against the 3.3 V ADC reference. Thresholds are
 * on the reconstructed rail voltage, with hysteresis and a consecutive-sample
 * debounce so supply sag or ADC noise cannot flap the peripheral. */
#define POWER_ADC_CHANNEL   6u
#define DIE_ADC_CHANNEL     (NUM_ADC_CHANNELS - 1u)	/* internal, above the pins */

#define POWER_SAMPLE_US     10000u	/* one sample every 10 ms */
#define POWER_DEBOUNCE      5u		/* consecutive samples to change state */
#define POWER_ON_MV         4000u	/* rail above this counts towards on */
#define POWER_OFF_MV        3500u	/* rail below this counts towards off */

static bool     _on;
static uint32_t _mv;
static uint16_t _die_raw;
static uint32_t _streak;
static uint64_t _last_sample_us;

/* One conversion on a chosen channel. The first result after a mux change is
 * DISCARDED: the sample capacitor has to charge to the new channel through
 * whatever impedance drives it, and the second conversion then starts from a
 * settled cap. This matters most for the die sensor, which is a biased diode
 * with a far higher source impedance than a resistive divider, and which here
 * follows a channel sitting three times higher. */
static uint16_t
read_channel(uint8_t ch)
{
	adc_select_input(ch);
	(void)adc_read();
	return (uint16_t)adc_read();
}

/* Sample both channels and cache them. Nothing else may touch the ADC mux. */
static void
sample(void)
{
	/* Conversions (and their calibration) live in adc_convert.h. */
	const uint32_t raw = read_channel(POWER_ADC_CHANNEL);
	_mv = adc_rail_mv((uint16_t)raw);
	_die_raw = read_channel(DIE_ADC_CHANNEL);
}

/* One debounce step: count consecutive samples on the far side of the
 * hysteresis band and flip the state when enough agree. */
static void
debounce(void)
{
	sample();
	const bool crossing = _on ? (_mv <= POWER_OFF_MV) : (_mv >= POWER_ON_MV);
	if (crossing) {
		if (++_streak >= POWER_DEBOUNCE) {
			_on = !_on;
			_streak = 0u;
		}
	} else {
		_streak = 0u;
	}
}

extern "C" void
power_init(void)
{
	adc_init();
	adc_gpio_init(GPIO_PWR_SENSE);
	adc_set_temp_sensor_enabled(true);

	/* Seed the state: assume off, then run a full debounce burst so a
	 * live rail is recognised before the caller first asks. */
	_on = false;
	_streak = 0u;
	for (uint32_t i = 0u; i <= POWER_DEBOUNCE; i++)
		debounce();
	_last_sample_us = time_us_64();
}

extern "C" void
power_task(void)
{
	const uint64_t now = time_us_64();
	if (now - _last_sample_us < POWER_SAMPLE_US)
		return;
	_last_sample_us = now;
	debounce();
}

extern "C" bool
power_is_on(void)
{
	return _on;
}

extern "C" uint32_t
power_millivolts(void)
{
	return _mv;
}

extern "C" uint16_t
power_die_raw(void)
{
	return _die_raw;
}

/* Tenths of a degree; see adc_convert.h for the conversion and its caveats. */
extern "C" int32_t
power_die_decicelsius(void)
{
	return adc_die_decicelsius(_die_raw);
}

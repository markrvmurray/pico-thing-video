/* power.cpp – FreeBSD style */

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "pico_thing_video.h"
#include "power.h"

/* The main 5V rail reaches ADC6 through a divide-by-2 resistor pair, so a
 * healthy rail reads ~2.5 V against the 3.3 V ADC reference. Thresholds are
 * on the reconstructed rail voltage, with hysteresis and a consecutive-sample
 * debounce so supply sag or ADC noise cannot flap the peripheral. */
#define POWER_ADC_CHANNEL   6u
#define POWER_SAMPLE_US     10000u	/* one sample every 10 ms */
#define POWER_DEBOUNCE      5u		/* consecutive samples to change state */
#define POWER_ON_MV         4000u	/* rail above this counts towards on */
#define POWER_OFF_MV        3500u	/* rail below this counts towards off */

static bool     _on;
static uint32_t _mv;
static uint32_t _streak;
static uint64_t _last_sample_us;

static uint32_t
sample_mv(void)
{
	/* 12-bit result against the 3.3 V reference; undo the divide-by-2. */
	const uint32_t raw = adc_read();
	return (raw * 2u * 3300u) / 4095u;
}

/* One debounce step: count consecutive samples on the far side of the
 * hysteresis band and flip the state when enough agree. */
static void
debounce(void)
{
	_mv = sample_mv();
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
	adc_select_input(POWER_ADC_CHANNEL);

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

/* sound.cpp -- FreeBSD style
 *
 * One PWM tone on GPIO_AUDIO. See sound.h.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "pico_thing_video.h"
#include "sound.h"
#include "vid_link.h"

/* The bell: short, mid-pitched, loud -- the amplifier has its own volume
 * knob, so the firmware gives it the cleanest signal it can. */
#define BELL_HZ		1000u
#define BELL_MS		120u
#define BELL_VOLUME	100u

static uint     slice, channel;
static bool     playing;
static uint32_t started_us, length_us;
static uint32_t bells;			/* for status: the guest's $07s */

void
sound_init(void)
{
	slice = pwm_gpio_to_slice_num(GPIO_AUDIO);
	channel = pwm_gpio_to_channel(GPIO_AUDIO);
	/* Idle is a steady LOW: level 0 holds the pin down whatever the slice
	 * is doing, so it can run all the time and a tone is only a change of
	 * wrap and level -- no stopping mid-cycle with the pin left high. */
	pwm_set_chan_level(slice, channel, 0u);
	pwm_set_enabled(slice, true);
	gpio_set_function(GPIO_AUDIO, GPIO_FUNC_PWM);
}

static void
silence(void)
{
	pwm_set_chan_level(slice, channel, 0u);
	playing = false;
}

void
sound_tone(uint32_t hz, uint32_t ms, uint8_t volume)
{
	if (hz < 20u || hz > 20000u || ms == 0u || volume == 0u) {
		silence();
		return;
	}
	if (volume > 100u)
		volume = 100u;

	/* The counter must fit 16 bits, so divide clk_sys down just enough:
	 * the smallest integer divider that keeps a period under 65,536
	 * counts, which leaves the finest resolution for the duty. 20 Hz at
	 * 320 MHz needs 245, inside the divider's 8-bit integer part. */
	const uint32_t clk = clock_get_hz(clk_sys);
	uint32_t div = clk / (hz * 65536u) + 1u;

	if (div > 255u)
		div = 255u;
	const uint32_t period = clk / (div * hz);	/* counts per cycle */
	/* Volume 100 is a 50 % duty: the largest fundamental. */
	const uint32_t level = (period * volume) / 200u;

	pwm_set_clkdiv_int_frac4(slice, (uint8_t)div, 0u);
	pwm_set_wrap(slice, (uint16_t)(period - 1u));
	pwm_set_chan_level(slice, channel, (uint16_t)level);
	started_us = time_us_32();
	length_us = ms * 1000u;
	playing = true;
}

void
sound_bell(void)
{
	bells++;
	sound_tone(BELL_HZ, BELL_MS, BELL_VOLUME);
}

void
sound_poll(void)
{
	if (playing && time_us_32() - started_us >= length_us)
		silence();
}

void
sound_print_status(void)
{
	printf("Sound:       GP%u PWM, %s; %lu bell%s\n", (unsigned)GPIO_AUDIO,
	    playing ? "playing" : "idle", (unsigned long)bells,
	    bells == 1u ? "" : "s");
}

/* The console's bell, $07. Platform-provided, like vid_link_irq_out(): the
 * console is portable and must not know how this board makes a sound. It is
 * called on the render core, the core that owns sound. */
extern "C" void
vid_link_bell(void)
{
	sound_bell();
}

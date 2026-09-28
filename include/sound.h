/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * SIMPLE SOUND: one PWM tone on GPIO_AUDIO (GP40, test point TP613 on the
 * v2.0 PCB), for a low-pass filter, a small amplifier and a speaker.
 *
 * The PWM runs AT THE TONE'S FREQUENCY, so the pin carries a square wave the
 * filter rounds off; its DUTY is the volume, since the fundamental that
 * survives the filter scales with sin(pi * duty), largest at 50 %. The
 * hardware makes the waveform: no interrupt, no DMA, no CPU per cycle. Core 0
 * starts a tone and stops it when its time is up (sound_poll), and that is
 * all this costs.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Claim the pin and its PWM slice; silent until a tone is asked for. */
void sound_init(void);

/* Play `hz` for `ms` milliseconds at `volume` percent (0-100; 100 = the
 * loudest the square wave gives, a 50 % duty). A new tone replaces one in
 * progress. hz outside 20..20000, or ms or volume 0, is silence. */
void sound_tone(uint32_t hz, uint32_t ms, uint8_t volume);

/* The console bell: a short tone. */
void sound_bell(void);

/* Core 0 loop: end a tone whose time is up. Cheap when idle. */
void sound_poll(void);

/* Console diagnostics. */
void sound_print_status(void);

#ifdef __cplusplus
}
#endif

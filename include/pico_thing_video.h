/**
* Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define DEVICE_VERSION "v0.1.0"

extern volatile bool verbose;

#include <cstdint>
#include <cstdio>

#include "hardware/gpio.h"
#include "hardware/pio.h"

// === Pin map ===
// The fast/timing-critical pins (E, Q, R/!W, A5-0, D7-0, !CS) are owned by the
// PIO bus programs and defined in pico_thing_video.pio (exported via pico_thing_video.pio.h):
//	GPIO_E       = 0	// [i]  E clock (supplied by host)
//	GPIO_Q       = 1	// [i]  Q clock (supplied by host)
//	GPIO_RW      = 2	// [i]  Bus R/!W
//	GPIO_A_BASE  = 3	// [i]  Address bus, 6 pins
//	GPIO_D_BASE  = 9	// [io] Data bus, 8 pins
//	GPIO_CS      = 17	// [i]  Device !CS
enum {
	// Interrupt back to the host CPU
	GPIO_IRQ = 26,     // [o*] !IRQ (simulated open-collector)

	// Host reset, observed (input)
	GPIO_RESET = 29,   // [i]  !RESET

	// Default assignment
	GPIO_UART_TX = 30, // [o]  RP2xxx UART Tx (console)
	GPIO_UART_RX = 31, // [i]  RP2xxx UART Rx (console)

	// Fast communication with the video Pico (future)
	// GPIO_PIX_BASE = 32,

	// External power enable
	GPIO_POWER = 46,   // [o]  External power enable
};

// !RESET is active-low and an INPUT: the host drives it, we observe it.
#define RESET_IS_ASSERTED (!gpio_get(GPIO_RESET))

// POWER is active-high.
#define POWER_ASSERT gpio_put(GPIO_POWER, true);
#define POWER_DEASSERT gpio_put(GPIO_POWER, false);
#define POWER_IS_ASSERTED (gpio_get(GPIO_POWER))

// !IRQ is a simulated open-collector output. Becoming an output drives it low
// (assert); becoming an input releases it to high impedance (deassert).
#define ASSERT_IRQ gpio_set_dir(GPIO_IRQ, GPIO_OUT)
#define DEASSERT_IRQ gpio_set_dir(GPIO_IRQ, GPIO_IN)
#define IRQ_IS_ASSERTED ((gpio_get_dir(GPIO_IRQ) == GPIO_OUT) && !gpio_get(GPIO_IRQ))

void init_pins_range(uint8_t base, uint8_t count);

struct pio_dma {
	PIO pio;
	int sm;
	int address_channel;
	int data_channel;
	uint32_t irq;
};

// === Device address window ===
// The Pico exposes a 64-byte register window (see registers.h) that the host
// 6809 reads and writes. Device sub-registers (video, keyboard, mouse) will be
// laid out here as they are added.
#define REGISTER_BASE	uint16_t(0xFFC0u)
#define ADDRESS(index)	uint16_t(REGISTER_BASE + index)

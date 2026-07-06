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
// The fast/timing-critical pins (E, Q, R/!W, A2-0, D7-0, !CS) are owned by the
// PIO bus programs and defined in pico_thing_video.pio (exported via pico_thing_video.pio.h):
//	GPIO_E       = 25	// [i]  E clock (supplied by host)
//	GPIO_Q       = 26	// [i]  Q clock (supplied by host)
//	GPIO_RW      = 27	// [i]  Bus R/!W
//	GPIO_A_BASE  = 28	// [i]  Address bus, 3 pins (A2-0)
//	GPIO_D_BASE  = 31	// [io] Data bus, 8 pins (D7-0)
//	GPIO_CS      = 39	// [i]  Device !CS (above the D block)
enum {
	// Default assignment
	GPIO_UART_TX = 0,	// [o]  RP2xxx UART Tx (console)
	GPIO_UART_RX = 1,	// [i]  RP2xxx UART Rx (console)

	// Interrupt back to the host CPU
	GPIO_IRQ     = 24,	// [o*] !IRQ (simulated open-collector)

	// Host reset, observed (input)
	GPIO_RESET   = 23,	// [i]  !RESET
};

// !RESET is active-low and an INPUT: the host drives it, we observe it.
#define RESET_IS_ASSERTED (!gpio_get(GPIO_RESET))

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
// The Pico exposes an 8-byte register window (see registers.h) that the host
// 6809 reads and writes.
#define REGISTER_BASE	uint16_t(0xFFC0u)
#define ADDRESS(index)	uint16_t(REGISTER_BASE + index)

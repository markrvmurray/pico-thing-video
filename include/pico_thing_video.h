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
// The first bracket is hardware-validation state, as in the main pico's
// pico_thing.h: [Y] proven on hardware, [N] known bad, [ ] not yet exercised.
// The second is direction: [i] input, [o] output, [o*] simulated
// open-collector output, [io] bidirectional.
enum {
	// Console UART (UART0)
	GPIO_UART_TX  = 0,	// [Y] [o]  RP2xxx UART Tx (console)
	GPIO_UART_RX  = 1,	// [Y] [i]  RP2xxx UART Rx (console)

	// USB-C front end (v2.0 PCB only; not connected on the Stamp XL carrier,
	// where VBUS comes from the carrier's own switched USB-A port). Role
	// drives the 74LVC2G3157 CC-mux selects and the TPS2051B VBUS-switch
	// enable together: high = host (Rp presented on CC, VBUS sourced), low =
	// device (Rd presented, VBUS off). !PowerFault is the TPS2051B's
	// open-drain !FLT (10k pull-up on the board); low = overcurrent or
	// overtemperature while sourcing VBUS.
	GPIO_PWR_FAULT = 7,	// [Y] [i]  !FLT from the TPS2051B VBUS switch (unfaulted state proven; fault path untested)
	GPIO_USB_ROLE  = 8,	// [Y] [o]  USB role select: 1 = host, 0 = device

	// HDMI connector sideband (v2.0 PCB only; unused by the firmware so far)
	GPIO_HPD      = 9,	// [Y] [i]  HDMI hot-plug detect (external 4k7 pull-down: erratum E9)
	GPIO_DDC_SDA  = 10,	// [Y] [io] HDMI DDC (I2C1 SDA)
	GPIO_DDC_SCL  = 11,	// [Y] [io] HDMI DDC (I2C1 SCL)

	// HSTX DVI output — 4 differential pairs on GP12-19 (Pico DVI Sock).
	// Per-pair lane/clock assignment is board-gated in hstx_dvi_init()
	// (video.cpp): SolderParty has CK on GP14/15; Pimoroni has CK on GP12/13.
	GPIO_HSTX_BASE = 12,	// [Y] [o]  HSTX DVI base (GPIO_HSTX_COUNT pins)

	// Host reset, observed (input)
	GPIO_RESET    = 20,	// [Y] [i]  !RESET

	// Interrupt back to the host CPU
	GPIO_IRQ      = 21,	// [ ] [o*] !IRQ (simulated open-collector; the one bus signal not yet proven)

	// The timing-critical bus pins (E, Q, R/!W, A2-0, D7-0, !CS) and the two PIO
	// trace pins are owned by the PIO programs and defined in
	// pico_thing_video.pio (exported via pico_thing_video.pio.h). pioasm needs
	// them at assembly time, so they cannot live in this header:
//	GPIO_TRACE_RD = 23	// [ ] [o]  PIO bus-read trace (debug)
//	GPIO_TRACE_WR = 24	// [ ] [o]  PIO bus-write trace (debug)
//	GPIO_E        = 25	// [Y] [i]  E clock (supplied by host)
//	GPIO_Q        = 26	// [Y] [i]  Q clock (supplied by host)
//	GPIO_RW       = 27	// [Y] [i]  Bus R/!W
//	GPIO_A_BASE   = 28	// [Y] [i]  Address bus, 3 pins (A0 on 28, bit i on base+i)
//	GPIO_D_BASE   = 31	// [Y] [io] Data bus, 8 pins (D0 on 31, bit i on base+i)
//	GPIO_CS       = 39	// [Y] [i]  Device !CS (above the D block)

	// Main-power sense (v2.0 PCB): the board's main 5V supply through a
	// divide-by-2 resistor pair, so a healthy rail reads ~2.5 V. Sampled by
	// power.cpp to follow the host system's power state automatically.
	GPIO_PWR_SENSE = 46,	// [Y] [i]  ADC6: main 5V rail / 2
};

// HSTX drives GPIO_HSTX_COUNT consecutive pins starting at GPIO_HSTX_BASE.
#define GPIO_HSTX_COUNT 8u

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
// 6809 reads and writes. External glue decodes the guest address and asserts
// !CS over $FF10-$FF17; the Pico itself only sees !CS and A2-0, so REGISTER_BASE
// is just the label used by the register dump, not a value the bus path decodes.
#define REGISTER_BASE	uint16_t(0xFF10u)
#define ADDRESS(index)	uint16_t(REGISTER_BASE + index)

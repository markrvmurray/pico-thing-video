/**
* Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pico2 (RP2350) firmware acting as a slave peripheral on an external
 * MC6809E bus. The host system supplies the E/Q clocks and drives the
 * address/data bus; this Pico watches its !CS and services byte reads and
 * writes to an 8-byte register window at $FF10, through which it also serves
 * DVI video and USB HID input.
 *
 * THIS FILE is startup and the Core 0 loop: clocks and core voltage, the
 * one-off bring-up order, and the round-robin of console, power sensing and
 * rendering. The two halves it starts live apart on purpose --
 *
 *   bus_engine.cpp  hard real-time: bus PIO/DMA, the ISRs, Core 1.
 *   console.cpp     the slow path: command dispatch and status.
 *
 * -- because they have opposite deadline classes and were previously in one
 * file, where the only thing keeping unbounded console work out of the bus
 * core was discipline.
 */

// Build-time toggle (must precede the includes it gates). When defined, clk_sys
// runs at 320 MHz for more Core-0 grunt rendering the 6809's video commands, and
// the HSTX/DVI clock is decoupled in main() so the pixel clock stays at 38.4 MHz.
// Off by default; validate on the bench before relying on it.
// #define FAST_SYS_CLK_320

#include <cstdio>
#include <ctime>

#include "pico/aon_timer.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

#include "build_time.h"
#include "bus_engine.h"
#include "chip_stepping.h"
#include "console.h"
#include "i2c_service.h"
#include "pico_thing_video.h"
#include "power.h"
#include "usb.h"
#include "vid_link.h"
#include "video.h"
#include "video_render.h"   // video_frames, the cursor blink timebase

// ── System clock ────────────────────────────────────────────────────────────
// Default 192 MHz: the HSTX DVI serialiser divides clk_sys to a 38.4 MHz pixel
// clock (CSR CLKDIV=5). clk_hstx tracks clk_sys, so the DVI mode is tied to it.
// The bus PIO runs off clk_sys at div=1, still far faster than the ~1-2 MHz 6809.
// FAST_SYS_CLK_320 (toggle at the top of this file) selects 320 MHz instead,
// with clk_hstx decoupled in main() to hold the pixel clock at 38.4 MHz.
#ifdef FAST_SYS_CLK_320
#define SYS_CLK_DEFAULT 320000000
#else
#define SYS_CLK_DEFAULT 192000000
#endif

// Default pico uart for command-line interactive access
#define CONSOLE uart0
#define CONSOLE_BAUDRATE 115200

#ifdef MEASURE_FREQS
static void
measure_freqs()
{
	uint32_t f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY)/1000;
	uint32_t f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY)/1000;
	uint32_t f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
	uint32_t f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS)/1000;
	uint32_t f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI)/1000;
	uint32_t f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB)/1000;
	uint32_t f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC)/1000;
	printf("pll_sys  = %d MHz\n", f_pll_sys);
	printf("pll_usb  = %d MHz\n", f_pll_usb);
	printf("rosc     = %d kHz\n", f_rosc);
	printf("clk_sys  = %d MHz\n", f_clk_sys);
	printf("clk_peri = %d MHz\n", f_clk_peri);
	printf("clk_usb  = %d MHz\n", f_clk_usb);
	printf("clk_adc  = %d MHz\n", f_clk_adc);
// Can't measure clk_ref/xosc as it is the ref
}
#endif

int
main()
{
#ifdef FAST_SYS_CLK_320
	// 320 MHz needs a core-voltage bump. Verify stability/thermals on hardware.
	vreg_set_voltage(VREG_VOLTAGE_1_30);
#else
	// 192 MHz is only a mild overclock; the default core voltage is enough
	// (pico-vidterm ran 192 MHz at stock voltage).
	vreg_set_voltage(VREG_VOLTAGE_1_10);
#endif
	sleep_ms(5);

	gpio_set_function(GPIO_UART_TX, UART_FUNCSEL_NUM(uart0, GPIO_UART_TX));
	gpio_set_function(GPIO_UART_RX, UART_FUNCSEL_NUM(uart0, GPIO_UART_RX));

	stdio_init_all();

	printf("\n\nPico2 MC6809E bus peripheral %s\n", DEVICE_VERSION);
	printf("Pico2 MC6809E built on %s %s\n", __DATE__, __TIME__);
	chip_stepping_print();

#ifdef MEASURE_FREQS
	measure_freqs();
#endif
	printf("Core0 raising sys_clock to %d MHz ... ", SYS_CLK_DEFAULT/1000000);
	if (!set_sys_clock_hz(SYS_CLK_DEFAULT, false)) {
		printf("unsuccessful! Reverting to %d MHz ... ", SYS_CLK_HZ/1000000);
		set_sys_clock_hz(SYS_CLK_HZ, true);
	}

#ifdef FAST_SYS_CLK_320
	// clk_sys now runs at 320 MHz off PLL_SYS. Decouple the HSTX/DVI clock so the
	// pixel clock stays at 38.4 MHz (clk_hstx 192 MHz / CSR CLKDIV 5). We can't
	// divide 320 MHz down to 192 with an integer, so drive clk_hstx from PLL_USB
	// instead: reconfigure PLL_USB to 192 MHz, keep USB at 48 MHz (192 / 4 on
	// clk_usb), and take clk_hstx straight off PLL_USB. Must run before usb_init.
	//
	// UNTESTED — this branch is not built by default (so it is not compile- or
	// bench-checked). Validate the PLL params, the AUXSRC constants, and USB
	// enumeration on hardware before relying on it. Note: usb_init()'s
	// "USB clock != 48000 kHz" check measures the PLL_USB *primary* output, which
	// is now 192 MHz — that warning is expected and cosmetic (clk_usb is 48 MHz).
	pll_init(pll_usb, 1, 960 * MHZ, 5, 1);   // VCO 960 MHz -> 192 MHz PLL_USB out
	clock_configure(clk_usb, 0,
		CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
		192 * MHZ, 48 * MHZ);                // 192 / 4 = 48 MHz for USB
	clock_configure(clk_hstx, 0,
		CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
		192 * MHZ, 192 * MHZ);               // 192 MHz -> 38.4 MHz DVI pixel clock
#endif

	stdio_init_all();
	uart_init(CONSOLE, CONSOLE_BAUDRATE);
	printf("successful!\n");
	printf("Core0 console baudrate set to %d\n", CONSOLE_BAUDRATE);

	usb_init();
	printf("Core0 initialised USB\n");

	tm tm = build_time_tm();
	aon_timer_stop();
	aon_timer_start_calendar(&tm);

	// Follow the host system's main 5V rail (ADC6, divide-by-2): the bus and
	// video interfaces serve only while it is up. Unlike the main pico there
	// is no power *control* here — the peripheral simply comes alive with
	// power on. On power loss it reboots back into this standby (below).
	power_init();
	i2c_service_init();
	if (!power_is_on()) {
		printf("Core0 standby: main 5V is absent — console only\n");
		while (!power_is_on()) {
			console_poll();
			power_task();
		}
		printf("\nCore0 main 5V detected\n");
		console_reprompt();
	}

	// Bring up the bus peripheral and launch Core 1 (bus engine + video).
	bus_engine_start();

	printf("Core0 ready!\n");

	// Wait for Core 1's main loop to be running before entering the console.
	while (bus_engine_loop_count() < 100u) {
		sleep_ms(1u);
		usb_poll();
	}

	console_print_time();

	for (;;) {
		console_poll();
		power_task();

		// Render the guest's command stream here, off the bus core.
		vid_link_render();

		// Cursor, once per frame rather than once per pass: this loop
		// spins, and the cursor code lives in flash, so calling it every
		// time would flood the bus with instruction fetches for no gain.
		{
			static uint32_t serviced_frame;
			const uint32_t f = video_frames;
			if (f != serviced_frame) {
				serviced_frame = f;
				vidterm_cursor_service();
			}
		}

		// Power lost: quiesce everything except the console. A clean reboot
		// back into the standby wait above is the shutdown — every power
		// cycle then gets a from-scratch bring-up, instead of trying to
		// pause and resume the PIO state machines, the DMA chains and the
		// video scan-out in place.
		if (!power_is_on()) {
			printf("\nCore0 main 5V lost — restarting into standby\n");
			stdio_flush();
			sleep_ms(20u);
			watchdog_reboot(0u, 0u, 0u);
			for (;;)
				tight_loop_contents();
		}
	}
}

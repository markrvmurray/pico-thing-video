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

// FAST_SYS_CLK_320 comes from the BUILD, not from here: configure with
// -DFAST_SYS_CLK_320=ON. When set, clk_sys runs at 320 MHz for more Core-0
// grunt rendering the 6809's video commands, the HSTX/DVI clock is decoupled
// onto PLL_USB in main() so the pixel clock stays at 38.4 MHz, and the core
// voltage is raised. Off by default; validate on the bench before relying on
// it. (It was a #define here once, which meant a 320 MHz image was an edited
// source tree rather than a configuration, and an .elf could quietly become
// the other build on a later rebuild.)

#include <cstdio>
#include <ctime>

#include "pico/aon_timer.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/powman.h"
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
//
// Core voltage is chosen HERE, beside the clock it exists for, because the two
// are a pair: raising one without the other is either unstable or needlessly
// hot. vreg_set_voltage() does not know or care what clk_sys is doing.
//
// Careful if raising this: the SDK SILENTLY CLAMPS any request above
// VREG_VOLTAGE_MAX (1.30 V) unless vreg_disable_voltage_limit() is called
// first. Code can therefore ask for 2.0 V, read as though it runs at 2.0 V,
// and actually run at 1.30 V -- which is the case on the main pico, whose
// reliable 320 MHz is in fact achieved at 1.30 V. The boot line below reports
// a clamp rather than letting it hide.
#ifdef FAST_SYS_CLK_320
#define SYS_CLK_DEFAULT 320000000
#define CORE_VOLTAGE    VREG_VOLTAGE_1_30
#define CORE_VOLTAGE_V  "1.30"
#else
#define SYS_CLK_DEFAULT 192000000
#define CORE_VOLTAGE    VREG_VOLTAGE_DEFAULT	/* 1.10 V, the power-up value */
#define CORE_VOLTAGE_V  "1.10"
#endif

// Default pico uart for command-line interactive access
#define CONSOLE uart0
#define CONSOLE_BAUDRATE 115200

// ── Core 0 load ─────────────────────────────────────────────────────────────
// What the rendering core actually spends its time on, so "we need more grunt"
// can be a measurement rather than an impression. Cheap enough to leave in:
// two timer reads per pass, on the core that has no deadline.
static uint32_t core0_epoch_us;
static uint32_t core0_serviced_frame;	/* seeded at loop entry, not zero */
static uint32_t core0_loops;
static uint64_t render_us;		// total inside vid_link_render()
static uint32_t render_max_us;		// worst single call
static uint64_t cursor_us;		// total inside the overlay service
static uint32_t frames_missed;		// frames that went by unserviced
static uint32_t frames_missed_burst;	// most missed between two services

void
core0_load_print(void)
{
	const uint32_t elapsed = time_us_32() - core0_epoch_us;
	// Busy fraction in tenths of a percent: no float printf in this
	// firmware, and a load figure is not the reason to start.
	const uint32_t busy = elapsed ?
	    (uint32_t)(((render_us + cursor_us) * 1000u) / elapsed) : 0u;

	printf("Core0 loop:  %lu  busy %lu.%lu%% (render %llu us, max %lu us;"
	       " overlay %llu us)\n",
	       (unsigned long)core0_loops,
	       (unsigned long)(busy / 10u), (unsigned long)(busy % 10u),
	       (unsigned long long)render_us, (unsigned long)render_max_us,
	       (unsigned long long)cursor_us);
	// Printing this costs frames of its own: the status page is the best
	// part of a kilobyte, and 115200 baud moves about 11 bytes per
	// millisecond, so each report blocks this loop for several frames.
	// The figure therefore includes the cost of asking for it.
	printf("Frames:      %lu drawn, %lu missed (worst run %lu)\n",
	       (unsigned long)video_frames,
	       (unsigned long)frames_missed,
	       (unsigned long)frames_missed_burst);
}

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
	// Voltage first, then frequency, with time to settle between: the clock
	// is raised further down, after stdio is up.
	vreg_set_voltage(CORE_VOLTAGE);
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
	// Say which voltage was selected, and whether the regulator will
	// actually deliver it.
	//
	// NOT by reading VSEL back: the limit acts on the regulator's OUTPUT,
	// not on the register, so a request above 1.30 V latches and reads back
	// at its full value while the rail sits at 1.30. vreg_get_voltage()
	// echoes the request and cannot see the clamp. The limit bit can, so
	// ask that instead. (There is no on-chip sense of the core rail, so
	// this is an inference, not a measurement.)
	{
		const bool limited = (powman_hw->vreg_ctrl &
		    POWMAN_VREG_CTRL_DISABLE_VOLTAGE_LIMIT_BITS) == 0u;
		const bool clamped = limited && ((CORE_VOLTAGE) > VREG_VOLTAGE_MAX);
		printf("Core0 core voltage %s V for %d MHz%s\n",
		       CORE_VOLTAGE_V, SYS_CLK_DEFAULT/1000000,
		       clamped ? " — CLAMPED to 1.30 V, the request exceeds"
		                 " VREG_VOLTAGE_MAX and the limit is on" : "");
	}

	printf("Core0 raising sys_clock to %d MHz ... ", SYS_CLK_DEFAULT/1000000);
	if (!set_sys_clock_hz(SYS_CLK_DEFAULT, false)) {
		printf("unsuccessful! Reverting to %d MHz ... ", SYS_CLK_HZ/1000000);
		set_sys_clock_hz(SYS_CLK_HZ, true);
		// Drop the voltage back: it was raised for a clock we did not
		// get, and the fallback needs no more than the default. Order
		// matters -- frequency down first, then voltage. Without this a
		// failed clock change half-reverts, leaving the part hot for no
		// benefit in exactly the situation where something is already
		// wrong.
		vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
		printf("core voltage back to 1.10 V ... ");
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
	// FIRST, get the console off PLL_USB.
	//
	// set_sys_clock_hz() deliberately parks clk_peri on PLL_USB at 48 MHz,
	// so that peripherals keep a clock which does not move when clk_sys
	// does (PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK defaults off).
	// Moving PLL_USB to 192 MHz for the DVI clock therefore drags clk_peri
	// with it -- four times too fast -- while the SDK goes on believing it
	// is 48 MHz. uart_init() below reads that cached figure, so it computes
	// a divisor four times wrong and the console emits garbage. Re-
	// initialising the UART does NOT fix that on its own: the number it
	// works from is the lie.
	//
	// clock_configure() sets the hardware AND updates the cached frequency,
	// which is what makes the later uart_init() correct. Point clk_peri at
	// the crystal rather than at either PLL: the console is then immune to
	// any future experiment with the clock tree, which is precisely the
	// thing one wants working while experimenting with the clock tree.
	// clk_peri's divider is two bits (1 to 3), so it could not have divided
	// 192 back down to 48 in any case.
	stdio_flush();
	clock_configure(clk_peri, 0,
		CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
		XOSC_HZ, XOSC_HZ);

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

	// Start the frame accounting from where the display actually is. Core 1
	// brought the video engine up and has been drawing since before this
	// loop existed; counting those frames as missed charges this core for
	// time it did not have, and shows up as a large "worst run" that never
	// happened.
	core0_serviced_frame = video_frames;
	core0_epoch_us = time_us_32();
	for (;;) {
		core0_loops = core0_loops + 1u;

		console_poll();
		power_task();

		// Render the guest's command stream here, off the bus core.
		{
			const uint32_t t0 = time_us_32();
			vid_link_render();
			const uint32_t dt = time_us_32() - t0;
			render_us += dt;
			if (dt > render_max_us)
				render_max_us = dt;
		}

		// Cursor, once per frame rather than once per pass: this loop
		// spins, and the cursor code lives in flash, so calling it every
		// time would flood the bus with instruction fetches for no gain.
		{
			const uint32_t f = video_frames;
			if (f != core0_serviced_frame) {
				// Frames that went by without a service are the
				// measure that matters: the display does not
				// wait, so anything above zero means one pass
				// round this loop outlasted a frame. That is
				// the signal Core 0 has run out of time, and
				// the reason to reach for more of it.
				const uint32_t skipped = f - core0_serviced_frame - 1u;
				frames_missed += skipped;
				if (skipped > frames_missed_burst)
					frames_missed_burst = skipped;
				core0_serviced_frame = f;

				const uint32_t t0 = time_us_32();
				vidterm_cursor_service();
				cursor_us += time_us_32() - t0;
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

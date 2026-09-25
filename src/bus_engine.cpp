/* bus_engine.cpp -- the MC6809E bus peripheral: PIO, DMA, the two bus ISRs,
 * and Core 1.
 *
 * This is the HARD REAL-TIME half of the firmware, and it is a separate
 * translation unit for that reason rather than for tidiness. Core 1 owes the
 * video scan-out a DMA re-arm inside a ~5 us blanking interval while also
 * serving bus interrupts at 6809 rates; everything here is short, bounded and
 * RAM-resident where it must be. Nothing unbounded belongs in this file --
 * console work, rendering and USB all live on Core 0.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cstdio>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/resets.h"

#include "bus_engine.h"
#include "pico_thing_video.h"
#include "registers.h"
#include "vid_link.h"
#include "video.h"

#include "pico_thing_video.pio.h"

// --- PIO / DMA resources ---
// Read responder on pio0; write capturer on pio1. Each uses a two-channel DMA
// chain: an address channel feeds the target register address from the PIO,
// and a data channel moves the byte to/from the register window.
#define BUS_READ_PIO  pio0
#define BUS_WRITE_PIO pio1

static pio_dma piodma_read = {
	BUS_READ_PIO,
	0,
	0,
	1,
	DMA_IRQ_0
};

static pio_dma piodma_write = {
	BUS_WRITE_PIO,
	0,
	4,
	5,
	DMA_IRQ_1
};

// PIO program offsets (needed for pio_remove_program on shutdown)
static int pio_read_offset;
static int pio_write_offset;

static volatile bool core1_initialised = false;

// --- Core 1 diagnostics ---
static volatile uint32_t core1_loop_count = 0u;
static volatile uint32_t core1_isr_count = 0u;
static volatile uint8_t  core1_last_write_loc = 0u;
static volatile uint8_t  core1_last_read_loc = 0u;

// --- Bus read notification ---
// The read DMA auto-returns read_registers[addr] to the host. The ISR records
// the location so Core 1 can apply side effects (e.g. auto-clearing status
// registers) for device sub-registers added later.
static volatile bool    read_irq_received = false;
static volatile uint8_t read_location = 0u;
// Per-offset tally of guest reads. `Last read` alone only names the most
// recent offset, which cannot distinguish "the guest is polling steadily"
// from "the guest read once and stopped" — a distinction that cost a long
// hunt once, when a stalled console turned out to be the guest never issuing
// a read rather than anything in the serve path. Counting per offset shows
// the access pattern directly: a status register polled at a steady rate,
// a data register read only when the guest consumes, and reserved offsets
// that should stay at zero.
static volatile uint32_t read_offset_count[registers::register_length];

// --- Bus write ring ---
// The write DMA stores the guest byte into write_registers[addr]. The ISR
// mirrors it into the read side and pushes {location, value} here for Core 1
// to dispatch. Replaces a single-slot notification that dropped back-to-back
// writes arriving faster than the for(;;) loop could drain them.
struct write_entry {
	uint8_t location;
	uint8_t value;
};
static constexpr uint8_t WRITE_RING_SIZE = 32u;  // must be power of 2
static constexpr uint8_t WRITE_RING_MASK = WRITE_RING_SIZE - 1u;
static volatile write_entry write_ring[WRITE_RING_SIZE];
static volatile uint8_t write_ring_head = 0u;    // written by ISR
static volatile uint8_t write_ring_tail = 0u;    // read by for(;;) loop

// ── Handler cost ────────────────────────────────────────────────────────────
// What the bus interrupt handlers actually cost, in CYCLES. Microseconds are
// useless here: these should run in single-digit microseconds, and a timer
// with 1 us resolution cannot see the difference between comfortable and
// nearly-late. The Cortex-M33 cycle counter gives ~5 ns at 192 MHz.
//
// This exists because the margin in these handlers has been invisible. A guest
// hang was chased for days across four wrong hypotheses, and every one of them
// would have been settled in minutes by a number saying how long these take and
// whether the worst case was creeping. Cheap enough to leave in: two register
// reads per handler, which is small against what the handler does and is
// honest about including itself.
#define DWT_LAR		(*(volatile uint32_t *)0xE0001FB0)
#define DWT_CTRL	(*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT	(*(volatile uint32_t *)0xE0001004)
#define SCB_DEMCR	(*(volatile uint32_t *)0xE000EDFC)

static volatile uint32_t isr_read_max;		// cycles, worst single entry
static volatile uint32_t isr_write_max;
static volatile uint32_t isr_read_total;	// cycles, summed
static volatile uint32_t isr_write_total;
static volatile uint32_t isr_read_calls;
static volatile uint32_t isr_write_calls;

// A MAXIMUM IS MONOTONIC IN SAMPLE COUNT: it latches the worst value since
// boot, so a longer run can only ever report a bigger one, and two runs of
// different length cannot be compared by it at all. That flaw produced a
// threefold overstatement of one clock against another before it was noticed.
//
// So count the TAIL as well: how many handler entries exceeded a fixed wall-
// clock threshold, against how many there were. A rate is comparable across
// runs of any length and across clock speeds, which the maximum is not. The
// threshold is held in cycles and computed once from clk_sys, so the ISR does
// no arithmetic beyond a compare.
#define ISR_TAIL_NS	500u		// half a microsecond
static uint32_t isr_tail_cy;		// ISR_TAIL_NS converted, set at enable
static volatile uint32_t isr_read_over;

// Stale-service detection: the previous handler's entry time and duration, and
// how often a read arrived before the previous restage had finished.
static uint32_t          isr_read_prev_in;
static uint32_t          isr_read_prev_dt;
static volatile uint32_t isr_read_stale;
static volatile uint32_t isr_read_stale_worst;
static volatile uint32_t isr_write_over;

// MUST BE CALLED ON THE CORE BEING MEASURED. The DWT is a per-core debug
// component on the Cortex-M33: enabling it on core 0 leaves core 1's counter
// stopped, and the handlers then time themselves as taking zero cycles --
// which is exactly how this first shipped, and reads as "instrumentation
// working, margin enormous" rather than as a fault.
static void
cycle_counter_enable(void)
{
	SCB_DEMCR = SCB_DEMCR | (1u << 24);	// TRCENA
	DWT_LAR = 0xC5ACCE55u;			// unlock, where the lock exists
	DWT_CYCCNT = 0u;
	DWT_CTRL = DWT_CTRL | 1u;		// CYCCNTENA

	// Fixed WALL-CLOCK threshold, so the tail rate means the same thing at
	// 192 MHz and at 320. Computed once; the handlers only compare.
	isr_tail_cy = (uint32_t)(((uint64_t)clock_get_hz(clk_sys) *
	    ISR_TAIL_NS) / 1000000000u);
}

// ISR for the R/!W bus read loop and DMA chain.
// The read DMA has already handed read_registers[addr] to the host; we only
// record which location was read for optional Core 1 side effects.
void __isr
__time_critical_func(dma_bus_read_irq_handler())
{
	const uint32_t t_in = DWT_CYCCNT;
	dma_channel_acknowledge_irq0(piodma_read.data_channel);
	core1_isr_count++;
	const uintptr_t read_addr = dma_channel_hw_addr(piodma_read.data_channel)->read_addr;
	read_location = read_addr & registers::register_mask;
	read_offset_count[read_location]++;
	read_irq_received = true;
	// Pop-on-read for the link's Data register happens HERE, not in the
	// Core 1 loop: every guest read gets its own ISR invocation (6809 bus
	// reads are far slower than this tier's latency), so the pop can
	// neither be lost to mailbox overwrite nor double-counted.
	vid_link_guest_read(read_location);
	if (read_location == VID_MOUSE_OFFSET)
		vid_link_mouse_read();
	else if (read_location == VID_INFO_OFFSET)
		vid_link_info_read();

	const uint32_t dt = DWT_CYCCNT - t_in;
	isr_read_total = isr_read_total + dt;
	isr_read_calls = isr_read_calls + 1u;
	if (dt > isr_read_max)
		isr_read_max = dt;
	if (dt > isr_tail_cy)
		isr_read_over = isr_read_over + 1u;

	// DID WE SERVE A STALE BYTE? This is the failure the ISR's duration
	// only hints at. The DMA hands the guest whatever is staged the moment
	// !CS asserts; this handler then advances and restages behind it. If
	// the next read arrives before that finished, the guest was given the
	// byte it had already taken -- and on a 6809 a stale fetch is a wild
	// PC, which looks like the guest crashing of its own accord long
	// afterwards.
	//
	// The test is the interval between consecutive handler entries against
	// how long the previous one took. Closer together than that, and the
	// restage had not completed when the next cycle was served.
	//
	// It cannot happen at 1 MHz with a healthy handler: reads are at least
	// a microsecond apart and the handler takes 0.4 us at worst. So a
	// non-zero count here is not a tight margin, it is proof that
	// something stalled this core long enough to miss a bus cycle -- which
	// is exactly what a hang with no other explanation needs.
	if (isr_read_prev_in != 0u) {
		const uint32_t gap = t_in - isr_read_prev_in;

		if (gap < isr_read_prev_dt) {
			isr_read_stale = isr_read_stale + 1u;
			if (gap < isr_read_stale_worst || isr_read_stale == 1u)
				isr_read_stale_worst = gap;
		}
	}
	isr_read_prev_in = t_in | 1u;	/* never zero: 0 means "no previous" */
	isr_read_prev_dt = dt;
}

// ISR for the R/!W bus write loop and DMA chain.
// The write DMA has stored the guest byte into write_registers[addr]. Mirror it
// into the read side so the host reads back what it wrote, then queue it for
// Core 1 to dispatch any device side effects.
void __isr
__time_critical_func(dma_bus_write_irq_handler())
{
	const uint32_t t_in = DWT_CYCCNT;
	dma_channel_acknowledge_irq1(piodma_write.data_channel);
	core1_isr_count++;
	const uintptr_t written_addr = dma_channel_hw_addr(piodma_write.data_channel)->write_addr;
	const uint8_t wloc = written_addr & registers::register_mask;
	const uint8_t wval = *reinterpret_cast<volatile uint8_t *>(written_addr);
	// Mirror into the read side (scratch-register behaviour for the
	// reserved registers). The link offsets are NOT mirrored: their read
	// side is device-owned (VID.Data stages the input head byte, VID.Stat
	// the status, VID.Mouse the pointer snapshot), and a guest write must
	// not clobber it.
	if (wloc > VID_INFO_OFFSET)
		reg[wloc] = wval;
	// Mouse re-latch/resync happens HERE rather than on the Core 1 drain:
	// a guest may read $FF12 in the very next instruction, which would
	// race the drain, and this write exists precisely to leave the guest
	// in a known state.
	if (wloc == VID_MOUSE_OFFSET)
		vid_link_mouse_write();
	else if (wloc == VID_INFO_OFFSET)
		vid_link_info_write();
	// Queue for Core 1.
	const uint8_t h = write_ring_head;
	const uint8_t next_h = (h + 1u) & WRITE_RING_MASK;
	if (next_h == write_ring_tail)
		write_ring_tail = (write_ring_tail + 1u) & WRITE_RING_MASK;
	write_ring[h].location = wloc;
	write_ring[h].value = wval;
	write_ring_head = next_h;

	const uint32_t dt = DWT_CYCCNT - t_in;
	isr_write_total = isr_write_total + dt;
	isr_write_calls = isr_write_calls + 1u;
	if (dt > isr_write_max)
		isr_write_max = dt;
	if (dt > isr_tail_cy)
		isr_write_over = isr_write_over + 1u;
}

static void
peripheral_clear()
{
	registers::clear();
}

// Push a single {location, value} write into the ring so the Core 1 dispatch
// loop treats a console modify the same as a guest bus write.
void
bus_engine_inject_write(uint8_t loc, uint8_t val)
{
	const uint8_t h = write_ring_head;
	const uint8_t next_h = (h + 1u) & WRITE_RING_MASK;
	if (next_h == write_ring_tail)
		write_ring_tail = (write_ring_tail + 1u) & WRITE_RING_MASK;
	write_ring[h].location = loc;
	write_ring[h].value = val;
	write_ring_head = next_h;
}

void
init_pins_range(const uint8_t base, const uint8_t count)
{
	for (uint8_t i = 0; i < count; ++i)
		gpio_init(base + i);
}

// One-off Core 1 bring-up: wire the bus + video IRQs/DMA/PIO and start the
// video engine. Deliberately flash-resident (default placement) — it runs once
// at startup, so keeping it out of RAM leaves that budget for the framebuffer.
// Only the hot drain loop in main_core1() has to stay RAM-resident.
// __noinline is load-bearing: without it the compiler could inline this back
// into main_core1() (which is in RAM), defeating the point.
// Bus DMA IRQs sit one priority step below the video scanline IRQ (which stays
// at 0, the highest), so the per-scanline video handler preempts a running bus
// handler and never misses its DMA re-arm deadline. Lower number = higher
// priority on Cortex-M33; 0x40 keeps the bus well above the default (0x80).
#define BUS_IRQ_PRIORITY 0x40u

static void __noinline
core1_bringup()
{
	printf("Core1 process starting\n");

	// On this core, because that is where the bus handlers run and the
	// counter is per-core.
	cycle_counter_enable();

	irq_set_exclusive_handler(piodma_read.irq, dma_bus_read_irq_handler);
	irq_set_priority(piodma_read.irq, BUS_IRQ_PRIORITY);
	irq_set_enabled(piodma_read.irq, true);
	dma_channel_set_irq0_enabled(piodma_read.data_channel, true);
	dma_channel_start(piodma_read.address_channel);
	pio_sm_put(piodma_read.pio, piodma_read.sm, registers::read_address() >> 3); // Three bits for A0-2
	printf("Core1 read DMA chain configured\n");

	irq_set_exclusive_handler(piodma_write.irq, dma_bus_write_irq_handler);
	irq_set_priority(piodma_write.irq, BUS_IRQ_PRIORITY);
	irq_set_enabled(piodma_write.irq, true);
	dma_channel_set_irq1_enabled(piodma_write.data_channel, true);
	dma_channel_start(piodma_write.address_channel);
	pio_sm_put(piodma_write.pio, piodma_write.sm, registers::write_address() >> 3); // Three bits for A0-2
	printf("Core1 write DMA chain configured\n");

	pio_sm_set_enabled(piodma_read.pio, piodma_read.sm, true);
	pio_sm_set_enabled(piodma_write.pio, piodma_write.sm, true);
	printf("Core1 read pio%d/sm%d and write pio%d/sm%d started\n",
	       PIO_NUM(piodma_read.pio), piodma_read.sm,
	       PIO_NUM(piodma_write.pio), piodma_write.sm);

	// Bring up the video engine on this core: its per-scanline DMA IRQ
	// (DMA_IRQ_2) is serviced here, alongside the bus IRQs (DMA_IRQ_0/1).
	video_init();
	printf("Core1 video engine started\n");
#ifdef VIDEO_DEMO
	// Test-only: draw the built-in test screen once. The DMA scans it out
	// autonomously afterwards, so we fall through to the bus loop.
	video_demo();
	printf("Core1 video demo drawn\n");
#endif

	// Bring the guest link up last: READY means everything above works.
	vid_link_init();
	printf("Core1 guest link ready (protocol version %u)\n", VID_STAT_VERSION);

	printf("Core1 process started\n");
	core1_initialised = true;
}

// Core 1 entry: one-off bring-up (in flash) then the RAM-resident drain loop.
static void
__no_inline_not_in_flash_func(main_core1)()
{
	core1_bringup();

	bool reset_was_asserted = false;

	for (;;) {
		core1_loop_count++;

		// Observe host !RESET. On the asserting edge, clear the register
		// window so the peripheral presents a clean slate to the host.
		const bool reset_now = RESET_IS_ASSERTED;
		if (reset_now && !reset_was_asserted) {
			registers::clear();
			vid_link_reset();	// flush FIFOs, restage Data/Stat
		}
		reset_was_asserted = reset_now;

		// Drain guest writes. The byte was already mirrored to the read side
		// in the ISR; this loop is where video/keyboard/mouse register side
		// effects will be dispatched.
		while (write_ring_tail != write_ring_head) {
			const uint8_t t = write_ring_tail;
			core1_last_write_loc = write_ring[t].location;
			write_ring_tail = (t + 1u) & WRITE_RING_MASK;
			switch (write_ring[t].location) {
			case VID_DATA_OFFSET:
			case VID_STAT_OFFSET:
				vid_link_guest_write(write_ring[t].location,
				    write_ring[t].value);
				break;
			default:
				break;
			}
		}

		// Drain guest reads. Reserved for auto-clearing status registers.
		if (read_irq_received) {
			read_irq_received = false;
			core1_last_read_loc = read_location;
			switch (read_location) {
			default:	// link pops happen in the read ISR itself
				break;
			}
		}

		// Keep the guest-visible registers current. Rendering the command
		// stream happens on Core 0: this core carries the bus interrupts
		// and the scanline deadline, and must stay resident and bounded.
		vid_link_stage();
	}
}

static void
bus_pio_dma_init()
{
	// Bus pins — all inputs. E/Q are supplied by the host and sampled by the
	// PIO programs via 'wait gpio'; they must be plain GPIO inputs (the Pico
	// no longer generates them).
	gpio_init(GPIO_E);
	gpio_set_dir(GPIO_E, GPIO_IN);
	gpio_init(GPIO_Q);
	gpio_set_dir(GPIO_Q, GPIO_IN);

	init_pins_range(GPIO_A_BASE, 3); // A0..2 inputs
	gpio_set_dir_in_masked(0b111u << GPIO_A_BASE);

	gpio_init(GPIO_RW);
	gpio_set_dir(GPIO_RW, GPIO_IN);
	init_pins_range(GPIO_D_BASE, 8); // D0..7 inputs initially (inputs/tri-state)
	gpio_set_dir_in_masked(0b11111111u << GPIO_D_BASE);

	gpio_init(GPIO_CS);
	gpio_set_dir(GPIO_CS, GPIO_IN);

	// DMA for the main MC6809 bus read loop
	if (dma_channel_is_claimed(piodma_read.address_channel)) {
		printf("Core0 DMA channel %d for bus_read address is already claimed\n",
		       piodma_read.address_channel);
		panic("Unable to proceed");
	}
	dma_channel_claim(piodma_read.address_channel);
	if (dma_channel_is_claimed(piodma_read.data_channel)) {
		printf("Core0 DMA channel %d for bus_read data is already claimed\n", piodma_read.data_channel);
		panic("Unable to proceed");
	}
	dma_channel_claim(piodma_read.data_channel);

	// SM: Bus read responder
	// The bus pins span GPIO 23-39, so the PIO GPIO base must be 16 (window
	// HARD-RESET BOTH BUS PIOs FIRST, so bring-up does not depend on what
	// was left behind. A warm reset over SWD -- which is how this board is
	// flashed, many times a day -- does not necessarily return every
	// peripheral to its power-on state, and PIO carries exactly the kind
	// of state that matters here: loaded instruction memory, a GPIO base,
	// running state machines. The SDK's own bookkeeping lives in RAM and
	// IS reinitialised at boot, so after a warm reset the software
	// believes the PIO is pristine while the hardware disagrees. Resetting
	// the blocks makes a warm boot identical to a cold one and costs
	// microseconds, once.
	reset_unreset_block_num_wait_blocking(RESET_PIO0);
	reset_unreset_block_num_wait_blocking(RESET_PIO1);

	// DMA too, and for a nastier reason. The scan-out ping-pong pair runs
	// for ever by design, chained to each other; a warm reset does not stop
	// them, so they carry on reading the framebuffer into a machine that
	// thinks it has just booted. Worse, dma_claim_unused_channel() consults
	// a bitmap in RAM that HAS been reinitialised, so it will happily hand
	// video back the very channels still transferring, which then get
	// reconfigured mid-flight. Nothing before this point uses DMA -- not
	// the USB stack, not stdio -- so this is the right place to insist on a
	// known state.
	reset_unreset_block_num_wait_blocking(RESET_DMA);

	// 16-47). This MUST happen before pio_add_program(): pio_set_gpio_base()
	// refuses once a program is loaded (returns PICO_ERROR_INVALID_STATE and
	// leaves the base at 0), and pio_add_program() relocates the wait-gpio
	// instructions relative to the base as it loads them.
	if (pio_set_gpio_base(piodma_read.pio, 16) != PICO_OK) {
		// Silent and fatal if unchecked: the base stays at 0, every bus
		// pin shifts down by 16, and the state machine simply never
		// sees the bus. Nothing else reports it -- it took a debugger
		// to find last time.
		printf("Core0 read PIO refused GPIO base 16\n");
		panic("Unable to proceed");
	}
	pio_read_offset = pio_add_program(piodma_read.pio, &mc6809_bus_read_program);
	if (pio_read_offset < 0) {
		printf("Core0 insufficient space for bus_read program\n");
		panic("Unable to proceed");
	}
	piodma_read.sm = pio_claim_unused_sm(piodma_read.pio, true);
	if (piodma_read.sm < 0) {
		printf("Core0 unable to claim unused SM for bus_read program\n");
		panic("Unable to proceed");
	}
	mc6809_bus_read_program_init(piodma_read.pio, piodma_read.sm, pio_read_offset);
	printf("Core0 read pio%u sm%d configured\n", PIO_NUM(piodma_read.pio), piodma_read.sm);
	// DMA move the requested memory data to PIO for output
	dma_channel_config read_out_dma = dma_channel_get_default_config(piodma_read.data_channel);
	channel_config_set_high_priority(&read_out_dma, true);
	channel_config_set_dreq(&read_out_dma, pio_get_dreq(piodma_read.pio, piodma_read.sm, true));
	channel_config_set_read_increment(&read_out_dma, false);
	channel_config_set_write_increment(&read_out_dma, false);
	channel_config_set_transfer_data_size(&read_out_dma, DMA_SIZE_8);
	channel_config_set_chain_to(&read_out_dma, piodma_read.address_channel);
	dma_channel_configure(
		piodma_read.data_channel,
		&read_out_dma,
		&piodma_read.pio->txf[piodma_read.sm],	// dst
		nullptr,				// src - will be overwritten by the below DMA
		dma_encode_transfer_count(1),
		false);
	printf("Core0 DMA read_out channel configured\n");
	// DMA move address from PIO into the data DMA config
	dma_channel_config read_address_dma = dma_channel_get_default_config(piodma_read.address_channel);
	channel_config_set_high_priority(&read_address_dma, true);
	channel_config_set_dreq(&read_address_dma, pio_get_dreq(piodma_read.pio, piodma_read.sm, false));
	channel_config_set_read_increment(&read_address_dma, false);
	channel_config_set_write_increment(&read_address_dma, false);
	channel_config_set_transfer_data_size(&read_address_dma, DMA_SIZE_32);
	channel_config_set_chain_to(&read_address_dma, piodma_read.data_channel);
	dma_channel_configure(
		piodma_read.address_channel,
		&read_address_dma,
		&dma_channel_hw_addr(piodma_read.data_channel)->read_addr,	// dst
		&piodma_read.pio->rxf[piodma_read.sm],				// src
		dma_encode_transfer_count(1),
		false);
	printf("Core0 DMA read_address channel configured\n");

	// SM: Bus write responder
	// Base 16 before the program is loaded, as for the read PIO above.
	if (pio_set_gpio_base(piodma_write.pio, 16) != PICO_OK) {
		printf("Core0 write PIO refused GPIO base 16\n");
		panic("Unable to proceed");
	}
	pio_write_offset = pio_add_program(piodma_write.pio, &mc6809_bus_write_program);
	if (pio_write_offset < 0) {
		printf("Core0 insufficient space for bus_write program\n");
		panic("Unable to proceed");
	}
	piodma_write.sm = pio_claim_unused_sm(piodma_write.pio, true);
	if (piodma_write.sm < 0) {
		printf("Core0 unable to claim unused SM for bus_write program\n");
		panic("Unable to proceed");
	}
	mc6809_bus_write_program_init(piodma_write.pio, piodma_write.sm, pio_write_offset);
	printf("Core0 write pio%u sm%d configured\n", PIO_NUM(piodma_write.pio), piodma_write.sm);
	// DMA for the main MC6809 bus write loop
	if (dma_channel_is_claimed(piodma_write.address_channel)) {
		printf("Core0 DMA channel %d for bus_write address is already claimed\n",
		       piodma_write.address_channel);
		panic("Unable to proceed");
	}
	dma_channel_claim(piodma_write.address_channel);
	if (dma_channel_is_claimed(piodma_write.data_channel)) {
		printf("Core0 DMA channel %d for bus_write data is already claimed\n",
		       piodma_write.data_channel);
		panic("Unable to proceed");
	}
	dma_channel_claim(piodma_write.data_channel);
	dma_channel_config write_in_dma = dma_channel_get_default_config(piodma_write.data_channel);
	channel_config_set_high_priority(&write_in_dma, true);
	channel_config_set_dreq(&write_in_dma, pio_get_dreq(piodma_write.pio, piodma_write.sm, false));
	channel_config_set_read_increment(&write_in_dma, false);
	channel_config_set_write_increment(&write_in_dma, false);
	channel_config_set_transfer_data_size(&write_in_dma, DMA_SIZE_8);
	channel_config_set_chain_to(&write_in_dma, piodma_write.address_channel);
	dma_channel_configure(
		piodma_write.data_channel,
		&write_in_dma,
		nullptr,					// dst - will be overwritten by the below DMA
		&piodma_write.pio->rxf[piodma_write.sm],	// src
		dma_encode_transfer_count(1),
		false);
	printf("Core0 DMA write_in channel configured\n");
	// DMA move address from PIO into the write data DMA config
	dma_channel_config write_address_dma = dma_channel_get_default_config(piodma_write.address_channel);
	channel_config_set_high_priority(&write_address_dma, true);
	channel_config_set_dreq(&write_address_dma, pio_get_dreq(piodma_write.pio, piodma_write.sm, false));
	channel_config_set_read_increment(&write_address_dma, false);
	channel_config_set_write_increment(&write_address_dma, false);
	channel_config_set_transfer_data_size(&write_address_dma, DMA_SIZE_32);
	channel_config_set_chain_to(&write_address_dma, piodma_write.data_channel);
	dma_channel_configure(
		piodma_write.address_channel,
		&write_address_dma,
		&dma_channel_hw_addr(piodma_write.data_channel)->write_addr,	// dst
		&piodma_write.pio->rxf[piodma_write.sm],			// src
		dma_encode_transfer_count(1),
		false);
	printf("Core0 DMA write_address channel configured\n");
}

// Bring the peripheral up: the host-facing discrete signals, the bus PIO/DMA
// engine, a cleared register window, and Core 1.
void
bus_engine_start(void)
{
	// !IRQ is a simulated open-collector output: latch low, then release to
	// high impedance until a device needs to interrupt the host.
	gpio_init(GPIO_IRQ);
	gpio_put(GPIO_IRQ, false);
	gpio_set_drive_strength(GPIO_IRQ, GPIO_DRIVE_STRENGTH_8MA);
	DEASSERT_IRQ;

	// !RESET is an input: the host drives it, we observe it. Pull up so a
	// disconnected line reads released (the signal is active-low).
	gpio_init(GPIO_RESET);
	gpio_set_dir(GPIO_RESET, GPIO_IN);
	// gpio_pull_up(GPIO_RESET);

	bus_pio_dma_init();
	peripheral_clear();
	printf("Core0 register window initialised\n");

	multicore_launch_core1(main_core1);
}


/* Drive the host !IRQ line on behalf of the portable link code (see
 * vid_link.h). Simulated open collector: becoming an output drives the
 * pre-loaded 0 onto the line, becoming an input releases it to the host's
 * pull-up. Reached from Core 1's staging pass, hence RAM-resident -- and only
 * ever on a change of state, so this is not in the per-pass cost. */
extern "C" void
__not_in_flash_func(vid_link_irq_out)(bool assert)
{
	if (assert)
		ASSERT_IRQ;
	else
		DEASSERT_IRQ;
}

/* ---- Status, for the console -------------------------------------------- */

bool
bus_engine_serving(void)
{
	return core1_initialised;
}

uint32_t
bus_engine_loop_count(void)
{
	return core1_loop_count;
}

void
bus_engine_print_status(void)
{
	// Cycles, with microseconds alongside because the deadline that
	// matters -- the scan-out DMA's re-arm window -- is quoted in them.
	const uint32_t hz = clock_get_hz(clk_sys) / 1000000u;
	// Two lines, and read the RATE not the maximum when comparing runs:
	// the maximum latches since boot and can only grow, so it flatters a
	// short run against a long one.
	struct { const char *what; uint32_t mx, tot, over, sum; } isr[2] = {
		{ "read ", isr_read_max,  isr_read_calls,  isr_read_over,  isr_read_total  },
		{ "write", isr_write_max, isr_write_calls, isr_write_over, isr_write_total },
	};
	for (int i = 0; i < 2; i++) {
		const uint32_t n = isr[i].tot;
		const uint32_t ppm = n ? (uint32_t)(((uint64_t)isr[i].over *
		    1000000u) / n) : 0u;
		printf("Bus ISR %s: max %lu cy (%lu.%02lu us)  avg %lu cy  "
		       "over %uns: %lu/%lu (%lu ppm)\n",
		       isr[i].what,
		       (unsigned long)isr[i].mx,
		       (unsigned long)(isr[i].mx / hz),
		       (unsigned long)((isr[i].mx * 100u / hz) % 100u),
		       (unsigned long)(n ? isr[i].sum / n : 0u),
		       ISR_TAIL_NS,
		       (unsigned long)isr[i].over, (unsigned long)n,
		       (unsigned long)ppm);
	}
	// Silence is the expected reading, so this line only appears when it
	// is not silent. A stale service is not a margin to watch, it is a
	// byte the guest was handed twice.
	if (isr_read_stale != 0u)
		printf("Bus ISR read : STALE SERVICE x%lu — closest pair %lu cy"
		       " apart; the guest was served a byte twice\n",
		       (unsigned long)isr_read_stale,
		       (unsigned long)isr_read_stale_worst);
	printf("Last write:  %02X  last read: %02X\n",
	       core1_last_write_loc, core1_last_read_loc);
	printf("Guest reads: FF10=%lu FF11=%lu (others %lu)\n",
	       (unsigned long)read_offset_count[0], (unsigned long)read_offset_count[1],
	       (unsigned long)(read_offset_count[2] + read_offset_count[3] +
	           read_offset_count[4] + read_offset_count[5] +
	           read_offset_count[6] + read_offset_count[7]));
}

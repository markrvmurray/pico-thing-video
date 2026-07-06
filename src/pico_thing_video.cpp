/**
* Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pico2 (RP2350) firmware acting as a slave peripheral on an external
 * MC6809E bus. The host system supplies the E/Q clocks and drives the
 * address/data bus; this Pico watches its !CS and services byte reads and
 * writes to an 8-byte register window at $FF10. A video processor and USB
 * host (keyboard/mouse) will be layered on top of this window later.
 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "pico/aon_timer.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#include "tusb.h"
#include "usb.h"
#include "registers.h"
#include "pico_thing_video.h"
#include "build_time.h"
#include "command_parser.h"

/* Flex/bison generated scanner API (yy_buffer_state is flex's internal type) */
extern "C" {
struct yy_buffer_state;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
extern YY_BUFFER_STATE cmd_yy_scan_string(const char *);
extern void            cmd_yy_delete_buffer(YY_BUFFER_STATE);
extern int             cmd_yyparse(parsed_cmd_t *);
extern void            cmd_yy_reset_str_pool(void);
}

// PIO program headers generated from the .pio file
#include "pico_thing_video.pio.h"

#define SYS_CLK_DEFAULT 320000000

// Default pico uart for command-line interactive access
#define CONSOLE uart0
#define CONSOLE_BAUDRATE 115200

#define COMMAND_BUFFER_LEN 256u

volatile bool verbose = false;

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

// ISR for the R/!W bus read loop and DMA chain.
// The read DMA has already handed read_registers[addr] to the host; we only
// record which location was read for optional Core 1 side effects.
void __isr
__time_critical_func(dma_bus_read_irq_handler())
{
	dma_channel_acknowledge_irq0(piodma_read.data_channel);
	core1_isr_count++;
	const uintptr_t read_addr = dma_channel_hw_addr(piodma_read.data_channel)->read_addr;
	read_location = read_addr & registers::register_mask;
	read_irq_received = true;
}

// ISR for the R/!W bus write loop and DMA chain.
// The write DMA has stored the guest byte into write_registers[addr]. Mirror it
// into the read side so the host reads back what it wrote, then queue it for
// Core 1 to dispatch any device side effects.
void __isr
__time_critical_func(dma_bus_write_irq_handler())
{
	dma_channel_acknowledge_irq1(piodma_write.data_channel);
	core1_isr_count++;
	const uintptr_t written_addr = dma_channel_hw_addr(piodma_write.data_channel)->write_addr;
	const uint8_t wloc = written_addr & registers::register_mask;
	const uint8_t wval = *reinterpret_cast<volatile uint8_t *>(written_addr);
	// Mirror into the read side (scratch-register behaviour for the whole
	// window). Device sub-registers with side effects override this in the
	// Core 1 dispatch loop.
	reg[wloc] = wval;
	// Queue for Core 1.
	const uint8_t h = write_ring_head;
	const uint8_t next_h = (h + 1u) & WRITE_RING_MASK;
	if (next_h == write_ring_tail)
		write_ring_tail = (write_ring_tail + 1u) & WRITE_RING_MASK;
	write_ring[h].location = wloc;
	write_ring[h].value = wval;
	write_ring_head = next_h;
}

static void
peripheral_clear()
{
	registers::clear();
}

static void
print_time()
{
	tm tm{};
	if (aon_timer_get_time_calendar(&tm)) // gives POSIX timespec
		printf("%04d-%02d-%02d %02d:%02d:%02d UTC\n",
		       tm.tm_year + 1900,
		       tm.tm_mon + 1,
		       tm.tm_mday,
		       tm.tm_hour,
		       tm.tm_min,
		       tm.tm_sec);
	else
		printf("Unable to get calendar time\n");
}

/* cmd_parse() - glue between process_command() and the flex/bison parser */
bool
cmd_parse(char *input, parsed_cmd_t *out)
{
	out->tag = CMD_NONE;
	cmd_yy_reset_str_pool();
	YY_BUFFER_STATE buf = cmd_yy_scan_string(input);
	int rc = cmd_yyparse(out);
	cmd_yy_delete_buffer(buf);
	/* YYERROR in grammar actions does not invoke yyerror(), so cmd.tag may
	 * remain CMD_NONE on failure.  Normalise to CMD_SYNTAX_ERROR. */
	if (rc != 0 && out->tag == CMD_NONE)
		out->tag = CMD_SYNTAX_ERROR;
	return rc == 0;
}

static void set_power();
static void reset_power();

// Push a single {location, value} write into the ring so the Core 1 dispatch
// loop treats a console modify the same as a guest bus write.
static void
inject_write(uint8_t loc, uint8_t val)
{
	const uint8_t h = write_ring_head;
	const uint8_t next_h = (h + 1u) & WRITE_RING_MASK;
	if (next_h == write_ring_tail)
		write_ring_tail = (write_ring_tail + 1u) & WRITE_RING_MASK;
	write_ring[h].location = loc;
	write_ring[h].value = val;
	write_ring_head = next_h;
}

static void
process_command(char *buf)
{
	uint8_t buffer[64];
	parsed_cmd_t cmd;

	/* Skip blank lines */
	while (isspace(static_cast<unsigned char>(*buf)))
		buf++;
	if (*buf == '\0')
		return;

	if (!cmd_parse(buf, &cmd))
		return; /* error message already printed by parser */

	switch (cmd.tag) {

	case CMD_VERBOSE:
		verbose = !verbose;
		printf("Verbose mode %s\n", verbose ? "on" : "off");
		break;

	case CMD_STATUS:
		printf("Verbose:     %s\n", verbose ? "on" : "off");
		printf("!RESET:      %s\n", RESET_IS_ASSERTED ? "asserted" : "released");
		printf("!IRQ:        %s\n", IRQ_IS_ASSERTED ? "asserted" : "released");
		printf("Core1 loop:  %lu  isr: %lu\n",
		       (unsigned long)core1_loop_count, (unsigned long)core1_isr_count);
		printf("Last write:  %02X  last read: %02X\n",
		       core1_last_write_loc, core1_last_read_loc);
		registers::dump();
		break;

	case CMD_EXAMINE:
		registers::dump();
		break;

	case CMD_MODIFY: {
		const uint8_t off   = cmd.modify.start & registers::register_mask;
		const uint8_t count = cmd.modify.count;
		for (uint8_t i = 0; i < count; i++) {
			const uint8_t loc = (off + i) & registers::register_mask;
			const uint8_t val = cmd.modify.data[i];
			registers::write(loc) = val;	// write side
			reg[loc] = val;			// mirror to read side
			inject_write(loc, val);		// let Core 1 dispatch see it
			printf("%02X: %02X\n", loc, val);
		}
		break;
	}

	case CMD_FILL: {
		const uint8_t val = cmd.fill.value;
		for (uint16_t i = 0u; i < registers::register_length; i++) {
			registers::write(i) = val;
			reg[i] = val;
		}
		printf("Filled register window with %02X\n", val);
		break;
	}

	case CMD_USB: {
		/* Send each space-separated word as a separate USB CDC write.
		 * Optional port prefix: "usb 2 ..." sends to CDC interface 1. */
		char args[sizeof(cmd.usb.args)];
		memcpy(args, cmd.usb.args, sizeof(args));
		char *ptr = args;
		uint8_t itf = 0;
		if (ptr[0] == '2' && (ptr[1] == ' ' || ptr[1] == '\t')) {
			itf = 1;
			ptr += 2;
		}
		char *word;
		while ((word = strsep(&ptr, " \t")) != nullptr) {
			if (*word == '\0')
				continue;
			snprintf(reinterpret_cast<char *>(buffer), sizeof(buffer), "%s\r\n", word);
			usb_cdc_write_n(itf, buffer, strlen(reinterpret_cast<char *>(buffer)));
		}
		usb_cdc_flush_n(itf);
		break;
	}

	default:
		break;
	}
}

static constexpr char prompt[] = ">>> ";

static void
poll_console()
{
	static char buf[COMMAND_BUFFER_LEN];
	static int idx = 0;
	static bool prompted = false;

	while (!core1_initialised) {
		sleep_ms(1u);
		usb_poll();
	}

	print_time();

	for (;;) {
		if (!prompted) {
			printf(prompt);
			prompted = true;
		}
		// Poll USB before reading a character so that all chars delivered by
		// this tud_task() call are visible to getchar_timeout_us().
		usb_poll();
		const int ch = getchar_timeout_us(0);
		if (ch != PICO_ERROR_TIMEOUT) {
			if (ch == '\r' || ch == '\n') {
				putchar('\n');
				buf[idx] = '\0';
				process_command(buf);
				idx = 0;
				prompted = false;
			} else if (ch == '\b' || ch == 0x7F) {
				if (idx > 0) {
					putchar('\b');
					putchar(' ');
					putchar('\b');
					buf[--idx] = '\0';
				}
			} else if (idx < static_cast<int>(COMMAND_BUFFER_LEN) - 1 &&
				   ch >= ' ' && ch < 0x7F) {
				putchar(ch);
				buf[idx++] = ch;
			}
		}
	}
}

void
init_pins_range(const uint8_t base, const uint8_t count)
{
	for (uint8_t i = 0; i < count; ++i)
		gpio_init(base + i);
}

static void
__no_inline_not_in_flash_func(main_core1)()
{
	printf("Core1 process starting\n");

	irq_set_exclusive_handler(piodma_read.irq, dma_bus_read_irq_handler);
	irq_set_priority(piodma_read.irq, 0);
	irq_set_enabled(piodma_read.irq, true);
	dma_channel_set_irq0_enabled(piodma_read.data_channel, true);
	dma_channel_start(piodma_read.address_channel);
	pio_sm_put(piodma_read.pio, piodma_read.sm, registers::read_address() >> 3); // Three bits for A0-2
	printf("Core1 read DMA chain configured\n");

	irq_set_exclusive_handler(piodma_write.irq, dma_bus_write_irq_handler);
	irq_set_priority(piodma_write.irq, 0);
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

	printf("Core1 process started\n");
	core1_initialised = true;

	bool reset_was_asserted = false;

	for (;;) {
		core1_loop_count++;

		// Observe host !RESET. On the asserting edge, clear the register
		// window so the peripheral presents a clean slate to the host.
		const bool reset_now = RESET_IS_ASSERTED;
		if (reset_now && !reset_was_asserted)
			registers::clear();
		reset_was_asserted = reset_now;

		// Drain guest writes. The byte was already mirrored to the read side
		// in the ISR; this loop is where video/keyboard/mouse register side
		// effects will be dispatched.
		while (write_ring_tail != write_ring_head) {
			const uint8_t t = write_ring_tail;
			core1_last_write_loc = write_ring[t].location;
			write_ring_tail = (t + 1u) & WRITE_RING_MASK;
			switch (write_ring[t].location) {
			default:
				break;
			}
		}

		// Drain guest reads. Reserved for auto-clearing status registers.
		if (read_irq_received) {
			read_irq_received = false;
			core1_last_read_loc = read_location;
			switch (read_location) {
			default:
				break;
			}
		}
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

#define MEASURE_FREQS
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

static void
pio_shutdown()
{
	// --- Disable IRQs first (handlers reference DMA/PIO resources) ---

	// DMA read IRQ
	dma_channel_set_irq0_enabled(piodma_read.data_channel, false);
	irq_set_enabled(piodma_read.irq, false);
	irq_remove_handler(piodma_read.irq, dma_bus_read_irq_handler);

	// DMA write IRQ
	dma_channel_set_irq1_enabled(piodma_write.data_channel, false);
	irq_set_enabled(piodma_write.irq, false);
	irq_remove_handler(piodma_write.irq, dma_bus_write_irq_handler);

	// --- Bus write SM + DMA ---
	dma_channel_abort(piodma_write.address_channel);
	dma_channel_abort(piodma_write.data_channel);
	dma_channel_unclaim(piodma_write.address_channel);
	dma_channel_unclaim(piodma_write.data_channel);
	mc6809_bus_write_program_deinit(piodma_write.pio, piodma_write.sm);
	pio_remove_program(piodma_write.pio, &mc6809_bus_write_program, pio_write_offset);

	// --- Bus read SM + DMA ---
	dma_channel_abort(piodma_read.address_channel);
	dma_channel_abort(piodma_read.data_channel);
	dma_channel_unclaim(piodma_read.address_channel);
	dma_channel_unclaim(piodma_read.data_channel);
	mc6809_bus_read_program_deinit(piodma_read.pio, piodma_read.sm);
	pio_remove_program(piodma_read.pio, &mc6809_bus_read_program, pio_read_offset);
}

static void
set_power()
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

static void
reset_power()
{
	// Kill Core 1 before touching any resources it uses
	core1_initialised = false;
	multicore_reset_core1();
	printf("Core0 core1 stopped\n");

	// Now safe to tear down IRQs, DMA and PIO
	pio_shutdown();
	printf("Core0 PIO/DMA disabled\n");

	// Release the interrupt line and remove power
	gpio_put(GPIO_IRQ, false);
	DEASSERT_IRQ;
	printf("Core0 powered down\n");
}

int
main()
{
	vreg_set_voltage(VREG_VOLTAGE_2_00);
	sleep_ms(5);

	gpio_set_function(GPIO_UART_TX, UART_FUNCSEL_NUM(uart0, GPIO_UART_TX));
	gpio_set_function(GPIO_UART_RX, UART_FUNCSEL_NUM(uart0, GPIO_UART_RX));

	stdio_init_all();

	printf("\n\nPico2 MC6809E bus peripheral %s\n", DEVICE_VERSION);
	printf("Pico2 MC6809E built on %s %s\n", __DATE__, __TIME__);

#ifdef MEASURE_FREQS
	measure_freqs();
#endif
	printf("Core0 raising sys_clock to %d MHz ... ", SYS_CLK_DEFAULT/1000000);
	if (!set_sys_clock_hz(SYS_CLK_DEFAULT, false)) {
		printf("unsuccessful! Reverting to %d MHz ... ", SYS_CLK_HZ/1000000);
		set_sys_clock_hz(SYS_CLK_HZ, true);
	}
	stdio_init_all();
	uart_init(CONSOLE, CONSOLE_BAUDRATE);
	printf("successful!\n");
	printf("Core0 console baudrate set to %d\n", CONSOLE_BAUDRATE);

	usb_init();
	printf("Core0 initialised USB\n");

	tm tm = build_time_tm();
	aon_timer_stop();
	aon_timer_start_calendar(&tm);

	printf("Core0 ready!\n");

	// Wait for Core 1's main loop to be running before entering the console.
	while (core1_loop_count < 100u) {
		sleep_ms(1u);
		usb_poll();
	}

	poll_console();
}

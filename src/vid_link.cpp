/* vid_link.cpp – FreeBSD style */

#include <stdio.h>

#include "pico/stdlib.h"

#include "registers.h"
#include "vid_link.h"

/* Command FIFO (6809 -> us). The register contract promises TDRE set means
 * the next single Data write is accepted; 2 KB is the advertised minimum
 * depth. Only the guest produces and only Core 1 consumes, so TDRE staged
 * from "free >= 1" can never be invalidated by another producer. */
#define TX_FIFO_SIZE  2048u	/* power of two */
#define TX_FIFO_MASK  (TX_FIFO_SIZE - 1u)

/* Input FIFO (us -> 6809). Loopback feeds it today; the USB HID event
 * stream (M4, produced on Core 0) will feed it later, so head/tail are
 * kept SPSC-safe across cores. */
#define RX_FIFO_SIZE  256u	/* power of two */
#define RX_FIFO_MASK  (RX_FIFO_SIZE - 1u)

static uint8_t           tx_fifo[TX_FIFO_SIZE];
static volatile uint16_t tx_head, tx_tail;	/* head: producer, tail: consumer */

static uint8_t           rx_fifo[RX_FIFO_SIZE];
static volatile uint16_t rx_head, rx_tail;

static bool     ready;
static bool     loopback;

/* Diagnostics for the console 'status' command. */
static volatile uint32_t stat_tx_accepted;	/* bytes taken off the bus */
static volatile uint32_t stat_tx_consumed;	/* bytes eaten by the consumer */
static volatile uint32_t stat_rx_pushed;	/* bytes accepted into the input FIFO */
static volatile uint32_t stat_rx_popped;	/* Data reads that dequeued */
static volatile uint32_t stat_rx_overruns;	/* loopback pushes dropped */
static volatile uint8_t  stat_last_byte;

extern registers &reg;

static inline uint16_t
tx_level(void)
{
	return (uint16_t)(tx_head - tx_tail);
}

static inline uint16_t
rx_level(void)
{
	return (uint16_t)(rx_head - rx_tail);
}

static void
rx_push(uint8_t b)
{
	if (rx_level() >= RX_FIFO_SIZE) {
		stat_rx_overruns = stat_rx_overruns + 1u;
		return;
	}
	rx_fifo[rx_head & RX_FIFO_MASK] = b;
	rx_head = rx_head + 1u;
	stat_rx_pushed = stat_rx_pushed + 1u;
}

/* Restage the guest-visible Data and Stat bytes from FIFO state. Idempotent;
 * run every loop pass so console pokes or a window clear self-heal. Also run
 * from the read ISR after a pop, so the very next guest read is coherent —
 * hence RAM-resident. */
static void
__not_in_flash_func(restage)(void)
{
	uint8_t st = VID_STAT_VERSION << VID_STAT_VER_LSB;

	if (rx_level() > 0u) {
		st |= VID_STAT_RDRF;
		reg[VID_DATA_OFFSET] = rx_fifo[rx_tail & RX_FIFO_MASK];
	}
	if (tx_level() < TX_FIFO_SIZE)
		st |= VID_STAT_TDRE;
	if (ready)
		st |= VID_STAT_READY;
	reg[VID_STAT_OFFSET] = st;
}

/* Set by a reset on the bus core, acted on by the render core: re-arming the
 * renderer means touching the framebuffer, which only the render core may do. */
static volatile bool consume_reset_pending;

extern "C" void
vid_link_reset(void)
{
	/* Empty both rings by moving only the index this core owns: it
	 * produces into tx and consumes from rx, so tx_head and rx_tail are
	 * both safe to write here while the other core works the far ends. */
	tx_head = tx_tail;
	rx_tail = rx_head;
	consume_reset_pending = true;
	restage();
}

extern "C" void
vid_link_init(void)
{
	loopback = false;
	ready = true;
	vid_link_reset();
}

extern "C" void
vid_link_input(uint8_t byte)
{
	/* Core 0 producer: push only — Core 1's next task pass restages
	 * Data/Stat (sub-microsecond), keeping all reg[] writes on one core. */
	rx_push(byte);
}

extern "C" void
__not_in_flash_func(vid_link_guest_write)(uint8_t location, uint8_t value)
{
	switch (location) {
	case VID_DATA_OFFSET:
		/* TDRE promised acceptance; a full FIFO here means the guest
		 * wrote without polling — drop, as agreed (graceful, no hang). */
		if (tx_level() < TX_FIFO_SIZE) {
			tx_fifo[tx_head & TX_FIFO_MASK] = value;
			tx_head = tx_head + 1u;
			stat_tx_accepted = stat_tx_accepted + 1u;
		}
		break;
	case VID_STAT_OFFSET:
		loopback = (value & VID_CTL_LOOPBACK) != 0u;
		if (value & VID_CTL_RESET)
			vid_link_reset();
		break;
	default:
		break;
	}
}

extern "C" void
__not_in_flash_func(vid_link_guest_read)(uint8_t location)
{
	/* Called from the read ISR: the DMA already served the staged byte;
	 * advance behind it and restage immediately, so the guest's next
	 * Stat/Data read is coherent even if the loop is held off. Every
	 * Data read gets its own ISR invocation at 6809 bus rates, so pops
	 * cannot be lost or double-counted. */
	if (location == VID_DATA_OFFSET && rx_level() > 0u) {
		rx_tail = rx_tail + 1u;
		stat_rx_popped = stat_rx_popped + 1u;
		restage();
	}
}

extern "C" void
__not_in_flash_func(vid_link_stage)(void)
{
	/* Bus core: refresh what the guest sees, nothing more. */
	restage();
}

extern "C" void
vid_link_render(void)
{
	/* Render core. Takes as long as it takes; the guest is flow-controlled
	 * by TDRE and the bus core keeps the registers current meanwhile. */
	if (consume_reset_pending) {
		consume_reset_pending = false;
		vid_link_consume_reset();
	}

	/* Drain the command FIFO. Loopback (M0 test mode) diverts bytes to
	 * the input FIFO; otherwise they feed the console layer. */
	bool consumed = false;

	if (tx_tail != tx_head && !loopback)
		vid_link_consume_begin();

	while (tx_tail != tx_head) {
		const uint8_t b = tx_fifo[tx_tail & TX_FIFO_MASK];
		tx_tail = tx_tail + 1u;
		stat_tx_consumed = stat_tx_consumed + 1u;
		stat_last_byte = b;
		if (loopback) {
			rx_push(b);
		} else {
			vid_link_consume(b);
			consumed = true;
		}
	}
	if (consumed)
		vid_link_consume_flush();
}

extern "C" void
vid_link_print_status(void)
{
	printf("Link:        %s%s, ver %u; tx acc %lu cons %lu, last %02X\n",
	       ready ? "ready" : "down",
	       loopback ? ", loopback" : "",
	       (unsigned)VID_STAT_VERSION,
	       (unsigned long)stat_tx_accepted, (unsigned long)stat_tx_consumed,
	       stat_last_byte);
	printf("Input:       recv %lu pop %lu pending %u ovr %lu; RDRF %s\n",
	       (unsigned long)stat_rx_pushed, (unsigned long)stat_rx_popped,
	       (unsigned)rx_level(),
	       (unsigned long)stat_rx_overruns,
	       (reg[VID_STAT_OFFSET] & VID_STAT_RDRF) ? "set" : "clear");
}

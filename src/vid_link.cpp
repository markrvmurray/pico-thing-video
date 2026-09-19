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

extern "C" void
vid_link_reset(void)
{
	tx_tail = tx_head;
	rx_tail = rx_head;
	vid_link_consume_reset();
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
__not_in_flash_func(vid_link_task)(void)
{
	/* Drain the command FIFO. Loopback (M0 test mode) diverts bytes to
	 * the input FIFO; otherwise they feed the console layer. */
	bool consumed = false;

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
	restage();
}

extern "C" void
vid_link_print_status(void)
{
	printf("Link:        %s%s, ver %u; tx acc %lu cons %lu, rx pop %lu ovr %lu, last %02X\n",
	       ready ? "ready" : "down",
	       loopback ? ", loopback" : "",
	       (unsigned)VID_STAT_VERSION,
	       (unsigned long)stat_tx_accepted, (unsigned long)stat_tx_consumed,
	       (unsigned long)stat_rx_popped, (unsigned long)stat_rx_overruns,
	       stat_last_byte);
}

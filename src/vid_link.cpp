/* vid_link.cpp – FreeBSD style */

#include <atomic>
#include <stdio.h>

#include "pico/stdlib.h"

#include "registers.h"
#include "video.h"	/* FB_WIDTH/HEIGHT, TEXT_COLS/ROWS */
#include "vid_link.h"
#include "cmd_trace.h"

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
static bool     rie;		/* receive interrupt enable (Ctl bit 7) */
static bool     irq_asserted;	/* what we last told the platform */

/* ── Mouse ───────────────────────────────────────────────────────────────
 * Live state belongs to the render core (the HID cooker updates it); the
 * snapshot, its index and the guest-visible register belong to the bus core,
 * keeping to the one-writer-per-field rule the link relies on elsewhere.
 *
 * The bus core never writes the live state. "Cleared on latch" is therefore
 * implemented the other way round: the bus core bumps a generation counter
 * when it latches, and the render core clears its own accumulators when it
 * notices the generation move. Nothing crosses.
 *
 * The position pair is published under a seqlock so the bus core cannot latch
 * a half-updated coordinate; everything else is a single byte and atomic in
 * its own right. */
static volatile uint32_t ms_seq;		/* odd while the pair is in flux */
static volatile int      ms_x, ms_y;		/* render core owns */
static volatile uint8_t  ms_buttons;
static volatile uint8_t  ms_sticky;		/* presses since the last latch */
static volatile int8_t   ms_wheel;		/* notches since the last latch */
static volatile uint32_t ms_latch_gen;		/* bus core bumps on latch */
static volatile bool     ms_changed;		/* drives VID.Stat bit 6 */

/* TWO INVARIANTS THE GUEST DRIVER RELIES ON. Its poll skips the seven reads
 * entirely when VID.Stat bit 6 is clear, which is only safe because:
 *
 *   1. bit 6 is clear ONLY when the mouse is genuinely idle. ms_changed is
 *      set by EVERY call to vid_link_mouse_update(), and the HID cooker calls
 *      it on every report -- a press included. Making that conditional (on
 *      movement, say) would let a press hide behind a clear bit 6, and the
 *      driver would never come to collect it.
 *
 *   2. An edge survives any number of skipped polls. The press and wheel
 *      accumulators clear when a PASS STARTS, never when a latch happens, so
 *      a click falling entirely inside a tick the driver skipped is still
 *      waiting for the next pass that runs.
 *
 * Both are load-bearing for SS.MsSig on the 6809 side. Breaking either one
 * loses clicks silently and intermittently -- the worst way to find out. */

static uint8_t  ms_snapshot[VID_MOUSE_SNAPSHOT_LEN];   /* bus core only */
static uint8_t  ms_index;

/* Bus core: fold the live state into the snapshot.
 *
 * Position is copied; the edge information ACCUMULATES. Latching may happen
 * several times before the guest looks, so presses and wheel notches are OR'd
 * and summed into accumulators rather than replacing what is already there —
 * otherwise a second latch before the read would erase the press the first one
 * captured, which is the very thing byte 1 exists to prevent. The accumulators
 * are cleared when the guest STARTS a pass, not when we latch. */
static uint8_t ms_sticky_acc;
static int     ms_wheel_acc;
static volatile bool ms_dirty;		/* snapshot is behind the live state */
static volatile bool ms_held;		/* guest latched explicitly; hold it */

static void
__not_in_flash_func(ms_latch)(void)
{
	/* ONE ATTEMPT, NEVER A RETRY LOOP. This runs on the bus core, reachable
	 * from the read ISR, which owes the scan-out DMA a re-arm inside about
	 * 5 us and owes the bus its next service at 6809 rates. Spinning until
	 * the render core finishes publishing would wait not on its handful of
	 * instructions but on whatever interrupts it midway -- USB, the ADC
	 * pass -- which is no bound at all.
	 *
	 * So: read the pair, and if it was torn, KEEP THE PREVIOUS SNAPSHOT and
	 * leave. ms_dirty stays set, so the next pass tries again and the data
	 * is at worst one pass stale. A mouse sample a pass old is worth
	 * incomparably more than an unbounded stall in an interrupt handler. */
	const uint32_t s0 = ms_seq;
	const int x = ms_x;
	const int y = ms_y;
	if ((s0 & 1u) != 0u || s0 != ms_seq)
		return;			/* torn; previous snapshot stands */

	ms_sticky_acc = (uint8_t)(ms_sticky_acc | ms_sticky);
	ms_wheel_acc += ms_wheel;
	if (ms_wheel_acc > 127) ms_wheel_acc = 127;
	if (ms_wheel_acc < -128) ms_wheel_acc = -128;

	/* Tell the render core those contributions have been taken; it clears
	 * its own copies, so nothing is written across the core boundary. */
	ms_latch_gen = ms_latch_gen + 1u;

	ms_snapshot[0] = ms_buttons;
	ms_snapshot[1] = ms_sticky_acc;
	ms_snapshot[2] = (uint8_t)(x >> 8);	/* big-endian, as the 6809 reads */
	ms_snapshot[3] = (uint8_t)x;
	ms_snapshot[4] = (uint8_t)(y >> 8);
	ms_snapshot[5] = (uint8_t)y;
	ms_snapshot[6] = (uint8_t)ms_wheel_acc;
	ms_dirty = false;
}

extern "C" void
vid_link_mouse_update(int x, int y, uint8_t buttons, int wheel_delta)
{
	/* Render core. Drop whatever the bus core has already taken before
	 * folding in anything new, so a press can never be discarded between
	 * being recorded and being reported. */
	static uint32_t seen_gen;
	static uint8_t  prev_buttons;

	const uint32_t gen = ms_latch_gen;
	if (gen != seen_gen) {
		seen_gen = gen;
		ms_sticky = 0u;
		ms_wheel = 0;
	}

	ms_seq = ms_seq + 1u;		/* odd: pair in flux */
	ms_x = x;
	ms_y = y;
	ms_seq = ms_seq + 1u;		/* even: pair consistent */

	ms_sticky = (uint8_t)(ms_sticky | (uint8_t)(buttons & ~prev_buttons));
	prev_buttons = buttons;
	ms_buttons = buttons;

	int w = (int)ms_wheel + wheel_delta;
	if (w > 127) w = 127;
	if (w < -128) w = -128;
	ms_wheel = (int8_t)w;

	ms_changed = true;
	ms_dirty = true;
}

extern "C" void
__not_in_flash_func(vid_link_mouse_read)(void)
{
	/* Bus core, from the read ISR: the byte has already gone out, so step
	 * on and stage the next. */
	if (++ms_index >= VID_MOUSE_SNAPSHOT_LEN) {
		ms_index = 0u;
		ms_held = false;
	} else if (ms_index == 1u) {
		/* Byte 0 has just been served, so the guest is committed to this
		 * pass: the sample is taken, and the edges it carries are now
		 * its responsibility. */
		ms_changed = false;
		ms_sticky_acc = 0u;
		ms_wheel_acc = 0;
		ms_held = false;
	}
	reg[VID_MOUSE_OFFSET] = ms_snapshot[ms_index];
}

/* ---- VID.Info: the drawing surface, walked like the mouse snapshot ------ */
static uint8_t info_block[VID_INFO_LEN] = {
	(uint8_t)(FB_WIDTH  >> 8), (uint8_t)FB_WIDTH,
	(uint8_t)(FB_HEIGHT >> 8), (uint8_t)FB_HEIGHT,
	(uint8_t)TEXT_COLS,
	(uint8_t)TEXT_ROWS,
};
static uint8_t info_index;

/* Selecting a window changes what this register describes, and the change
 * arrives from the render core while the bus core is serving reads. Rather
 * than lock, a new geometry waits in a shadow and is adopted only at index 0
 * -- between passes -- so a pass that is under way finishes describing one
 * surface rather than half of two. Same discipline as the mouse snapshot,
 * for the same reason. */
static volatile uint8_t info_shadow[VID_INFO_LEN];
static volatile bool    info_shadow_valid;

extern "C" void
vid_link_info_set(uint16_t width_px, uint16_t height_px,
    uint8_t cols, uint8_t rows)
{
	info_shadow[0] = (uint8_t)(width_px >> 8);
	info_shadow[1] = (uint8_t)width_px;
	info_shadow[2] = (uint8_t)(height_px >> 8);
	info_shadow[3] = (uint8_t)height_px;
	info_shadow[4] = cols;
	info_shadow[5] = rows;
	std::atomic_thread_fence(std::memory_order_release);
	info_shadow_valid = true;
}

static inline void
info_adopt_if_pending(void)
{
	if (!info_shadow_valid)
		return;
	std::atomic_thread_fence(std::memory_order_acquire);
	for (unsigned i = 0u; i < VID_INFO_LEN; i++)
		info_block[i] = info_shadow[i];
	info_shadow_valid = false;
}

extern "C" void
__not_in_flash_func(vid_link_info_read)(void)
{
	/* Bus core, from the read ISR: the byte has gone out, step on and
	 * stage the next. Constant data, so no latching and nothing to hold
	 * consistent -- unlike the mouse, a pass cannot tear. */
	if (++info_index >= VID_INFO_LEN) {
		info_index = 0u;
		info_adopt_if_pending();
	}
	reg[VID_INFO_OFFSET] = info_block[info_index];
}

extern "C" void
__not_in_flash_func(vid_link_info_write)(void)
{
	/* Homing the index is the START of a pass, which is exactly when a
	 * geometry change may be taken up: adopting here means a guest that
	 * selects a window and then reads Info sees the new surface on its
	 * very next pass, rather than one pass later. */
	info_adopt_if_pending();

	/* Any value homes the index, so a pass is self-framing: write, then
	 * read six. Handled synchronously in the write ISR for the same reason
	 * the mouse is -- a guest may read in the very next instruction, which
	 * would race the Core 1 drain. */
	info_index = 0u;
	reg[VID_INFO_OFFSET] = info_block[0];
}

extern "C" void
__not_in_flash_func(vid_link_mouse_write)(void)
{
	/* Bus core, from the write ISR -- SYNCHRONOUSLY, not via the write
	 * ring. A guest doing "sta $FF12 / lda $FF12" back to back would race
	 * Core 1's drain, and the one job of this write is to put the guest in
	 * a known state; it cannot be the thing that is uncertain.
	 *
	 * Any value: take a sample now and home the index. This is what makes
	 * a pass self-framing -- write, then read seven bytes -- so losing
	 * count is not a state the guest can reach, and recovering from one
	 * costs a write rather than the destructive link reset. */
	ms_index = 0u;
	ms_latch();
	ms_held = true;		/* the sample is the guest's, until it reads */
	reg[VID_MOUSE_OFFSET] = ms_snapshot[0];
}

extern "C" bool
vid_link_irq_asserted(void)
{
	return irq_asserted;
}

extern "C" void
vid_link_mouse_print(void)
{
	printf("Mouse:       %d,%d buttons %c%c%c; snapshot byte %u%s\n",
	       ms_x, ms_y,
	       (ms_buttons & VID_MOUSE_BTN_LEFT)   ? 'L' : '-',
	       (ms_buttons & VID_MOUSE_BTN_MIDDLE) ? 'M' : '-',
	       (ms_buttons & VID_MOUSE_BTN_RIGHT)  ? 'R' : '-',
	       (unsigned)ms_index,
	       ms_changed ? ", changed" : "");
}

/* Diagnostics for the console 'status' command. */
static volatile uint32_t stat_tx_accepted;	/* bytes taken off the bus */
static volatile uint32_t stat_tx_consumed;	/* bytes eaten by the consumer */
static volatile uint32_t stat_rx_pushed;	/* bytes accepted into the input FIFO */
static volatile uint32_t stat_rx_popped;	/* Data reads that dequeued */
static volatile uint32_t stat_rx_overruns;	/* loopback pushes dropped */
static volatile uint32_t stat_tx_capped;	/* drains that hit the bound */
static volatile uint32_t stat_tx_dropped;	/* writes lost to a full FIFO */
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
	/* RELEASE. The ring slots are plain bytes and the index is volatile,
	 * which orders the index against other volatile accesses and NOTHING
	 * against the slot. Without this the store of the byte may be observed
	 * after the store of the index, and the consuming core reads a slot
	 * that still holds its PREVIOUS contents -- an older character, which
	 * in a ring that has been in use is whatever was typed before. */
	std::atomic_thread_fence(std::memory_order_release);
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

	const bool rdrf = rx_level() > 0u;
	if (rdrf) {
		st |= VID_STAT_RDRF;
		/* ACQUIRE, pairing with the release in rx_push(): having seen
		 * the index move, make sure we also see the byte it published. */
		std::atomic_thread_fence(std::memory_order_acquire);
		reg[VID_DATA_OFFSET] = rx_fifo[rx_tail & RX_FIFO_MASK];
	}

	/* !IRQ follows RDRF while the guest has asked for it, exactly as an
	 * MC6850 does: asserted when a byte is waiting, released once the
	 * guest has read the last one. Bit 7 mirrors the pin, so a guest
	 * sharing the interrupt can tell whether this device is the source
	 * without a side effect. */
	const bool want_irq = rie && rdrf;
	if (want_irq)
		st |= VID_STAT_IRQ;
	if (want_irq != irq_asserted) {
		irq_asserted = want_irq;
		vid_link_irq_out(want_irq);	/* on CHANGE only */
	}
	if (tx_level() < TX_FIFO_SIZE)
		st |= VID_STAT_TDRE;
	if (ready)
		st |= VID_STAT_READY;
	if (ms_changed) {
		st |= VID_STAT_MOUSE;
		/* Parked between passes: keep byte 0 current, so a pass starts
		 * with a sample taken microseconds ago rather than one left over
		 * from the previous pass. Once a pass is under way the snapshot
		 * is frozen, which is what makes it self-consistent. */
		if (ms_index == 0u && ms_dirty && !ms_held) {
			ms_latch();
			reg[VID_MOUSE_OFFSET] = ms_snapshot[0];
		}
	}
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

	/* Put the snapshot back at byte 0. A guest that abandons a pass part
	 * way through has no other way to regain phase, and reset is exactly
	 * the lever it reaches for; without this the index stays stranded and
	 * every later pass is permanently misaligned. */
	ms_index = 0u;
	ms_latch();
	reg[VID_MOUSE_OFFSET] = ms_snapshot[0];

	info_index = 0u;
	reg[VID_INFO_OFFSET] = info_block[0];

	restage();
}

extern "C" void
vid_link_init(void)
{
	loopback = false;
	rie = false;		/* never interrupt a guest that has not asked */
	ready = true;
	vid_link_reset();		/* also homes the mouse snapshot */
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
		/* TDRE promised acceptance, so a full FIFO here means the
		 * guest wrote without polling. Dropping is the agreed
		 * behaviour -- better than blocking a bus cycle -- but it is
		 * NOT graceful, whatever the old comment here claimed.
		 *
		 * This stream is framed: every command has a fixed parameter
		 * count and the bytes that follow are read as whatever the
		 * frame says comes next. Lose one byte and every byte after
		 * it is misread -- a coordinate taken from a colour, a
		 * sub-code taken from half a coordinate. The picture does not
		 * break, it goes subtly WRONG, which is far harder to
		 * recognise than a blank screen.
		 *
		 * So count it. A silent drop is the difference between
		 * "the guest sent something odd" and "we lost a byte and
		 * everything after it was nonsense", and until now nothing
		 * could tell those apart. */
		if (tx_level() < TX_FIFO_SIZE) {
			tx_fifo[tx_head & TX_FIFO_MASK] = value;
			/* RELEASE, as in rx_push: the slot must be visible before
			 * the index that advertises it. This ring carries the
			 * guest's output, so a torn publish renders a stale
			 * character on screen. */
			std::atomic_thread_fence(std::memory_order_release);
			tx_head = tx_head + 1u;
			stat_tx_accepted = stat_tx_accepted + 1u;
		} else {
			stat_tx_dropped = stat_tx_dropped + 1u;
		}
		break;
	case VID_MOUSE_OFFSET:
	case VID_INFO_OFFSET:
		/* Already handled synchronously in the write ISR (see
		 * vid_link_mouse_write / _info_write); nothing to do here. */
		break;
	case VID_STAT_OFFSET:
		loopback = (value & VID_CTL_LOOPBACK) != 0u;
		rie = (value & VID_CTL_RIE) != 0u;
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
	/* Render core. Bounded per call (VID_LINK_DRAIN_MAX) because this runs
	 * on the core that owes the overlay service a frame; the rest drains on
	 * the next pass. If the guest outruns that, the FIFO backs up and TDRE
	 * flow-controls it, and the bus core keeps the registers current
	 * meanwhile. */
	if (consume_reset_pending) {
		consume_reset_pending = false;
		vid_link_consume_reset();
	}

	/* An earlier burst's drawing still owed. Finish that, and ONLY that:
	 * draining new bytes first could let a later burst's graphics land
	 * before the earlier burst's text was drawn, and opaque text would then
	 * paint over them. The guest is flow-controlled by TDRE meanwhile. */
	if (!loopback && vid_link_consume_busy()) {
		vid_link_consume_begin();
		vid_link_consume_flush();
		return;
	}

	/* Drain the command FIFO. Loopback (M0 test mode) diverts bytes to
	 * the input FIFO; otherwise they feed the console layer. */
	bool consumed = false;

	if (tx_tail != tx_head && !loopback)
		vid_link_consume_begin();

	if (tx_tail != tx_head)
		std::atomic_thread_fence(std::memory_order_acquire);
	unsigned budget = VID_LINK_DRAIN_MAX;
	/* The trace's storage, asked once per call rather than per byte -- and
	 * only when there IS a byte. This call runs millions of times a second
	 * on an idle link, and asking unconditionally (a call into another file
	 * that calls win_store_slots()) raised the cost of an idle pass from
	 * ~0.22 to ~0.29 us, which is what the "busy" figure in status reports
	 * on a quiet board. */
	uint8_t *const trace = (tx_tail != tx_head) ? cmd_trace_base() : NULL;

	while (tx_tail != tx_head && budget > 0u) {
		budget = budget - 1u;
		const uint8_t b = tx_fifo[tx_tail & TX_FIFO_MASK];
		tx_tail = tx_tail + 1u;

		/* Record the byte AS CONSUMED, with its role in the framing
		 * taken BEFORE the consumer sees it -- after, the state has
		 * already moved on to describe the next byte. Indexed by the
		 * absolute count, which is what makes an offset in a dump
		 * line up with "cons" in status and with the guest's map. */
		if (trace != NULL) {
			const uint32_t at =
			    (stat_tx_consumed & (CMD_TRACE_ENTRIES - 1u)) * 2u;

			trace[at] = b;
			trace[at + 1u] = loopback ? (uint8_t)VID_BYTE_LOOPBACK :
			    vid_link_consume_class();
		}
		stat_tx_consumed = stat_tx_consumed + 1u;
		stat_last_byte = b;
		if (loopback) {
			rx_push(b);
		} else {
			vid_link_consume(b);
			consumed = true;
		}
	}
	/* Whether the bound actually binds is the thing worth knowing: zero
	 * here means it never limited anything and costs nothing, while a
	 * climbing count means the guest is being flow-controlled and the
	 * bound wants revisiting against the service interval. */
	if (budget == 0u && tx_tail != tx_head)
		stat_tx_capped = stat_tx_capped + 1u;
	if (consumed)
		vid_link_consume_flush();
}

extern "C" void
vid_link_trace_span(uint32_t *oldest, uint32_t *next)
{
	const uint32_t n = stat_tx_consumed;

	*next = n;
	*oldest = (cmd_trace_base() == NULL) ? n :
	    (n > CMD_TRACE_ENTRIES ? n - CMD_TRACE_ENTRIES : 0u);
}

extern "C" uint32_t
vid_link_trace_read(uint32_t from, uint32_t n, uint8_t *bytes,
    uint8_t *classes)
{
	const uint8_t *trace = cmd_trace_base();
	uint32_t oldest, next, got = 0;

	vid_link_trace_span(&oldest, &next);
	if (trace == NULL || from < oldest)
		return 0;
	while (got < n && from + got < next) {
		const uint32_t at =
		    ((from + got) & (CMD_TRACE_ENTRIES - 1u)) * 2u;

		bytes[got] = trace[at];
		classes[got] = trace[at + 1u];
		got++;
	}
	return got;
}

extern "C" void
vid_link_trace_print(uint32_t n)
{
	uint32_t oldest, next;

	if (cmd_trace_base() == NULL) {
		printf("Trace:       no storage — the PSRAM probe found"
		       " nothing, so no bytes were kept\n");
		return;
	}
	vid_link_trace_span(&oldest, &next);
	if (n > next - oldest)
		n = next - oldest;
	printf("Trace:       %lu bytes consumed; showing the last %lu"
	       " (holds %lu)\n", (unsigned long)next, (unsigned long)n,
	       (unsigned long)CMD_TRACE_ENTRIES);
	printf("             |start .subcode  param ~payload Lloopback;"
	       " offsets match \"cons\" in status\n");

	/* Sixteen to a line, each marked by the role it played. The marks
	 * are the point: a dropped byte shows as a command whose parameters
	 * run into the next command's start -- a "|" where a parameter
	 * should be, or a sub-code where a coordinate was expected. */
	static const char mark[5] = { '|', '.', ' ', '~', 'L' };
	uint32_t at = next - n;

	while (at < next) {
		uint8_t b[16], c[16];
		const uint32_t line = (next - at < 16u) ? next - at : 16u;
		const uint32_t got = vid_link_trace_read(at, line, b, c);

		printf("  %8lu ", (unsigned long)at);
		for (uint32_t i = 0; i < 16u; i++) {
			if (i < got)
				printf("%c%02X", c[i] < 5u ? mark[c[i]] : '?',
				    b[i]);
			else
				printf("   ");
		}
		printf("  ");
		for (uint32_t i = 0; i < got; i++)
			putchar((b[i] >= 0x20u && b[i] < 0x7Fu) ? b[i] : '.');
		putchar('\n');
		if (got == 0u)
			break;		/* overwritten under us: stop, not spin */
		at += got;
	}
}

extern "C" bool
vid_link_idle(void)
{
	return tx_tail == tx_head && !vid_link_consume_busy();
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
	printf("             pending %u, drain capped %lu\n",
	       (unsigned)tx_level(), (unsigned long)stat_tx_capped);
	/* Only speaks when it has something to say, because a zero here is
	 * the only acceptable reading. Anything else means the command stream
	 * lost its framing and everything drawn after that point was decoded
	 * from the wrong bytes. */
	if (stat_tx_dropped != 0u)
		printf("             %lu WRITES DROPPED — the command stream"
		       " lost framing from that point\n",
		       (unsigned long)stat_tx_dropped);
	printf("Input:       recv %lu pop %lu pending %u ovr %lu; RDRF %s\n",
	       (unsigned long)stat_rx_pushed, (unsigned long)stat_rx_popped,
	       (unsigned)rx_level(),
	       (unsigned long)stat_rx_overruns,
	       (reg[VID_STAT_OFFSET] & VID_STAT_RDRF) ? "set" : "clear");
}

/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The guest-facing video console link: an MC6850-shaped mailbox in the
 * $FF10 window, agreed with the NitrOS-9 side (docs/copico-video-protocol.md
 * in the nitros9 tree is the payload contract; this header is the register
 * contract).
 *
 *   $FF10 VID.Data  R: dequeue one input byte/event  W: enqueue one byte to us
 *   $FF11 VID.Stat  R: status (below)                W: control (below)
 *   $FF12-$FF17     reserved
 *
 * Status is deliberately MC6850-flavoured so the 6809 driver can follow
 * sc6850.asm: bit0 RDRF (input available), bit1 TDRE (space free — set
 * guarantees the next single Data write is accepted). The guest polls; no
 * interrupt is part of the contract (bit7 is reserved for a future !IRQ
 * accelerator).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE GUEST MUST READ THIS WINDOW ONE BYTE AT A TIME. A 16-bit load
 * straddling $FF10/$FF11 (LDD, LDX, LDU...) is forbidden, for two
 * independent reasons:
 *
 * 1. READS HAVE SIDE EFFECTS here. A read of Data POPS the input FIFO and a
 *    read of Mouse STEPS the snapshot index, so a 16-bit load consumes a
 *    byte the guest never asked for and desynchronises a mouse pass. This
 *    is the same trap that catches line-rendering monitors: on hardware
 *    2026-09-22 a 6809 monitor's "examine" of a SINGLE byte at $FF10
 *    rendered a full eight-byte line, reading every offset once.
 *
 * 2. It is the ONLY access pattern that can place two window accesses on
 *    CONSECUTIVE bus cycles. Everything else is instruction-separated: LDA
 *    extended is five cycles with the data read on the last, so two window
 *    reads from separate instructions are at least five E cycles apart.
 *    Pop-on-read runs in the bus read ISR and must restage before the next
 *    access, and the measured worst case (0.73 us at 320 MHz) fits inside
 *    five cycles at 3 MHz -- 1.67 us -- but NOT inside one bus cycle at
 *    3 MHz, which is 0.33 us. The second half of a 16-bit load would be
 *    served the byte still staged.
 *
 * At 1 MHz this is advice. At 3 MHz -- and Mark has HD6309 parts in stock
 * -- it is load-bearing. Nothing in the firmware detects a violation: the
 * guest is trusted here, deliberately, because policing it would put work
 * in the hot path to catch a bug that belongs to the bus master. */

#define VID_DATA_OFFSET   0u	/* $FF10 */
#define VID_STAT_OFFSET   1u	/* $FF11 */
#define VID_MOUSE_OFFSET  2u	/* $FF12 */
#define VID_INFO_OFFSET   3u	/* $FF13 */

/* VID.Stat (read) */
#define VID_STAT_RDRF     0x01u	/* input byte available in Data */
#define VID_STAT_TDRE     0x02u	/* space free: next Data write is accepted */
#define VID_STAT_READY    0x04u	/* firmware alive and initialised */
#define VID_STAT_VER_LSB  3u	/* bits 5-3: protocol version -- AND THE
				 * LIVENESS CHECK. A guest must verify this
				 * field before believing anything else in the
				 * byte, because an absent device is not a
				 * quiet one: while this peripheral is reset,
				 * reflashed or unpowered it does not drive the
				 * bus at all, and the 6809 reads whatever the
				 * hardware gives it. With pull-ups that is
				 * $FF, which presents as RDRF set, TDRE set
				 * and READY set -- "a byte is waiting" -- and
				 * a guest that trusts it will ring phantom
				 * input for as long as we are away. $FF reads
				 * as version 7 here and $00 as version 0;
				 * neither matches, so one mask and one compare
				 * make the guest immune to our absence. */
/* DO NOT BUMP THIS UNILATERALLY.
 *
 * The guest checks it STRICTLY: NitrOS-9's vcon rejects the device unless this
 * field reads exactly 1, deliberately, because the field doubles as the
 * liveness check above and a thing that gates liveness must fail CLOSED. A
 * protocol change the driver has not been told about should stop it, not be
 * silently tolerated.
 *
 * The consequence is real coupling: raising this number disables the guest's
 * console until the NitrOS-9 side raises its own constant to match. That is
 * the intended behaviour, and it only works if the change is agreed first.
 * Committed 2026-09-23: this version changes by agreement with the NitrOS-9
 * side, reviewed before any firmware ships, the way the mouse protocol was
 * designed -- not as a side effect of some other work. */
#define VID_STAT_VERSION  1u
#define VID_STAT_IRQ      0x80u	/* this device is asserting !IRQ (MC6850
				 * puts its IRQ flag in bit 7 too, and drives
				 * the pin from it; so do we) */
#define VID_STAT_MOUSE    0x40u	/* pointer or buttons moved since the last
				 * complete snapshot read */

/* VID.Ctl (write): bit1 is a level, bit0 an action — $03 = reset with
 * loopback left on. */
#define VID_CTL_RESET     0x01u	/* flush both FIFOs (M1+: clear/home too) */
#define VID_CTL_LOOPBACK  0x02u	/* echo command bytes into the input FIFO */
#define VID_CTL_RIE       0x80u	/* receive interrupt enable: assert !IRQ while
				 * RDRF is set. Bit 7 is where the MC6850 keeps
				 * RIE. OFF after reset, so a polling guest is
				 * never interrupted by a line it did not ask
				 * for. */

/* All of the below run on Core 1 (the bus dispatch loop) unless noted. */

/* Bring the link up: flush, set READY + version, stage Data/Stat. */
void vid_link_init(void);

/* Soft reset — also invoked on the host's !RESET edge. */
void vid_link_reset(void);

/* Dispatch a guest write to offset 0 or 1 (from the write ring). */
void vid_link_guest_write(uint8_t location, uint8_t value);

/* Note a guest read at an offset (from the read notification): a Data read
 * pops the input FIFO. */
void vid_link_guest_read(uint8_t location);

/* The link is serviced from BOTH cores, split by deadline class.
 *
 * vid_link_stage() runs on the bus core, alongside the scanline interrupt.
 * It only refreshes the guest-visible registers — short, bounded work.
 *
 * vid_link_render() runs on the other core and does everything expensive:
 * draining the command FIFO and rendering it. Rendering must not share a
 * core with the video scan-out; that core's deadline is measured in
 * microseconds and its code has to stay resident, whereas a renderer is
 * unbounded and lives in flash.
 *
 * The split is safe because each ring index has exactly one writer. The bus
 * core fills the command FIFO and empties the input FIFO; the render core
 * empties the command FIFO and fills the input FIFO (loopback, and cooked
 * keystrokes). A reset therefore clears each ring from the side that owns
 * its index, and hands the renderer's own re-arming to the render core. */
void vid_link_stage(void);

/* Tell the link what VID.Info should describe. Called by the console when a
 * window is selected, so the guest's read-back reports the surface it is
 * actually drawing on rather than the panel. Takes effect at the start of the
 * next read pass, never part way through one. */
void vid_link_info_set(uint16_t width_px, uint16_t height_px,
    uint8_t cols, uint8_t rows);

/* Bytes vid_link_render() will consume in ONE call. The drain used to run
 * until the FIFO was empty, so a single call could work through a full 2 KB
 * backlog: measured at 18 ms under a saturated console, against a 16.7 ms
 * frame. The overlay service is frame-gated and sits after the render call in
 * the same loop pass, so an overrunning drain delays the cursor by exactly
 * that much -- and the display does not wait.
 *
 * Chosen against the frame budget rather than picked round. The saturated
 * console measured 20.3 us per byte (10,000,000 bytes in 202.9 s, the
 * worst sustained case, scrolling continuously), so 256 bytes is about
 * 5.2 ms -- under a third of a frame, leaving the rest for one expensive
 * primitive plus the remainder of the loop.
 *
 * It costs no throughput. The render core managed 219,000 loop passes a
 * second while saturated, so even one byte per pass would outrun a 1 MHz
 * 6809; 256 is three orders of magnitude clear of anything the guest can
 * produce. The bound only ever binds on catch-up, which is exactly the case
 * it exists to cap.
 *
 * THE CALLER MUST THEREFORE CALL AGAIN. Whatever is left drains on the next
 * pass; if the guest outruns that, the FIFO backs up, TDRE deasserts and the
 * 6809 waits -- which is the correct outcome. Better to make the guest wait
 * than to miss a frame. */
#define VID_LINK_DRAIN_MAX  256u
void vid_link_render(void);

/* ── Mouse ($FF12 VID.Mouse, read) ───────────────────────────────────────
 * Successive reads walk a seven-byte snapshot; the index wraps and RE-LATCHES
 * at the start of each pass, so reading seven bytes in a row yields one
 * self-consistent sample without any handshake.
 *
 *   0    buttons now: bit0 left, bit1 right, bit2 middle
 *   1    pressed since the last snapshot, same bits -- cleared on latch
 *   2-3  X, big-endian, 0 .. FB_WIDTH-1
 *   4-5  Y, big-endian, 0 .. FB_HEIGHT-1
 *   6    wheel notches since the last snapshot, signed -- cleared on latch
 *
 * Byte 1 is what makes a slow poll safe: the guest's tick runs at 50Hz, so a
 * click shorter than 20ms would vanish between samples of byte 0 alone. The
 * sticky bits mean its edge detection cannot miss a press.
 *
 * VID.Stat bit 6 says whether anything has changed, so an idle mouse costs the
 * guest one bit test rather than seven reads.
 *
 * Deliberately NOT the CoCo3 32-byte SS.Mouse packet: that driver does its own
 * click counting and timing, and half the packet is window geometry only the
 * guest knows. We supply the device, it keeps the shadow. */
#define VID_MOUSE_SNAPSHOT_LEN 7u

#define VID_MOUSE_BTN_LEFT    0x01u
#define VID_MOUSE_BTN_RIGHT   0x02u
#define VID_MOUSE_BTN_MIDDLE  0x04u

/* Called by the HID cooker (render core) after each mouse report: absolute
 * position already clamped to the screen, current buttons, and the wheel
 * movement for this report. */
/* Drive the host !IRQ line. PLATFORM-PROVIDED, because this translation unit
 * is portable and must not know about GPIOs: the firmware drives GP21 as a
 * simulated open collector, and the simulator drives the emulated 6809's IRQ
 * pin. Called only when the state CHANGES -- it is reached from the bus core's
 * hot loop, which must not poke hardware on every pass. */
void vid_link_irq_out(bool assert);

/* The console bell ($07): make a short sound. Platform-provided for the same
 * reason -- the console is portable, and how a board makes a sound (PWM on a
 * pin here; a counter in the simulator) is not its business. Called on the
 * render core. */
void vid_link_bell(void);

/* The guest's Tone command ($1B $27): CoCo 3 SS.Tone values unchanged --
 * pitch 1-4095 (relative, higher is higher), duration in 1/60 s ticks with 0
 * meaning stop now, volume 0-63. Starts at once and replaces any tone or bell
 * sounding; it does not hold the byte stream up. Platform-provided, like the
 * bell. Render core. */
void vid_link_tone(uint16_t pitch, uint8_t ticks, uint8_t volume);

/* True while this device is asserting !IRQ. */
bool vid_link_irq_asserted(void);

void vid_link_mouse_update(int x, int y, uint8_t buttons, int wheel_delta);

/* Note a guest read of the mouse port: advance the snapshot index, relatching
 * at wrap. Called from the bus-read ISR, like the Data pop. */
void vid_link_mouse_read(void);
/* Guest write to VID.Mouse: sample now and home the snapshot index. Makes a
 * pass self-framing (write, then read VID_MOUSE_SNAPSHOT_LEN bytes), so a
 * guest that loses count resyncs with one write instead of a link reset. */
void vid_link_mouse_write(void);

/* ---- VID.Info ($FF13, read-only) --------------------------------------
 *
 * The drawing surface, so the guest need not hard-code it. Successive reads
 * walk six bytes and the index wraps; a write of any value homes it, exactly
 * as VID.Mouse works, so a guest frames a read the same way it already does
 * for the mouse: write, then read.
 *
 * Pixels first, deliberately: graphics code reads the first four bytes and
 * stops, text layout reads on. Big-endian, as the 6809 reads.
 *
 *   0-1  framebuffer width   (pixels)
 *   2-3  framebuffer height  (pixels)
 *   4    text columns
 *   5    text rows
 *
 * A constant in the protocol would go stale the moment the panel or the
 * scaler changed, and a request/response would need machinery the link does
 * not have. This costs one reserved register and no round trip. */
#define VID_INFO_LEN      6u
void vid_link_info_read(void);
void vid_link_info_write(void);

/* Console diagnostics: current pointer state. */
void vid_link_mouse_print(void);

/* Enqueue one input byte for the guest (sets RDRF via the next restage).
 * The M4 contract with the NitrOS-9 side: keystrokes are PLAIN bytes —
 * printable characters and the control bytes a shell needs (CR, BS,
 * Ctrl-C, ...), one byte per key. Called from the USB HID cooker on
 * Core 0 (the RX ring is single-producer/single-consumer across cores;
 * loopback, the only other producer, is a test mode). Drops when full. */
void vid_link_input(uint8_t byte);

/* True while the guest has input it has not yet read. Core 0, for the
 * keyboard's auto-repeat, which holds a repeat back rather than queue one
 * behind an unread byte. */
bool vid_link_input_pending(void);

/* Console diagnostics (Core 0; reads counters only). */
void vid_link_print_status(void);

/* TRUE when the render side has caught up: no command bytes waiting and no
 * drawing owed from an earlier burst. Render core only. Both the drain and the
 * text refresh are bounded per call, so "one call" no longer means "done" --
 * this is how anything that needs "done" asks for it. */
bool vid_link_idle(void);

/* The COMMAND TRACE (render core only): the last CMD_TRACE_ENTRIES bytes the
 * renderer consumed, each with its VID_BYTE_* class, addressed by ABSOLUTE
 * offset -- the same count "status" reports as "cons", which never resets, so
 * an offset quoted from a dump lines up with the guest's own byte map.
 * span() gives the oldest offset still held and the next to be written; read()
 * copies up to n entries starting at `from`, returning how many it copied
 * (entries already overwritten, or not yet written, are not returned).
 * print() dumps the most recent n. With no storage, all three say so. */
void vid_link_trace_span(uint32_t *oldest, uint32_t *next);
uint32_t vid_link_trace_read(uint32_t from, uint32_t n, uint8_t *bytes,
    uint8_t *classes);
void vid_link_trace_print(uint32_t n);

/* The consumer contract: the glass-TTY / command-interpreter layer
 * (vid_console.cpp — portable, shared with the simulator) implements
 * these. consume_begin() precedes each burst (the console uses it to lift
 * the cursor out of the way before anything draws); consume() gets each
 * command byte in order when loopback is off;
 * consume_flush() follows each drained burst (batch a refresh there);
 * consume_reset() accompanies every link reset (power-on, Ctl RESET,
 * host !RESET).
 *
 * consume_busy() is TRUE while an earlier burst's drawing is unfinished --
 * the text refresh is bounded per call, so a full screen takes several. While
 * it is, the render pass calls begin() and flush() WITHOUT draining any new
 * bytes: stream order is kept exactly as it was when a burst's text was drawn
 * in one go, since nothing from a later burst can run before an earlier
 * burst's drawing is complete. The guest meanwhile waits on TDRE, the same
 * trade the bounded drain already makes.
 *
 * consume_hold() is TRUE while a command in progress must finish before the
 * NEXT BYTE is even interpreted -- a flood fill, which is spread over passes
 * and finds its way by the colours on screen, so anything drawn beneath it
 * mid-fill would change the region it fills. The drain stops at once when it
 * rises; busy() then carries the command to completion. Distinct from busy()
 * on purpose: dirty text must NOT stop the drain, or a line of typing would
 * cost a render pass a character. */
void vid_link_consume_begin(void);
void vid_link_consume(uint8_t byte);
void vid_link_consume_flush(void);
void vid_link_consume_reset(void);
bool vid_link_consume_busy(void);
bool vid_link_consume_hold(void);

/* The ROLE the next byte will play in the framing, asked just before each
 * byte is handed to consume(). The command trace records it beside the byte,
 * so a dump shows where every command starts -- and therefore exactly where
 * the framing went wrong, if it ever does. */
uint8_t vid_link_consume_class(void);

#define VID_BYTE_START		0u	/* begins a command, or is plain text */
#define VID_BYTE_SUBCODE	1u	/* the sub-code after $1B */
#define VID_BYTE_PARAM		2u	/* a fixed parameter of a command */
#define VID_BYTE_PAYLOAD	3u	/* GPLoad's streamed data */
#define VID_BYTE_LOOPBACK	4u	/* echoed in loopback, never interpreted */

#ifdef __cplusplus
}
#endif

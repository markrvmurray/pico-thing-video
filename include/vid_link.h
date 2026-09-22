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

#define VID_DATA_OFFSET   0u	/* $FF10 */
#define VID_STAT_OFFSET   1u	/* $FF11 */
#define VID_MOUSE_OFFSET  2u	/* $FF12 */

/* VID.Stat (read) */
#define VID_STAT_RDRF     0x01u	/* input byte available in Data */
#define VID_STAT_TDRE     0x02u	/* space free: next Data write is accepted */
#define VID_STAT_READY    0x04u	/* firmware alive and initialised */
#define VID_STAT_VER_LSB  3u	/* bits 5-3: protocol version */
#define VID_STAT_VERSION  1u
#define VID_STAT_MOUSE    0x40u	/* pointer or buttons moved since the last
				 * complete snapshot read */

/* VID.Ctl (write): bit1 is a level, bit0 an action — $03 = reset with
 * loopback left on. */
#define VID_CTL_RESET     0x01u	/* flush both FIFOs (M1+: clear/home too) */
#define VID_CTL_LOOPBACK  0x02u	/* echo command bytes into the input FIFO */

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
void vid_link_mouse_update(int x, int y, uint8_t buttons, int wheel_delta);

/* Note a guest read of the mouse port: advance the snapshot index, relatching
 * at wrap. Called from the bus-read ISR, like the Data pop. */
void vid_link_mouse_read(void);
/* Guest write to VID.Mouse: sample now and home the snapshot index. Makes a
 * pass self-framing (write, then read VID_MOUSE_SNAPSHOT_LEN bytes), so a
 * guest that loses count resyncs with one write instead of a link reset. */
void vid_link_mouse_write(void);

/* Console diagnostics: current pointer state. */
void vid_link_mouse_print(void);

/* Enqueue one input byte for the guest (sets RDRF via the next restage).
 * The M4 contract with the NitrOS-9 side: keystrokes are PLAIN bytes —
 * printable characters and the control bytes a shell needs (CR, BS,
 * Ctrl-C, ...), one byte per key. Called from the USB HID cooker on
 * Core 0 (the RX ring is single-producer/single-consumer across cores;
 * loopback, the only other producer, is a test mode). Drops when full. */
void vid_link_input(uint8_t byte);

/* Console diagnostics (Core 0; reads counters only). */
void vid_link_print_status(void);

/* The consumer contract: the glass-TTY / command-interpreter layer
 * (vid_console.cpp — portable, shared with the simulator) implements
 * these. consume_begin() precedes each burst (the console uses it to lift
 * the cursor out of the way before anything draws); consume() gets each
 * command byte in order when loopback is off;
 * consume_flush() follows each drained burst (batch a refresh there);
 * consume_reset() accompanies every link reset (power-on, Ctl RESET,
 * host !RESET). */
void vid_link_consume_begin(void);
void vid_link_consume(uint8_t byte);
void vid_link_consume_flush(void);
void vid_link_consume_reset(void);

#ifdef __cplusplus
}
#endif

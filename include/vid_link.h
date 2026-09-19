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

/* VID.Stat (read) */
#define VID_STAT_RDRF     0x01u	/* input byte available in Data */
#define VID_STAT_TDRE     0x02u	/* space free: next Data write is accepted */
#define VID_STAT_READY    0x04u	/* firmware alive and initialised */
#define VID_STAT_VER_LSB  3u	/* bits 5-3: protocol version */
#define VID_STAT_VERSION  1u

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

/* Per-loop service: drain the command FIFO, restage Data and Stat. */
void vid_link_task(void);

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
 * these. consume() gets each command byte in order when loopback is off;
 * consume_flush() follows each drained burst (batch a refresh there);
 * consume_reset() accompanies every link reset (power-on, Ctl RESET,
 * host !RESET). */
void vid_link_consume(uint8_t byte);
void vid_link_consume_flush(void);
void vid_link_consume_reset(void);

#ifdef __cplusplus
}
#endif

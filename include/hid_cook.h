/* hid_cook.h -- HID report cooking: raw boot-protocol bytes to guest input.
 *
 * PORTABLE. No pico or TinyUSB includes, so the USim simulator compiles this
 * translation unit bit-identically and can drive it with synthetic reports.
 * Reports arrive as plain bytes rather than TinyUSB structs precisely to keep
 * that true; usb_host.cpp owns the stack and hands the bytes over.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef HID_COOK_H
#define HID_COOK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot-protocol report sizes. The keyboard's is exact; the mouse's is an
 * UPPER BOUND -- a boot mouse sends three bytes and wheel/pan are later
 * additions many mice never send, so treating five as a minimum discards
 * every report from an ordinary mouse. */
#define HID_KBD_REPORT_LEN	8u
#define HID_MOUSE_REPORT_MIN	3u
#define HID_MOUSE_REPORT_MAX	5u

/* Keyboard: modifier, reserved, then up to six held key usages. Emits one
 * cooked byte per NEW key down (UK layout) into the guest input FIFO. */
void hid_cook_keyboard(const uint8_t *report, unsigned len);

/* Mouse: buttons, dx, dy, then optional wheel and pan. Accumulates the
 * absolute cursor, publishes position and buttons for the guest snapshot, and
 * moves the on-screen pointer. */
void hid_cook_mouse(const uint8_t *report, unsigned len);

/* Echo cooked input to the console: one line per keystroke and per button
 * edge. Off until asked for -- invaluable during bring-up, noise while someone
 * is typing at the guest console. The console's `verbose` command drives this;
 * the flag is pushed in so this module stays free of console knowledge. */
void hid_cook_set_echo(bool on);

/* Forget held keys and centre the cursor. */
void hid_cook_reset(void);

/* State, for tests and the console status line. */
int  hid_cook_cursor_x(void);
int  hid_cook_cursor_y(void);
bool hid_cook_caps_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* HID_COOK_H */

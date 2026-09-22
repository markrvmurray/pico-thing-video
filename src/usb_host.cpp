/* usb_host.cpp -- USB HID host: keyboard + mouse (via hub). Compiled only in
 * host builds (USB_HOST_MODE).
 *
 * Both devices reach the 6809 through the video link's register window, and
 * they reach it by different routes because they want different shapes:
 *
 *   Keyboard -> VID.Data, one cooked byte per keystroke. "Cooked" means the
 *   guest never sees raw HID: UK layout, caps-lock XOR shift, Ctrl chords
 *   folded to C0, so what arrives is a final character.
 *
 *   Mouse -> VID.Mouse, a polled snapshot of position and buttons. Motion is
 *   never pushed, so it cannot storm the guest however fast the mouse moves.
 *   The 6809 keeps the click counting and hold timers, as its driver already
 *   does for CoCo3 hardware.
 *
 * Coordinates handed over are raw framebuffer pixels. The text-cell figures in
 * the console echo below are a convenience for reading the output against the
 * character grid, not a limit on what clients can have.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cstdint>
#include <cstdio>

#include "pico/time.h"
#include "tusb.h"

#include "usb.h"
#include "video.h"
#include "vid_link.h"   /* screen geometry: FB_WIDTH/FB_HEIGHT, CHAR_W/H, TEXT_COLS/ROWS */

/* Echo cooked events to the UART console (first-light bring-up aid). */
#ifndef USB_HOST_ECHO
#define USB_HOST_ECHO 1
#endif

/* Screen geometry (FB_WIDTH/FB_HEIGHT, CHAR_W/H, TEXT_COLS/ROWS) comes from
 * video.h -- the single source of truth -- so the cursor->cell mapping cannot
 * drift from the framebuffer. */

static void
emit_key(uint8_t c)
{
	/* Keystrokes feed the guest console as plain bytes; the UART echo is
	 * a bring-up aid. */
	vid_link_input(c);
#if USB_HOST_ECHO
	if (c >= ' ' && c < 0x7F)
		printf("KEY '%c' (0x%02X)\n", (char)c, c);
	else
		printf("KEY 0x%02X\n", c);
#endif
}

/* ======================================================================== *
 *  Keyboard cooking — UK layout, HID usage -> final character.
 *  Non-ASCII UK glyphs map to the display font's CP437 codepoints:
 *  £ = 0x9C, ¬ = 0xAA.
 * ======================================================================== */
static uint8_t
uk_cook(uint8_t kc, bool shift, bool caps)
{
	/* Letters: case is shift XOR caps-lock. */
	if (kc >= HID_KEY_A && kc <= HID_KEY_Z) {
		bool upper = shift ^ caps;
		return (uint8_t)((upper ? 'A' : 'a') + (kc - HID_KEY_A));
	}

	switch (kc) {
	case HID_KEY_1:             return shift ? '!'  : '1';
	case HID_KEY_2:             return shift ? '"'  : '2';   /* UK: shift-2 = " */
	case HID_KEY_3:             return shift ? 0x9C : '3';   /* UK: shift-3 = £ */
	case HID_KEY_4:             return shift ? '$'  : '4';
	case HID_KEY_5:             return shift ? '%'  : '5';
	case HID_KEY_6:             return shift ? '^'  : '6';
	case HID_KEY_7:             return shift ? '&'  : '7';
	case HID_KEY_8:             return shift ? '*'  : '8';
	case HID_KEY_9:             return shift ? '('  : '9';
	case HID_KEY_0:             return shift ? ')'  : '0';

	case HID_KEY_ENTER:         return 0x0D;  /* CR: the OS-9 line terminator
	                                            * (and the M4 input contract) */
	case HID_KEY_ESCAPE:        return 0x1B;
	case HID_KEY_BACKSPACE:     return 0x08;
	case HID_KEY_TAB:           return '\t';
	case HID_KEY_SPACE:         return ' ';

	case HID_KEY_MINUS:         return shift ? '_'  : '-';
	case HID_KEY_EQUAL:         return shift ? '+'  : '=';
	case HID_KEY_BRACKET_LEFT:  return shift ? '{'  : '[';
	case HID_KEY_BRACKET_RIGHT: return shift ? '}'  : ']';
	case HID_KEY_BACKSLASH:     return shift ? '~'  : '#';   /* UK #/~ key (left of Enter) */
	case HID_KEY_EUROPE_1:      return shift ? '~'  : '#';   /* some UK kbds report #/~ here */
	case HID_KEY_SEMICOLON:     return shift ? ':'  : ';';
	case HID_KEY_APOSTROPHE:    return shift ? '@'  : '\'';  /* UK '/@ */
	case HID_KEY_GRAVE:         return shift ? 0xAA : '`';   /* UK `/¬ (¬ = CP437 0xAA) */
	case HID_KEY_COMMA:         return shift ? '<'  : ',';
	case HID_KEY_PERIOD:        return shift ? '>'  : '.';
	case HID_KEY_SLASH:         return shift ? '?'  : '/';
	case HID_KEY_EUROPE_2:      return shift ? '|'  : '\\';  /* UK ISO \| key (left of Z) */
	default:                    return 0;
	}
}

/* Previous keyboard report, to detect key-down transitions. */
static hid_keyboard_report_t kbd_prev = { 0, 0, { 0, 0, 0, 0, 0, 0 } };
static bool                  caps_lock = false;

static bool
key_in_report(const hid_keyboard_report_t *r, uint8_t kc)
{
	for (int i = 0; i < 6; i++)
		if (r->keycode[i] == kc)
			return true;
	return false;
}

static void
process_kbd(const hid_keyboard_report_t *r)
{
	bool shift = (r->modifier &
		(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
	bool ctrl = (r->modifier &
		(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;

	for (int i = 0; i < 6; i++) {
		uint8_t kc = r->keycode[i];
		if (kc == 0)
			continue;
		if (key_in_report(&kbd_prev, kc))
			continue;               /* still held from last report — not a new down */

		if (kc == HID_KEY_CAPS_LOCK) {
			caps_lock = !caps_lock; /* toggle on press */
			continue;
		}

		uint8_t c = uk_cook(kc, shift, caps_lock);
		/* Ctrl chords: fold letters (and the usual @[\]^_ range) onto
		 * the C0 controls the shell's editing chars live in. */
		if (ctrl && c != 0) {
			uint8_t u = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 0x20) : c;
			if (u >= 0x40 && u <= 0x5F)
				c = (uint8_t)(u & 0x1F);
		}
		if (c != 0)
			emit_key(c);
	}

	kbd_prev = *r;
}

/* ======================================================================== *
 *  Mouse — absolute cursor from relative HID deltas.
 * ======================================================================== */
static int cursor_x = FB_WIDTH  / 2;   /* pixels, clamped to screen */
static int cursor_y = FB_HEIGHT / 2;

static inline int clampi(int v, int lo, int hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

static inline int cur_col(void) { return clampi(cursor_x / CHAR_W, 0, TEXT_COLS - 1); }
static inline int cur_row(void) { return clampi(cursor_y / CHAR_H, 0, TEXT_ROWS - 1); }

/* Report a button edge on the console. This is a BRING-UP AID, not a
 * protocol: it was how clicks were first confirmed with no guest driver in
 * existence, and it stays for that. Deliberately no click counting, drag
 * threshold or double-click window -- that timing belongs to the 6809, which
 * does it from the snapshot's buttons and pressed-since bytes, and a second
 * implementation here would only be a second thing to disagree.
 *
 * Pixels first, cells after: the guest register carries raw framebuffer
 * coordinates, and reporting only cells understated the resolution clients
 * actually get. */
static void
report_button(const char *name, bool now, bool *was)
{
	if (now == *was)
		return;
	*was = now;
#if USB_HOST_ECHO
	printf("MOUSE %s %s at %d,%d (col %d row %d)\n",
	       name, now ? "down" : "up",
	       cursor_x, cursor_y, cur_col(), cur_row());
#else
	(void)name;
#endif
}

static void
process_mouse(const hid_mouse_report_t *r)
{
	/* Accumulate relative motion into the absolute cursor, clamp to screen. */
	cursor_x = clampi(cursor_x + r->x, 0, FB_WIDTH  - 1);
	cursor_y = clampi(cursor_y + r->y, 0, FB_HEIGHT - 1);

	static bool was_left, was_right, was_middle;
	report_button("left",   (r->buttons & MOUSE_BUTTON_LEFT)   != 0, &was_left);
	report_button("right",  (r->buttons & MOUSE_BUTTON_RIGHT)  != 0, &was_right);
	report_button("middle", (r->buttons & MOUSE_BUTTON_MIDDLE) != 0, &was_middle);
#if USB_HOST_ECHO
	if (r->wheel)
		printf("MOUSE wheel %+d\n", (int)r->wheel);
#endif

	/* Publish the raw state for the guest. The cooked gestures above stay
	 * as local diagnostics; the guest gets position and buttons and does
	 * its own click counting, which is what its driver already expects. */
	uint8_t b = 0u;
	if (r->buttons & MOUSE_BUTTON_LEFT)   b |= VID_MOUSE_BTN_LEFT;
	if (r->buttons & MOUSE_BUTTON_RIGHT)  b |= VID_MOUSE_BTN_RIGHT;
	if (r->buttons & MOUSE_BUTTON_MIDDLE) b |= VID_MOUSE_BTN_MIDDLE;
	vid_link_mouse_update(cursor_x, cursor_y, b, r->wheel);

	/* Follow the pointer on screen. */
	vidterm_pointer_set(cursor_x, cursor_y);
}

/* ======================================================================== *
 *  TinyUSB host callbacks (resolved by the C stack — keep extern "C").
 * ======================================================================== */
static int hid_mounts = 0;

extern "C" void
tuh_mount_cb(uint8_t daddr)
{
	printf("USB host: device %u attached\n", daddr);
}

extern "C" void
tuh_umount_cb(uint8_t daddr)
{
	printf("USB host: device %u detached\n", daddr);
}

extern "C" void
tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
	uint8_t const *desc_report, uint16_t desc_len)
{
	(void)desc_report;
	(void)desc_len;

	uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
	const char *what = (proto == HID_ITF_PROTOCOL_KEYBOARD) ? "keyboard"
			 : (proto == HID_ITF_PROTOCOL_MOUSE)    ? "mouse"
			 :                                        "generic HID";
	printf("USB host: HID %s mounted (dev %u, itf %u)\n", what, dev_addr, instance);
	hid_mounts++;

	/* Relies on boot-protocol reports (fixed hid_keyboard/mouse_report_t):
	 * TinyUSB defaults HID boot interfaces to boot protocol. Kick the first
	 * report; report_received_cb re-arms thereafter. */
	if (!tuh_hid_receive_report(dev_addr, instance))
		printf("USB host: failed to request report (dev %u, itf %u)\n",
			dev_addr, instance);
}

extern "C" void
tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
	printf("USB host: HID unmounted (dev %u, itf %u)\n", dev_addr, instance);
	if (hid_mounts > 0)
		hid_mounts--;
}

extern "C" void
tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
	uint8_t const *report, uint16_t len)
{
	uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

	if (proto == HID_ITF_PROTOCOL_KEYBOARD &&
	    len >= (uint16_t)sizeof(hid_keyboard_report_t)) {
		process_kbd((const hid_keyboard_report_t *)report);
	} else if (proto == HID_ITF_PROTOCOL_MOUSE && len >= 3u) {
		/* A boot mouse reports THREE bytes -- buttons, x, y. Wheel and
		 * pan are later additions that many mice simply do not send, so
		 * demanding sizeof(hid_mouse_report_t) (five) drops every
		 * report from an ordinary mouse, silently, while still burning
		 * the CPU that received it. Copy the bytes that are present
		 * rather than casting a short buffer to the full struct. */
		hid_mouse_report_t m;
		m.buttons = report[0];
		m.x       = (int8_t)report[1];
		m.y       = (int8_t)report[2];
		m.wheel   = (len >= 4u) ? (int8_t)report[3] : (int8_t)0;
		m.pan     = (len >= 5u) ? (int8_t)report[4] : (int8_t)0;
		process_mouse(&m);
	} else {
		/* Say so once per instance. A report that arrives and matches
		 * nothing is invisible otherwise -- it looks exactly like a
		 * device that is not sending at all. */
		static uint8_t moaned[CFG_TUH_HID];
		if (instance < CFG_TUH_HID && moaned[instance] < 3u) {
			moaned[instance]++;
			printf("USB host: unhandled HID report (dev %u, itf %u,"
			       " protocol %u, %u bytes)\n",
			       dev_addr, instance, proto, (unsigned)len);
		}
	}

	/* Re-arm for the next report. */
	tuh_hid_receive_report(dev_addr, instance);
}

/* Public: usb_is_active() in usb.cpp. */
extern "C" bool
usb_host_mounted(void)
{
	return hid_mounts > 0;
}

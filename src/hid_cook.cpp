/* hid_cook.cpp -- HID report cooking: raw boot-protocol bytes to guest input.
 *
 * PORTABLE (see hid_cook.h). The USB HID usage IDs and modifier bits are
 * spelled out below rather than taken from tusb.h, which is what lets this
 * file compile host-side; usb_host.cpp static_asserts them against TinyUSB's
 * own values so the two cannot drift apart unnoticed.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cstdint>
#include <cstdio>

#include "hid_cook.h"
#include "video.h"	/* screen geometry + vidterm_pointer_set */
#include "vid_link.h"

/* Echo cooked input to the console. USB_HOST_ECHO decides whether the echo
 * exists at all (it is compiled out of the simulator); the runtime flag below
 * decides whether it speaks, and the console's `verbose` command drives it.
 *
 * Default OFF. The echo prints a line per keystroke and per button edge, which
 * is what bring-up wants and pure noise once someone is typing at the guest
 * console -- so it is asked for rather than assumed. The flag is PUSHED IN
 * through hid_cook_set_echo(): this file stays portable, and must not learn
 * that a console exists. */
#ifndef USB_HOST_ECHO
#define USB_HOST_ECHO 1
#endif

#if USB_HOST_ECHO
static bool echo_enabled;
#endif

void
hid_cook_set_echo(bool on)
{
#if USB_HOST_ECHO
	echo_enabled = on;
#else
	(void)on;
#endif
}

/* ---- USB HID usage IDs (boot keyboard) ---------------------------------- */
enum : uint8_t {
	KEY_A = 0x04, KEY_Z = 0x1D,
	KEY_1 = 0x1E, KEY_2 = 0x1F, KEY_3 = 0x20, KEY_4 = 0x21, KEY_5 = 0x22,
	KEY_6 = 0x23, KEY_7 = 0x24, KEY_8 = 0x25, KEY_9 = 0x26, KEY_0 = 0x27,
	KEY_ENTER = 0x28, KEY_ESCAPE = 0x29, KEY_BACKSPACE = 0x2A,
	KEY_TAB = 0x2B, KEY_SPACE = 0x2C,
	KEY_MINUS = 0x2D, KEY_EQUAL = 0x2E,
	KEY_BRACKET_LEFT = 0x2F, KEY_BRACKET_RIGHT = 0x30,
	KEY_BACKSLASH = 0x31, KEY_EUROPE_1 = 0x32,
	KEY_SEMICOLON = 0x33, KEY_APOSTROPHE = 0x34, KEY_GRAVE = 0x35,
	KEY_COMMA = 0x36, KEY_PERIOD = 0x37, KEY_SLASH = 0x38,
	KEY_CAPS_LOCK = 0x39,
	KEY_EUROPE_2 = 0x64,
};

enum : uint8_t {
	MOD_LEFTCTRL = 0x01, MOD_LEFTSHIFT = 0x02,
	MOD_RIGHTCTRL = 0x10, MOD_RIGHTSHIFT = 0x20,
};

enum : uint8_t {
	MOUSE_BTN_L = 0x01, MOUSE_BTN_R = 0x02, MOUSE_BTN_M = 0x04,
};

static void
emit_key(uint8_t c)
{
	/* Keystrokes feed the guest console as plain bytes; the echo is a
	 * bring-up aid. */
	vid_link_input(c);
#if USB_HOST_ECHO
	if (!echo_enabled)
		return;
	if (c >= ' ' && c < 0x7F)
		printf("KEY '%c' (0x%02X)\n", (char)c, c);
	else
		printf("KEY 0x%02X\n", c);
#endif
}

/* ======================================================================== *
 *  Keyboard cooking -- UK layout, HID usage -> final character.
 *  Non-ASCII UK glyphs map to the display font's CP437 codepoints:
 *  £ = 0x9C, ¬ = 0xAA.
 * ======================================================================== */
static uint8_t
uk_cook(uint8_t kc, bool shift, bool caps)
{
	/* Letters: case is shift XOR caps-lock. */
	if (kc >= KEY_A && kc <= KEY_Z) {
		bool upper = shift ^ caps;
		return (uint8_t)((upper ? 'A' : 'a') + (kc - KEY_A));
	}

	switch (kc) {
	case KEY_1:             return shift ? '!'  : '1';
	case KEY_2:             return shift ? '"'  : '2';   /* UK: shift-2 = " */
	case KEY_3:             return shift ? 0x9C : '3';   /* UK: shift-3 = £ */
	case KEY_4:             return shift ? '$'  : '4';
	case KEY_5:             return shift ? '%'  : '5';
	case KEY_6:             return shift ? '^'  : '6';
	case KEY_7:             return shift ? '&'  : '7';
	case KEY_8:             return shift ? '*'  : '8';
	case KEY_9:             return shift ? '('  : '9';
	case KEY_0:             return shift ? ')'  : '0';

	case KEY_ENTER:         return 0x0D;  /* CR: the OS-9 line terminator
	                                       * (and the guest input contract) */
	case KEY_ESCAPE:        return 0x1B;
	case KEY_BACKSPACE:     return 0x08;
	case KEY_TAB:           return '\t';
	case KEY_SPACE:         return ' ';

	case KEY_MINUS:         return shift ? '_'  : '-';
	case KEY_EQUAL:         return shift ? '+'  : '=';
	case KEY_BRACKET_LEFT:  return shift ? '{'  : '[';
	case KEY_BRACKET_RIGHT: return shift ? '}'  : ']';
	case KEY_BACKSLASH:     return shift ? '~'  : '#';   /* UK #/~ (left of Enter) */
	case KEY_EUROPE_1:      return shift ? '~'  : '#';   /* some UK kbds report #/~ here */
	case KEY_SEMICOLON:     return shift ? ':'  : ';';
	case KEY_APOSTROPHE:    return shift ? '@'  : '\'';  /* UK '/@ */
	case KEY_GRAVE:         return shift ? 0xAA : '`';   /* UK `/¬ (¬ = CP437 0xAA) */
	case KEY_COMMA:         return shift ? '<'  : ',';
	case KEY_PERIOD:        return shift ? '>'  : '.';
	case KEY_SLASH:         return shift ? '?'  : '/';
	case KEY_EUROPE_2:      return shift ? '|'  : '\\';  /* UK ISO \| (left of Z) */
	default:                return 0;
	}
}

/* Previous report's held keys, to detect key-down transitions. */
static uint8_t kbd_prev[6];
static bool    caps_lock;

static bool
was_held(uint8_t kc)
{
	for (int i = 0; i < 6; i++)
		if (kbd_prev[i] == kc)
			return true;
	return false;
}

void
hid_cook_keyboard(const uint8_t *report, unsigned len)
{
	if (report == nullptr || len < HID_KBD_REPORT_LEN)
		return;

	const uint8_t mod = report[0];
	/* report[1] is reserved. */
	const uint8_t *keys = &report[2];

	const bool shift = (mod & (MOD_LEFTSHIFT | MOD_RIGHTSHIFT)) != 0;
	const bool ctrl  = (mod & (MOD_LEFTCTRL  | MOD_RIGHTCTRL))  != 0;

	for (int i = 0; i < 6; i++) {
		const uint8_t kc = keys[i];
		if (kc == 0)
			continue;
		if (was_held(kc))
			continue;	/* held from last report, not a new down */

		if (kc == KEY_CAPS_LOCK) {
			caps_lock = !caps_lock;	/* toggle on press */
			continue;
		}

		uint8_t c = uk_cook(kc, shift, caps_lock);
		/* Ctrl chords: fold letters (and the usual @[\]^_ range) onto
		 * the C0 controls the shell's editing characters live in. */
		if (ctrl && c != 0) {
			uint8_t u = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 0x20) : c;
			if (u >= 0x40 && u <= 0x5F)
				c = (uint8_t)(u & 0x1F);
		}
		if (c != 0)
			emit_key(c);
	}

	for (int i = 0; i < 6; i++)
		kbd_prev[i] = keys[i];
}

/* ======================================================================== *
 *  Mouse -- absolute cursor from relative HID deltas.
 * ======================================================================== */
static int cursor_x = FB_WIDTH  / 2;	/* pixels, clamped to screen */
static int cursor_y = FB_HEIGHT / 2;

static inline int clampi(int v, int lo, int hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

#if USB_HOST_ECHO
/* Only the console echo wants cells; the guest is told pixels. */
static inline int cur_col(void) { return clampi(cursor_x / CHAR_W, 0, TEXT_COLS - 1); }
static inline int cur_row(void) { return clampi(cursor_y / CHAR_H, 0, TEXT_ROWS - 1); }
#endif

/* Report a button edge on the console. A BRING-UP AID, not a protocol: it was
 * how clicks were first confirmed with no guest driver in existence, and it
 * stays for that. Deliberately no click counting, drag threshold or
 * double-click window -- that timing belongs to the 6809, which derives it
 * from the snapshot's buttons and pressed-since bytes, and a second
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
	if (!echo_enabled)
		return;
	printf("MOUSE %s %s at %d,%d (col %d row %d)\n",
	       name, now ? "down" : "up",
	       cursor_x, cursor_y, cur_col(), cur_row());
#else
	(void)name;
#endif
}

void
hid_cook_mouse(const uint8_t *report, unsigned len)
{
	if (report == nullptr || len < HID_MOUSE_REPORT_MIN)
		return;

	const uint8_t buttons = report[0];
	const int     dx      = (int)(int8_t)report[1];
	const int     dy      = (int)(int8_t)report[2];
	const int     wheel   = (len >= 4u) ? (int)(int8_t)report[3] : 0;
	/* report[4] is pan, when present; nothing consumes it yet. */

	cursor_x = clampi(cursor_x + dx, 0, FB_WIDTH  - 1);
	cursor_y = clampi(cursor_y + dy, 0, FB_HEIGHT - 1);

	static bool was_left, was_right, was_middle;
	report_button("left",   (buttons & MOUSE_BTN_L) != 0, &was_left);
	report_button("right",  (buttons & MOUSE_BTN_R) != 0, &was_right);
	report_button("middle", (buttons & MOUSE_BTN_M) != 0, &was_middle);
#if USB_HOST_ECHO
	if (echo_enabled && wheel)
		printf("MOUSE wheel %+d\n", wheel);
#endif

	/* Publish raw state for the guest: position and buttons, with the
	 * click counting left to the 6809, which is what its driver expects. */
	uint8_t b = 0u;
	if (buttons & MOUSE_BTN_L) b |= VID_MOUSE_BTN_LEFT;
	if (buttons & MOUSE_BTN_R) b |= VID_MOUSE_BTN_RIGHT;
	if (buttons & MOUSE_BTN_M) b |= VID_MOUSE_BTN_MIDDLE;
	vid_link_mouse_update(cursor_x, cursor_y, b, wheel);

	/* Follow the pointer on screen. */
	vidterm_pointer_set(cursor_x, cursor_y);
}

void
hid_cook_reset(void)
{
	for (int i = 0; i < 6; i++)
		kbd_prev[i] = 0;
	caps_lock = false;
	cursor_x = FB_WIDTH  / 2;
	cursor_y = FB_HEIGHT / 2;
}

int  hid_cook_cursor_x(void)  { return cursor_x; }
int  hid_cook_cursor_y(void)  { return cursor_y; }
bool hid_cook_caps_lock(void) { return caps_lock; }

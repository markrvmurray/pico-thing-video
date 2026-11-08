/* hid_cook.cpp -- HID report cooking: raw boot-protocol bytes to guest input.
 *
 * PORTABLE (see hid_cook.h). The USB HID usage IDs and modifier bits are
 * spelled out below rather than taken from tusb.h, which is what lets this
 * file compile host-side. They are fixed by the HID Usage Tables, not by
 * TinyUSB; what usb_host.cpp static_asserts against TinyUSB is the REPORT
 * SIZES, the part that could drift. The usim suite checks every mapping.
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
	KEY_F1 = 0x3A, KEY_F12 = 0x45,
	KEY_INSERT = 0x49, KEY_HOME = 0x4A, KEY_PAGE_UP = 0x4B,
	KEY_DELETE = 0x4C, KEY_END = 0x4D, KEY_PAGE_DOWN = 0x4E,
	KEY_RIGHT = 0x4F, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52,
	KEY_NUM_LOCK = 0x53,
	KEY_KP_SLASH = 0x54, KEY_KP_STAR = 0x55, KEY_KP_MINUS = 0x56,
	KEY_KP_PLUS = 0x57, KEY_KP_ENTER = 0x58,
	KEY_KP_1 = 0x59, KEY_KP_2 = 0x5A, KEY_KP_3 = 0x5B, KEY_KP_4 = 0x5C,
	KEY_KP_5 = 0x5D, KEY_KP_6 = 0x5E, KEY_KP_7 = 0x5F, KEY_KP_8 = 0x60,
	KEY_KP_9 = 0x61, KEY_KP_0 = 0x62, KEY_KP_DOT = 0x63,
	KEY_EUROPE_2 = 0x64,
};

/* EVERY KEY IS ONE BYTE, and the special keys have bytes of their own, with
 * bit 7 set, that nothing else produces:
 *
 *   F1-F12          $80-$8B
 *                   up    down  left  right
 *   arrows, plain   $8C   $8D   $8E   $8F
 *   arrows, Shift   $90   $91   $92   $93
 *   arrows, Ctrl    $94   $95   $96   $97
 *   Home $98   End $99   PgUp $9A   PgDn $9B   Ins $9D   Del $9E
 *
 * ($9C is the pound sign, CP437, and stays so.) The arrows used to send the
 * CoCo keyboard's bytes from vtio's KeyTbl, which ARE control codes: left was
 * Backspace's $08, right Tab's $09, down LF, up FF, Ctrl+Up XOFF and
 * Shift+Left SCF's line-kill -- so a program could not tell an arrow from the
 * key it collided with, and a shell prompt erased its line on Shift+Left.
 * Now Backspace, Tab, LF and FF only ever mean themselves.
 *
 * The modified arrow columns matter (minted pages with Shift+Up/Down and goes
 * to line ends with Ctrl+Left/Right). With both held Shift wins, as in vtio.
 * The editing keys ignore Shift and Ctrl. Delete is the FORWARD delete; $7F
 * is never sent. The function keys are the guest's to spend as it likes --
 * the driver's window switch is the use in view -- and this end takes no
 * opinion on what any of these mean; it guarantees only that each is
 * DISTINGUISHABLE: no other key, and no Alt chord (below), lands on them. */
enum { ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT };

#define KEY_FN_BASE	0x80u	/* F1; F12 is $8B */
#define KEY_ARROW_BASE	0x8Cu	/* plain up; + 4 Shift, + 8 Ctrl */

enum : uint8_t {
	CODE_HOME = 0x98, CODE_END = 0x99, CODE_PAGE_UP = 0x9A,
	CODE_PAGE_DOWN = 0x9B, CODE_INSERT = 0x9D, CODE_DELETE = 0x9E,
};

static inline uint8_t
arrow(int dir, bool shift, bool ctrl)
{
	return (uint8_t)(KEY_ARROW_BASE + (shift ? 4u : ctrl ? 8u : 0u) +
	    (unsigned)dir);
}

/* ALT sets bit 7 on a printable result: $20-$7E become $A0-$FE. It is the
 * CoCo3 convention (Level 2 vtio: "CC Negative bit set if key is an ALT key
 * (high bit set in A)"), and either Alt key does it. It leaves alone what is
 * not printable -- control characters, so CR, Esc, Backspace and Tab, and
 * the Ctrl chords -- and what is already high: the special keys above, the
 * pound $9C and the not sign $AA. So the special keys stay distinguishable
 * from every Alt chord. The one overlap is deliberate: Alt-* is $AA, which
 * is the not sign's byte and the same CP437 character. */
#define KEY_ALT_BIT	0x80u

enum : uint8_t {
	MOD_LEFTCTRL = 0x01, MOD_LEFTSHIFT = 0x02, MOD_LEFTALT = 0x04,
	MOD_RIGHTCTRL = 0x10, MOD_RIGHTSHIFT = 0x20, MOD_RIGHTALT = 0x40,
};

enum : uint8_t {
	MOUSE_BTN_L = 0x01, MOUSE_BTN_R = 0x02, MOUSE_BTN_M = 0x04,
};

/* Keystrokes feed the guest console as plain bytes. */
static inline void
emit_key(uint8_t c)
{
	vid_link_input(c);
}

/* THE KEYBOARD ECHO (`verbose`): one line per event, saying what arrived and
 * what the 6809 was sent for it -- the HID usage (the USB "scancode"), the
 * modifiers held, and the byte, or why there was none. Releases, modifier-only
 * changes and rollover reports are echoed too, because "I pressed a key and
 * nothing happened" has several causes and the line should name which. */
#if USB_HOST_ECHO
static const char *
mod_text(uint8_t mod, char *buf, size_t n)
{
	static const char *const name[8] = {
		"LCtrl", "LShift", "LAlt", "LGUI",
		"RCtrl", "RShift", "RAlt", "RGUI",
	};
	size_t at = 0;

	buf[0] = '\0';
	for (int b = 0; b < 8; b++)
		if ((mod & (1u << b)) != 0u && at < n)
			at += (size_t)snprintf(&buf[at], n - at, "%s%s",
			    at != 0 ? "+" : "", name[b]);
	if (at == 0)
		snprintf(buf, n, "-");
	return buf;
}

/* What the 6809 is sent, as a person would name it. */
static const char *
byte_text(uint8_t c, char *buf, size_t n)
{
	static const char *const dir[4] = { "Up", "Down", "Left", "Right" };
	static const char *const col[3] = { "", "Shift+", "Ctrl+" };

	if (c >= KEY_FN_BASE && c <= KEY_FN_BASE + 11u)
		snprintf(buf, n, "F%u", (unsigned)(c - KEY_FN_BASE + 1u));
	else if (c >= KEY_ARROW_BASE && c < KEY_ARROW_BASE + 12u)
		snprintf(buf, n, "%s%s", col[(c - KEY_ARROW_BASE) / 4u],
		    dir[(c - KEY_ARROW_BASE) % 4u]);
	else if (c == CODE_HOME)
		snprintf(buf, n, "Home");
	else if (c == CODE_END)
		snprintf(buf, n, "End");
	else if (c == CODE_PAGE_UP)
		snprintf(buf, n, "PgUp");
	else if (c == CODE_PAGE_DOWN)
		snprintf(buf, n, "PgDn");
	else if (c == CODE_INSERT)
		snprintf(buf, n, "Ins");
	else if (c == CODE_DELETE)
		snprintf(buf, n, "Del (forward)");
	else if (c == 0x0D)
		snprintf(buf, n, "CR");
	else if (c == 0x08)
		snprintf(buf, n, "BS");
	else if (c == 0x09)
		snprintf(buf, n, "TAB");
	else if (c == 0x1B)
		snprintf(buf, n, "ESC");
	else if (c < 0x20)
		snprintf(buf, n, "^%c", (char)(c + 0x40));
	else if (c == 0x7F)
		snprintf(buf, n, "DEL");
	else if (c == 0x9C)
		snprintf(buf, n, "pound (CP437)");
	else if (c == 0xAA)
		snprintf(buf, n, "not (CP437) / Alt+'*'");
	else if (c < 0x7F)
		snprintf(buf, n, "'%c'", (char)c);
	else if (c >= 0xA0 && c <= 0xFE)
		snprintf(buf, n, "Alt+'%c'", (char)(c & 0x7F));
	else
		snprintf(buf, n, "-");
	return buf;
}

/* `sent` is the byte sent, or -1 with `why` saying why nothing was. */
static void
echo_key(const char *event, uint8_t kc, uint8_t mod, int sent, const char *why)
{
	char m[64], b[32];

	if (!echo_enabled)
		return;
	if (sent >= 0)
		printf("KEY %-4s usage 0x%02X  mod 0x%02X %-14s -> 0x%02X %s\n",
		    event, kc, mod, mod_text(mod, m, sizeof(m)), (unsigned)sent,
		    byte_text((uint8_t)sent, b, sizeof(b)));
	else
		printf("KEY %-4s usage 0x%02X  mod 0x%02X %-14s -> nothing: %s\n",
		    event, kc, mod, mod_text(mod, m, sizeof(m)), why);
}
#else
#define echo_key(event, kc, mod, sent, why)	((void)0)
#endif

/* ======================================================================== *
 *  Keyboard cooking -- UK layout, HID usage -> final character.
 *  Non-ASCII UK glyphs map to the display font's CP437 codepoints:
 *  £ = 0x9C, ¬ = 0xAA.
 * ======================================================================== */
static uint8_t
uk_cook(uint8_t kc, bool shift, bool ctrl, bool caps, bool num)
{
	/* The numeric keypad. With Num Lock on it types the same bytes as the
	 * main keys; off, its 8/2/4/6 are the arrows and 7/1/9/3/0/. the
	 * editing keys (Home, End, PgUp, PgDn, Ins, Del), as on a PC, sending
	 * what the dedicated keys send. 5 sends nothing. The operators and
	 * Enter do not depend on Num Lock. */
	if (kc >= KEY_KP_1 && kc <= KEY_KP_DOT) {
		static const char numeric[] = "1234567890.";

		if (num)
			return (uint8_t)numeric[kc - KEY_KP_1];
		switch (kc) {
		case KEY_KP_8: return arrow(ARROW_UP, shift, ctrl);
		case KEY_KP_2: return arrow(ARROW_DOWN, shift, ctrl);
		case KEY_KP_4: return arrow(ARROW_LEFT, shift, ctrl);
		case KEY_KP_6: return arrow(ARROW_RIGHT, shift, ctrl);
		case KEY_KP_7: return CODE_HOME;
		case KEY_KP_1: return CODE_END;
		case KEY_KP_9: return CODE_PAGE_UP;
		case KEY_KP_3: return CODE_PAGE_DOWN;
		case KEY_KP_0: return CODE_INSERT;
		case KEY_KP_DOT: return CODE_DELETE;
		default:       return 0;
		}
	}

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

	case KEY_F1: case KEY_F1 + 1: case KEY_F1 + 2:  case KEY_F1 + 3:
	case KEY_F1 + 4: case KEY_F1 + 5: case KEY_F1 + 6: case KEY_F1 + 7:
	case KEY_F1 + 8: case KEY_F1 + 9: case KEY_F1 + 10: case KEY_F12:
		return (uint8_t)(KEY_FN_BASE + (kc - KEY_F1));

	case KEY_UP:            return arrow(ARROW_UP, shift, ctrl);
	case KEY_DOWN:          return arrow(ARROW_DOWN, shift, ctrl);
	case KEY_LEFT:          return arrow(ARROW_LEFT, shift, ctrl);
	case KEY_RIGHT:         return arrow(ARROW_RIGHT, shift, ctrl);

	case KEY_HOME:          return CODE_HOME;
	case KEY_END:           return CODE_END;
	case KEY_PAGE_UP:       return CODE_PAGE_UP;
	case KEY_PAGE_DOWN:     return CODE_PAGE_DOWN;
	case KEY_INSERT:        return CODE_INSERT;
	case KEY_DELETE:        return CODE_DELETE;

	case KEY_KP_SLASH:      return '/';
	case KEY_KP_STAR:       return '*';
	case KEY_KP_MINUS:      return '-';
	case KEY_KP_PLUS:       return '+';
	case KEY_KP_ENTER:      return 0x0D;

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
static bool    num_lock = true;	/* the PC's usual state at power-on */

/* AUTO-REPEAT. A USB keyboard does not repeat: it reports which keys are
 * held, and usually only when that changes, so repeat is the host's job --
 * and it has to be THIS host's. The guest receives plain bytes with no
 * key-up, so it never learns that a key is still held and could not repeat
 * one if it tried.
 *
 * One key repeats: the most recent to go down that produced a byte. Any new
 * key-down ends the current repeat (as on a PC), and a release ends it too.
 * Not repeated: Caps Lock, Num Lock, and the function keys -- the last are
 * the guest's to spend on things like switching windows, and a held F-key
 * cycling windows is not a feature. The arrows and editing keys do repeat,
 * as do Alt chords. A repeat produces what the key produces NOW, with the
 * modifiers of the latest report, so pressing Shift mid-repeat changes case.
 *
 * TIME IS PUSHED IN (hid_cook_tick), like the echo flag, so this file stays
 * portable and the simulator can drive a virtual clock. The repeat is ARMED
 * on the first tick after the key-down -- microseconds later on the board --
 * so the report handler needs no clock of its own.
 *
 * A REPEAT WAITS FOR THE GUEST. While the guest has an unread input byte no
 * repeat is queued; it goes out as soon as the byte is taken. Without that, a
 * busy guest lets repeats pile up in the input FIFO and the key keeps
 * "repeating" after it is released -- the cursor overshoots, the scroll runs
 * on. A slow guest therefore gets a slower repeat, never a backlog. */
#define KEY_REPEAT_DELAY_MS	500u
#define KEY_REPEAT_INTERVAL_MS	50u	/* 20 a second */

static uint8_t  rpt_key;	/* usage repeating, 0 = none */
static uint8_t  rpt_mod;	/* modifiers of the latest report */
static bool     rpt_armed;	/* rpt_due is set */
static uint32_t rpt_due;	/* ms: next repeat due */

/* What key `kc` produces with modifiers `mod`: the UK layout, then Ctrl
 * chords folded onto the C0 controls the shell's editing characters live
 * in, then Alt's bit 7 on whatever is still printable. 0 for a key that
 * produces nothing. */
static uint8_t
cook_key(uint8_t kc, uint8_t mod)
{
	const bool shift = (mod & (MOD_LEFTSHIFT | MOD_RIGHTSHIFT)) != 0;
	const bool ctrl  = (mod & (MOD_LEFTCTRL  | MOD_RIGHTCTRL))  != 0;
	const bool alt   = (mod & (MOD_LEFTALT   | MOD_RIGHTALT))   != 0;
	uint8_t c = uk_cook(kc, shift, ctrl, caps_lock, num_lock);

	if (ctrl && c != 0) {
		uint8_t u = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 0x20) : c;
		if (u >= 0x40 && u <= 0x5F)
			c = (uint8_t)(u & 0x1F);
	}
	if (alt && c >= 0x20 && c <= 0x7E)
		c = (uint8_t)(c | KEY_ALT_BIT);
	return c;
}

static inline bool
repeats(uint8_t c)
{
	return c != 0 && (c < KEY_FN_BASE || c > KEY_FN_BASE + 11u);
}

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

	/* ErrorRollOver (and the other two error usages, 0x02 and 0x03): too
	 * many keys are down for the keyboard to say which. It is NOT a report
	 * of what is held, so it must not become the previous state -- it did,
	 * and the next ordinary report then made every held key look newly
	 * pressed and typed each of them again. Ignore it whole; the next real
	 * report carries on from the last real one. */
	for (int i = 0; i < 6; i++)
		if (keys[i] >= 0x01u && keys[i] <= 0x03u) {
#if USB_HOST_ECHO
			if (echo_enabled)
				printf("KEY error usage 0x%02X (%s) -> report"
				    " ignored, held keys unchanged\n", keys[i],
				    keys[i] == 0x01u ? "rollover: too many keys" :
				    keys[i] == 0x02u ? "POST fail" : "undefined");
#endif
			return;
		}

#if USB_HOST_ECHO
	/* Releases, and modifiers changing on their own: nothing is sent for
	 * either, but both change what the next key or repeat will produce. */
	if (echo_enabled) {
		for (int i = 0; i < 6; i++) {
			const uint8_t kc = kbd_prev[i];
			bool still = false;

			if (kc == 0)
				continue;
			for (int j = 0; j < 6; j++)
				still = still || keys[j] == kc;
			if (!still)
				printf("KEY up   usage 0x%02X\n", kc);
		}
		if (mod != rpt_mod) {
			char m[64];

			printf("KEY mods now 0x%02X %s\n", mod,
			    mod_text(mod, m, sizeof(m)));
		}
	}
#endif
	rpt_mod = mod;

	for (int i = 0; i < 6; i++) {
		const uint8_t kc = keys[i];
		if (kc == 0)
			continue;
		if (was_held(kc))
			continue;	/* held from last report, not a new down */

		/* A new key-down, of any kind, ends the current repeat. */
		rpt_key = 0;

		if (kc == KEY_CAPS_LOCK) {
			caps_lock = !caps_lock;	/* toggle on press */
			echo_key("down", kc, mod, -1,
			    caps_lock ? "Caps Lock now ON" : "Caps Lock now OFF");
			continue;
		}
		if (kc == KEY_NUM_LOCK) {
			num_lock = !num_lock;	/* toggle on press */
			echo_key("down", kc, mod, -1,
			    num_lock ? "Num Lock now ON" : "Num Lock now OFF");
			continue;
		}

		const uint8_t c = cook_key(kc, mod);

		if (c != 0) {
			emit_key(c);
			echo_key("down", kc, mod, c, nullptr);
		} else {
			echo_key("down", kc, mod, -1, "no mapping for this key");
		}
		if (repeats(c)) {
			rpt_key = kc;
			rpt_armed = false;	/* the next tick starts the delay */
		}
	}

	/* A released key stops repeating. */
	if (rpt_key != 0) {
		bool held = false;

		for (int i = 0; i < 6; i++)
			held = held || keys[i] == rpt_key;
		if (!held)
			rpt_key = 0;
	}

	for (int i = 0; i < 6; i++)
		kbd_prev[i] = keys[i];
}

void
hid_cook_tick(uint32_t now_ms)
{
	if (rpt_key == 0)
		return;
	if (!rpt_armed) {
		rpt_armed = true;
		rpt_due = now_ms + KEY_REPEAT_DELAY_MS;
		return;
	}
	/* Wrap-safe: the difference is taken unsigned and read signed. */
	if ((int32_t)(now_ms - rpt_due) < 0)
		return;
	if (vid_link_input_pending())
		return;		/* the guest has not taken the last one yet */

	const uint8_t c = cook_key(rpt_key, rpt_mod);

	if (repeats(c)) {
		emit_key(c);
		echo_key("rpt", rpt_key, rpt_mod, c, nullptr);
	}
	/* From NOW, not from when it was due: a repeat held back by a slow
	 * guest must not be followed by a burst catching up. */
	rpt_due = now_ms + KEY_REPEAT_INTERVAL_MS;
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
	num_lock = true;
	rpt_key = 0;
	rpt_mod = 0;
	rpt_armed = false;
	cursor_x = FB_WIDTH  / 2;
	cursor_y = FB_HEIGHT / 2;
}

int  hid_cook_cursor_x(void)  { return cursor_x; }
int  hid_cook_cursor_y(void)  { return cursor_y; }
bool hid_cook_caps_lock(void) { return caps_lock; }
bool hid_cook_num_lock(void)  { return num_lock; }

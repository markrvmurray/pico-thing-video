/* usb_host.cpp — USB HID host: keyboard + mouse (via hub), cooked into a
 * byte-oriented event stream. Compiled only in host builds (USB_HOST_MODE).
 *
 * The 6809 sees a parsimonious register interface: one shared STATUS bit
 * (EVT_AVAIL, !IRQ-backed) plus one DATA port read repeatedly to walk the
 * cooked-event stream. That register/PIO wiring (pop-on-read) is a follow-on,
 * gated on validating the bus path against a real 6809. For first-light this
 * file cooks HID reports into the same event bytes and echoes them to UART0, so
 * the whole host path (hub + HID enumeration + cooking) is bench-validatable
 * without the bus.
 *
 * "Cooked" means the guest never sees raw HID: keystrokes arrive as final
 * characters (UK layout), and the mouse arrives as discrete gestures (click,
 * double-click, drag, scroll) with text-cell coordinates — so mouse motion
 * cannot storm the interrupt.
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

/* ---- Cooked event headers (see the plan's protocol table) ---------------- */
enum : uint8_t {
	EVT_KEY         = 0x01, /* payload: 1 byte cooked key                    */
	EVT_L_CLICK     = 0x10, /* payload: col, row                             */
	EVT_R_CLICK     = 0x11,
	EVT_L_DCLICK    = 0x12,
	EVT_R_DCLICK    = 0x13,
	EVT_L_DRAG      = 0x14,
	EVT_R_DRAG      = 0x15,
	EVT_SCROLL_UP   = 0x16, /* no payload                                    */
	EVT_SCROLL_DOWN = 0x17,
};

/* Screen geometry (FB_WIDTH/FB_HEIGHT, CHAR_W/H, TEXT_COLS/ROWS) comes from
 * video.h — the single source of truth — so the cursor->cell mapping cannot
 * drift from the framebuffer. */

/* ---- Gesture tuning ------------------------------------------------------ */
static constexpr uint32_t DCLICK_MS = 400;  /* max gap between clicks        */
static constexpr int      DRAG_PX   = 4;    /* motion past this = drag       */

/* ======================================================================== *
 *  Event FIFO (byte ring). Single-producer (Core 0 cooking) for now; the
 *  pop-on-read advance IRQ becomes the consumer once the bus is wired. Uses
 *  the same power-of-two ring discipline as the bus write path.
 * ======================================================================== */
static constexpr uint16_t EVT_FIFO_SIZE = 256;         /* power of two       */
static volatile uint8_t   evt_buf[EVT_FIFO_SIZE];
static volatile uint16_t  evt_head = 0;                /* producer           */
static volatile uint16_t  evt_tail = 0;                /* consumer (future)  */

static inline void
evt_push(uint8_t b)
{
	uint16_t next = (uint16_t)((evt_head + 1) & (EVT_FIFO_SIZE - 1));
	if (next == evt_tail)
		return;                                /* full — drop (echo still shows it) */
	evt_buf[evt_head] = b;
	evt_head = next;
}

/* Public: has the event stream got anything? (STATUS.EVT_AVAIL source.) */
extern "C" bool
usb_host_events_pending(void)
{
	return evt_head != evt_tail;
}

/* ======================================================================== *
 *  Cooked-event emitters (push to FIFO + optional UART echo)
 * ======================================================================== */
#if USB_HOST_ECHO
static const char *
evt_name(uint8_t hdr)
{
	switch (hdr) {
	case EVT_L_CLICK:     return "L_CLICK";
	case EVT_R_CLICK:     return "R_CLICK";
	case EVT_L_DCLICK:    return "L_DCLICK";
	case EVT_R_DCLICK:    return "R_DCLICK";
	case EVT_L_DRAG:      return "L_DRAG";
	case EVT_R_DRAG:      return "R_DRAG";
	case EVT_SCROLL_UP:   return "SCROLL_UP";
	case EVT_SCROLL_DOWN: return "SCROLL_DOWN";
	default:              return "?";
	}
}
#endif

static void
emit_key(uint8_t c)
{
	/* M4: keystrokes feed the guest console as plain bytes. The local
	 * event FIFO and UART echo stay as diagnostics. */
	vid_link_input(c);
	evt_push(EVT_KEY);
	evt_push(c);
#if USB_HOST_ECHO
	if (c >= ' ' && c < 0x7F)
		printf("KEY '%c' (0x%02X)\n", (char)c, c);
	else
		printf("KEY 0x%02X\n", c);
#endif
}

static void
emit_coord(uint8_t hdr, int col, int row)
{
	evt_push(hdr);
	evt_push((uint8_t)col);
	evt_push((uint8_t)row);
#if USB_HOST_ECHO
	printf("%s col=%d row=%d\n", evt_name(hdr), col, row);
#endif
}

static void
emit_simple(uint8_t hdr)
{
	evt_push(hdr);
#if USB_HOST_ECHO
	printf("%s\n", evt_name(hdr));
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
 *  Mouse cooking — absolute cursor + gesture recognition.
 * ======================================================================== */
static int cursor_x = FB_WIDTH  / 2;   /* pixels, clamped to screen */
static int cursor_y = FB_HEIGHT / 2;

struct button_state {
	bool     down;
	uint32_t down_ms;
	int      down_x, down_y;           /* pixel position at press */
	bool     dragging;
	int      drag_col, drag_row;       /* last cell a drag event was emitted for */
	uint32_t last_click_ms;            /* for double-click detection */
	int      last_click_col, last_click_row;
};
static button_state btn_left  = {};
static button_state btn_right = {};

static inline int clampi(int v, int lo, int hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

static inline int cur_col(void) { return clampi(cursor_x / CHAR_W, 0, TEXT_COLS - 1); }
static inline int cur_row(void) { return clampi(cursor_y / CHAR_H, 0, TEXT_ROWS - 1); }

/* Handle one button's transitions. drag_hdr/click_hdr/dclick_hdr select L vs R. */
static void
process_button(button_state *b, bool now, uint32_t t,
	uint8_t drag_hdr, uint8_t click_hdr, uint8_t dclick_hdr)
{
	if (now && !b->down) {
		/* press */
		b->down     = true;
		b->down_ms  = t;
		b->down_x   = cursor_x;
		b->down_y   = cursor_y;
		b->dragging = false;
		b->drag_col = cur_col();
		b->drag_row = cur_row();
	} else if (now && b->down) {
		/* held — promote to drag once motion passes the threshold */
		int dx = cursor_x - b->down_x;
		int dy = cursor_y - b->down_y;
		if (!b->dragging && (dx*dx + dy*dy) >= DRAG_PX*DRAG_PX)
			b->dragging = true;
		if (b->dragging) {
			int col = cur_col(), row = cur_row();
			if (col != b->drag_col || row != b->drag_row) {
				b->drag_col = col;
				b->drag_row = row;
				emit_coord(drag_hdr, col, row);   /* rate-limited: only on cell change */
			}
		}
	} else if (!now && b->down) {
		/* release */
		b->down = false;
		if (!b->dragging) {
			int col = cur_col(), row = cur_row();
			bool near = (col == b->last_click_col && row == b->last_click_row);
			if (near && (t - b->last_click_ms) <= DCLICK_MS) {
				emit_coord(dclick_hdr, col, row);
				b->last_click_ms = 0;             /* consumed — no triple chaining */
			} else {
				emit_coord(click_hdr, col, row);
				b->last_click_ms   = t;
				b->last_click_col  = col;
				b->last_click_row  = row;
			}
		}
		b->dragging = false;
	}
}

static void
process_mouse(const hid_mouse_report_t *r)
{
	uint32_t t = to_ms_since_boot(get_absolute_time());

	/* Accumulate relative motion into the absolute cursor, clamp to screen. */
	cursor_x = clampi(cursor_x + r->x, 0, FB_WIDTH  - 1);
	cursor_y = clampi(cursor_y + r->y, 0, FB_HEIGHT - 1);

	process_button(&btn_left,  (r->buttons & MOUSE_BUTTON_LEFT)  != 0, t,
		EVT_L_DRAG, EVT_L_CLICK, EVT_L_DCLICK);
	process_button(&btn_right, (r->buttons & MOUSE_BUTTON_RIGHT) != 0, t,
		EVT_R_DRAG, EVT_R_CLICK, EVT_R_DCLICK);

	/* Wheel: one event per notch. */
	int w = r->wheel;
	while (w > 0) { emit_simple(EVT_SCROLL_UP);   w--; }
	while (w < 0) { emit_simple(EVT_SCROLL_DOWN); w++; }

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

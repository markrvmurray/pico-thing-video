/* usb_host.cpp -- USB HID host: the TinyUSB stack half. Compiled only in host
 * builds (USB_HOST_MODE).
 *
 * Deliberately thin. Everything that decides what a report MEANS -- the UK
 * keyboard layout, the absolute cursor, what reaches the guest -- lives in the
 * portable hid_cook.cpp, which the simulator compiles and tests with synthetic
 * reports. This file owns only what needs the hardware: enumeration, the boot
 * protocol, and handing the bytes over.
 *
 * That split exists because the one defect that reached the bench (three-byte
 * boot mouse reports being discarded) was in the only guest-facing code with
 * no automated coverage.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cstdint>
#include <cstdio>

#include "tusb.h"

#include "hid_cook.h"
#include "usb.h"

/* The cooker spells out the HID usage IDs and modifier bits itself, so that it
 * compiles without tusb.h. Pin them to TinyUSB's own values here: if the two
 * ever disagree, the build stops rather than the keyboard quietly producing
 * the wrong characters. */
static_assert(HID_KBD_REPORT_LEN == sizeof(hid_keyboard_report_t),
	"boot keyboard report size disagrees with TinyUSB");
static_assert(HID_MOUSE_REPORT_MAX == sizeof(hid_mouse_report_t),
	"boot mouse report size disagrees with TinyUSB");

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
	if (hid_mounts == 0) {
		/* Nothing left to hold a key down. Without this, a keyboard
		 * unplugged mid-keystroke leaves that key "held" for ever, and
		 * the next one plugged in cannot produce that character until
		 * some other key displaces it from the six-slot report. */
		hid_cook_reset();
	}
}

extern "C" void
tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
	uint8_t const *report, uint16_t len)
{
	uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

	if (proto == HID_ITF_PROTOCOL_KEYBOARD && len >= HID_KBD_REPORT_LEN) {
		hid_cook_keyboard(report, len);
	} else if (proto == HID_ITF_PROTOCOL_MOUSE && len >= HID_MOUSE_REPORT_MIN) {
		/* A boot mouse reports THREE bytes; wheel and pan are later
		 * additions many mice never send, so the struct size is an
		 * upper bound, not a minimum. The cooker takes what is there. */
		hid_cook_mouse(report, len);
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

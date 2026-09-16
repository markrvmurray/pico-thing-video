/* usb.c – FreeBSD style */

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "usb.h"
#include "pico_thing_video.h"

// --- USB optional init / force enumeration ---
void
usb_init(void)
{
	// Brief delay for host detection (macOS especially)
	sleep_ms(100);

	// Reset USB PHY to ensure clean state after debugger flash
	reset_block(RESETS_RESET_USBCTRL_BITS);
	unreset_block(RESETS_RESET_USBCTRL_BITS);
	sleep_ms(50);

	// Initialize board pins (USB DP/DM, etc.)
	board_init();

#if defined(PIMORONI_PGA2350)
	// v2.0 PCB USB-C front end: one Role GPIO switches the CC mux (Rp vs Rd)
	// and the TPS2051B VBUS switch together, so driving it to match the
	// build-time role is the whole host/device changeover. Do it before the
	// stack starts: a host must be sourcing VBUS before anything can attach.
	gpio_init(GPIO_USB_ROLE);
#if USB_HOST_MODE
	gpio_put(GPIO_USB_ROLE, 1);
#else
	gpio_put(GPIO_USB_ROLE, 0);
#endif
	gpio_set_dir(GPIO_USB_ROLE, GPIO_OUT);
	gpio_init(GPIO_PWR_FAULT);	// !FLT input; the board pulls it up
#endif

	// Check USB clock frequency
	uint usb_freq = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
	if (usb_freq != 48000)
		printf("ERROR: USB clock is %u kHz and not 48000 kHz\n",usb_freq);

	// TinyUSB stack — role is fixed at build time (single native PHY).
	tusb_rhport_init_t rh_init = {
#if USB_HOST_MODE
		.role = TUSB_ROLE_HOST,
#else
		.role = TUSB_ROLE_DEVICE,
#endif
		.speed = TUSB_SPEED_AUTO
	};
	tusb_init(0, &rh_init);

	// Optional board callback after TinyUSB init
	if (board_init_after_tusb)
		board_init_after_tusb();

	// USB runs at the default mid-range priority (0x80): its ISR is long and
	// bursty, so it sits below the bus (0x40) and video (0x00) DMA handlers and
	// yields to them, without being pinned to the absolute floor.
	// (Cortex-M33: higher number = lower priority.)
	irq_set_priority(USBCTRL_IRQ, 0x80u);

#if !USB_HOST_MODE
	// Force clean enumeration (device role only — the host does not present
	// itself, it powers and enumerates whatever is plugged in).
	sleep_ms(100);
	tud_disconnect();
	sleep_ms(100);
	tud_connect();

	// Optional extra delay for host
	sleep_ms(50);
#endif
}

#if !USB_HOST_MODE

#ifdef USB_DEBUG
static void
cdc_loopback(void)
{
	uint8_t buf[64];
	uint32_t count;

	if (!tud_cdc_connected())
		return;

	if (tud_cdc_available() > 0) {
		count = tud_cdc_read(buf, sizeof(buf));
		if (count > 0) {
			tud_cdc_write(buf, count);
			tud_cdc_write_flush();
		}
	}
}
#endif


// Called when the user sends serial data down the Pico's main USB/Power route.
// Hand the virtual UART to the guest CPU.
uint16_t
usb_cdc_read(uint8_t *buf, const uint16_t buf_len)
{
	if (!tud_cdc_connected())
		return 0u;

	if (tud_cdc_available() > 0)
		return tud_cdc_read(buf, buf_len);
	return 0u;
}

uint16_t
usb_cdc_write(const uint8_t *buf, const uint16_t buf_len)
{
	uint16_t ret_val = 0u;
	if (!tud_cdc_connected())
		return 0u;
	if (!tud_cdc_ready())
		return 0u;

	if (buf_len > 0)
		ret_val = tud_cdc_write(buf, buf_len);
	return ret_val;
}

void
usb_cdc_flush(void)
{
	if (tud_cdc_connected())
		tud_cdc_write_flush();
}

uint16_t
usb_cdc_read_n(uint8_t itf, uint8_t *buf, const uint16_t buf_len)
{
	if (!tud_cdc_n_connected(itf))
		return 0u;

	if (tud_cdc_n_available(itf) > 0)
		return tud_cdc_n_read(itf, buf, buf_len);
	return 0u;
}

uint16_t
usb_cdc_write_n(uint8_t itf, const uint8_t *buf, const uint16_t buf_len)
{
	uint16_t ret_val = 0u;
	if (!tud_cdc_n_connected(itf))
		return 0u;

	if (buf_len > 0)
		ret_val = tud_cdc_n_write(itf, buf, buf_len);
	return ret_val;
}

void
usb_cdc_flush_n(uint8_t itf)
{
	if (tud_cdc_n_connected(itf))
		tud_cdc_n_write_flush(itf);
}

/* Called when device mounted (enumerated) */
extern "C" void
tud_mount_cb(void)
{
	printf("USB mounted\n");
}

/* Called when device unmounted (disconnected) */
extern "C" void
tud_umount_cb(void)
{
	printf("USB unmounted\n");
}

/* CDC handled by polling; no extra callbacks required for simple loopback */

#else /* USB_HOST_MODE — no CDC in a host build: the guest console stays on UART0.
       * These stubs keep poll_console() and the `usb` command linking. */

uint16_t usb_cdc_read(uint8_t *, const uint16_t) { return 0u; }
uint16_t usb_cdc_write(const uint8_t *, const uint16_t) { return 0u; }
void     usb_cdc_flush(void) { }
uint16_t usb_cdc_read_n(uint8_t, uint8_t *, const uint16_t) { return 0u; }
uint16_t usb_cdc_write_n(uint8_t, const uint8_t *, const uint16_t) { return 0u; }
void     usb_cdc_flush_n(uint8_t) { }

/* Provided by usb_host.cpp: true once at least one HID device is mounted. */
extern "C" bool usb_host_mounted(void);

#endif /* !USB_HOST_MODE */

void
usb_poll(void)
{
#if USB_HOST_MODE
	tuh_task();
#if defined(PIMORONI_PGA2350)
	// TPS2051B !FLT while sourcing VBUS: log each edge, not each poll.
	static bool faulted = false;
	bool fault = !gpio_get(GPIO_PWR_FAULT);
	if (fault != faulted) {
		faulted = fault;
		printf(fault ? "USB: VBUS power fault (overcurrent/overtemp)\n"
		             : "USB: VBUS power fault cleared\n");
	}
#endif
#else
	tud_task();
	// Keep cranking the USB handle I'm waiting for DTR and RTS.
	//connected = (tud_cdc_get_line_state() & 0x03u) == 0x03u;
#ifdef USB_DEBUG
	cdc_loopback();
#endif
#endif
}

bool
usb_is_active(void)
{
#if USB_HOST_MODE
	return usb_host_mounted();
#else
	return (tud_ready());
#endif
}

/* usb.c – FreeBSD style */

#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "usb.h"

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

	// Check USB clock frequency
	uint usb_freq = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
	if (usb_freq != 48000)
		printf("ERROR: USB clock is %u kHz and not 48000 kHz\n",usb_freq);

	// TinyUSB device stack
	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO
	};
	tusb_init(0, &dev_init);

	// Optional board callback after TinyUSB init
	if (board_init_after_tusb)
		board_init_after_tusb();

	// USB runs at the default mid-range priority (0x80): its ISR is long and
	// bursty, so it sits below the bus (0x40) and video (0x00) DMA handlers and
	// yields to them, without being pinned to the absolute floor.
	// (Cortex-M33: higher number = lower priority.)
	irq_set_priority(USBCTRL_IRQ, 0x80u);

	// Force clean enumeration
	sleep_ms(100);
	tud_disconnect();
	sleep_ms(100);
	tud_connect();

	// Optional extra delay for host
	sleep_ms(50);
}

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

void
usb_poll(void)
{
	tud_task();
	// Keep cranking the USB handle I'm waiting for DTR and RTS.
	//connected = (tud_cdc_get_line_state() & 0x03u) == 0x03u;
#ifdef USB_DEBUG
	cdc_loopback();
#endif
}

bool
usb_is_active(void)
{
	return (tud_ready());
}

/* i2c_service.cpp – FreeBSD style */

#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "pico_thing_video.h"
#include "chip_stepping.h"
#include "i2c_service.h"

/* DDC is specified at 100 kHz; the EDID EEPROM in many sinks is no faster. */
#define DDC_I2C          i2c1
#define DDC_BAUDRATE     (100u * 1000u)
#define SCAN_TIMEOUT_US  1000u

extern "C" void
i2c_service_init(void)
{
	i2c_init(DDC_I2C, DDC_BAUDRATE);
	gpio_set_function(GPIO_DDC_SDA, GPIO_FUNC_I2C);
	gpio_set_function(GPIO_DDC_SCL, GPIO_FUNC_I2C);
	gpio_pull_up(GPIO_DDC_SDA);
	gpio_pull_up(GPIO_DDC_SCL);

	/* Hot-plug detect: the sink drives it high (from the +5V we supply)
	 * once its EDID is ready to read. The empty-socket low level wants a
	 * pull-down — but on E9-afflicted steppings (A2) the INTERNAL
	 * pull-down latches at ~2.1V once the pin has been driven high, so
	 * there it is left disabled and the board's external 4k7 on GP9
	 * defines the level. Fixed silicon (A3+) uses the internal pull-down,
	 * which also coexists happily with the 4k7 when both are present. */
	gpio_init(GPIO_HPD);
	gpio_set_dir(GPIO_HPD, GPIO_IN);
	if (chip_has_erratum_e9())
		gpio_disable_pulls(GPIO_HPD);
	else
		gpio_pull_down(GPIO_HPD);
}

/* The addresses HDMI itself assigns on the DDC bus. */
static const char *
ddc_hint(uint8_t addr)
{
	switch (addr) {
	case 0x30:
		return "  (EDID segment pointer)";
	case 0x37:
		return "  (DDC/CI monitor control)";
	case 0x50:
		return "  (EDID EEPROM)";
	case 0x54:
		return "  (SCDC, HDMI 2.x)";
	default:
		return "";
	}
}

extern "C" void
i2c_scan(void)
{
	int found = 0;

	/* HPD first: it is the sink's own "EDID is ready" signal, so it tells
	 * the reader whether an empty scan means "no sink" or "sink present
	 * but DDC unhappy". */
	printf("HPD: %s\n", gpio_get(GPIO_HPD) ?
	    "high (sink present, EDID ready)" : "low (no sink detected)");

	/* Raw line levels, as a hardware diagnostic. A powered sink's DDC
	 * pull-ups hold both lines firmly high; "high" on only the weak
	 * internal pull-ups (or a stuck-low line) points at missing +5V on
	 * HDMI pin 18, an adapter without the sideband pins, or soldering. */
	printf("Lines: SDA %s, SCL %s (idle should be high)\n",
	    gpio_get(GPIO_DDC_SDA) ? "high" : "LOW",
	    gpio_get(GPIO_DDC_SCL) ? "high" : "LOW");

	/* Line-charge test: detect EXTERNAL pull-ups without a meter. Drive
	 * the line low, float it with no internal pulls (E9-safe: nothing
	 * holds it in the latch band), and sample a few microseconds later.
	 * A powered sink's ~2k DDC pull-ups snap it high immediately; a dead
	 * sideband (no +5V at HDMI pin 18, an adapter without pins 15/16, a
	 * dry joint) leaves the line's capacitance sitting near ground. */
	static const struct { const char *name; uint gpio; } ddc_lines[] = {
		{ "SDA", GPIO_DDC_SDA },
		{ "SCL", GPIO_DDC_SCL },
	};
	for (unsigned i = 0; i < 2u; i++) {
		const uint g = ddc_lines[i].gpio;
		gpio_init(g);			/* SIO, input, no pulls */
		gpio_disable_pulls(g);
		gpio_put(g, false);
		gpio_set_dir(g, GPIO_OUT);	/* discharge the line */
		busy_wait_us(10);
		gpio_set_dir(g, GPIO_IN);	/* float */
		busy_wait_us(5);
		printf("%s external pull-up: %s\n", ddc_lines[i].name,
		    gpio_get(g) ? "YES (sink DDC side is powered)"
		                : "no (line stayed low)");
		gpio_set_function(g, GPIO_FUNC_I2C);	/* restore the bus */
		gpio_pull_up(g);
	}

	printf("Scanning I2C1 (HDMI DDC, SDA GP%d / SCL GP%d) 08-77...\n",
	       GPIO_DDC_SDA, GPIO_DDC_SCL);
	/* 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec; probing them
	 * yields general-call/hs-mode artefacts, not devices, so skip them. */
	for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
		uint8_t byte;
		const int ret = i2c_read_timeout_us(DDC_I2C, addr, &byte, 1u,
		    false, SCAN_TIMEOUT_US);
		if (ret >= 0) {
			printf("  %02X: ACK%s\n", addr, ddc_hint(addr));
			found++;
		}
	}
	if (found == 0)
		printf("  no devices responded (is a sink attached and the "
		    "main 5V rail up?)\n");
	else
		printf("%d device%s found\n", found, found == 1 ? "" : "s");
}

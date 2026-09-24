/* console.cpp -- the interactive serial console: command dispatch, status
 * reporting, and the non-blocking input poll.
 *
 * The SLOW PATH, deliberately kept away from the bus engine. Nothing here has
 * a deadline: it runs on Core 0 between power sampling and rendering, and it
 * stays alive in standby when main power is absent and the bus engine has
 * never been started.
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "bus_engine.h"
#include "chip_stepping.h"
#include "command_parser.h"
#include "console.h"
#include "hid_cook.h"
#include "i2c_service.h"
#include "pico_thing_video.h"
#include "power.h"
#include "registers.h"
#include "usb.h"
#include "vid_link.h"
#include "win_store.h"

extern "C" {
typedef struct yy_buffer_state *YY_BUFFER_STATE;
extern YY_BUFFER_STATE cmd_yy_scan_string(const char *);
extern void            cmd_yy_delete_buffer(YY_BUFFER_STATE);
extern int             cmd_yyparse(parsed_cmd_t *);
extern void            cmd_yy_reset_str_pool(void);
}

#define COMMAND_BUFFER_LEN 256u

volatile bool verbose = false;
volatile bool input_flood = false;


#define ANSI_CLEAR_SCREEN "\033[2J"
#define ANSI_CURSOR_HOME   "\033[H"

static void
print_power()
{
	const uint32_t mv = power_millivolts();
	printf("Main 5V:     %lu.%02lu V (%u counts) (%s) — %s\n",
	       (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 10u),
	       (unsigned)power_rail_raw(),
	       power_is_on() ? "present" : "absent",
	       bus_engine_serving() ? "bus and video serving" : "standby, console only");

	/* Die temperature, flagged rather than presented bare. Both ADC
	 * channels are uncalibrated against this board's real reference, and
	 * the main pico found both shift while it serves its bus -- the rail
	 * high, the die far too cold. Until that is characterised here, a
	 * figure taken while serving is an indication, not a measurement, and
	 * saying so beats printing a number that invites trust. */
	const int32_t dc = power_die_decicelsius();
	const int32_t ad = dc < 0 ? -dc : dc;
	printf("Die temp:    %s%ld.%ld C (%u counts, uncalibrated%s)\n",
	       dc < 0 ? "-" : "", (long)(ad / 10), (long)(ad % 10),
	       (unsigned)power_die_raw(),
	       bus_engine_serving() ? "; reads cold while serving" : "");
}

static void
print_status()
{
#if USB_HOST_MODE
	printf("Verbose:     %s (HID input echo)\n", verbose ? "on" : "off");
#else
	printf("Verbose:     %s (nothing to echo in a device build)\n",
	       verbose ? "on" : "off");
#endif
	chip_stepping_print();

	/* The clock tree, MEASURED rather than assumed. Two bugs in a row came
	 * from a clock quietly moving when a PLL was re-pointed underneath it:
	 * the console at four times its baud, then the ADC sampling four times
	 * too fast and under-reading. Both were invisible in the source, which
	 * said what it intended rather than what the hardware was doing. Each
	 * count takes a millisecond or so, which is nothing beside the cost of
	 * printing this page. */
	printf("Clocks:      sys %lu  peri %lu  usb %lu  adc %lu  hstx %lu kHz\n",
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX));
	print_power();
	printf("!RESET:      %s\n", RESET_IS_ASSERTED ? "asserted" : "released");
	printf("!IRQ:        %s (link: %s)\n",
	       IRQ_IS_ASSERTED ? "asserted" : "released",
	       vid_link_irq_asserted() ? "asserting" : "idle");
	bus_engine_print_status();
	core0_load_print();
	win_store_print_status();
	vid_link_print_status();
	vid_link_mouse_print();
	registers::dump();
}

/* cmd_parse() - glue between process_command() and the flex/bison parser */
bool
cmd_parse(char *input, parsed_cmd_t *out)
{
	out->tag = CMD_NONE;
	cmd_yy_reset_str_pool();
	YY_BUFFER_STATE buf = cmd_yy_scan_string(input);
	int rc = cmd_yyparse(out);
	cmd_yy_delete_buffer(buf);
	/* YYERROR in grammar actions does not invoke yyerror(), so cmd.tag may
	 * remain CMD_NONE on failure.  Normalise to CMD_SYNTAX_ERROR. */
	if (rc != 0 && out->tag == CMD_NONE)
		out->tag = CMD_SYNTAX_ERROR;
	return rc == 0;
}

static void
process_command(char *buf)
{
	uint8_t buffer[64];
	parsed_cmd_t cmd;

	/* Skip blank lines */
	while (isspace(static_cast<unsigned char>(*buf)))
		buf++;
	if (*buf == '\0')
		return;

	if (!cmd_parse(buf, &cmd))
		return; /* error message already printed by parser */

	switch (cmd.tag) {

	case CMD_FLOOD:
		/* A measurement hook, not a feature. Expensive Data reads need
		 * the input FIFO non-empty, and loopback is the only guest-
		 * reachable way to fill it -- but loopback diverts the command
		 * stream away from the console, so the renderer draws nothing
		 * and the two conditions we most want together are mutually
		 * exclusive from the guest side. Feeding the FIFO from here
		 * uses the ordinary keyboard producer, so the guest can write
		 * a rendering command stream and still find a byte waiting on
		 * every read. It is what a very fast typist would do. */
		input_flood = !input_flood;
		printf("Input flood %s%s\n", input_flood ? "on" : "off",
		       input_flood ? " — guest reads now take the pop path" : "");
		break;

	case CMD_VERBOSE:
		verbose = !verbose;
#if USB_HOST_MODE
		/* Say what it actually does. A toggle that reports only its own
		 * state leaves the operator unable to tell "quiet" from
		 * "broken" -- which is what this command did for months while
		 * driving nothing at all. */
		hid_cook_set_echo(verbose);
		printf("HID input echo %s\n", verbose ? "on" : "off");
#else
		printf("Verbose %s — but this is a device build, so there is no"
		       " HID input to echo\n", verbose ? "on" : "off");
#endif
		break;

	case CMD_STATUS: {
		print_status();
		/* Optional repeat: redraw every period ms until any key. */
		uint32_t period = cmd.status.period;
		while (period > 0u) {
			for (uint32_t i = 0u; i < period; i++) {
				usb_poll();
				power_task();
				if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
					goto status_done;
				sleep_ms(1u);
			}
			printf("%s%s", ANSI_CLEAR_SCREEN, ANSI_CURSOR_HOME);
			print_status();
		}
status_done:
		break;
	}

	case CMD_HELP:
		// Keep this in step with the grammar in command_parser.y and the
		// keyword table in command_lexer.l.  Every command has a two-letter
		// alias except "help"; keywords and hex are case-insensitive.
		printf(
		"\nAll numbers are hex unless stated.  Two-letter aliases in brackets.\n"
		"\n--- Registers ---\n"
		"  examine            (ex)  dump the 8-byte register window\n"
		"  modify <off> <b>.. (mo)  write bytes from offset off (0..7)\n"
		"  fill [v]           (fi)  fill the window with v (default 00)\n"
		"\n--- Host ---\n"
		"  status [period]    (st)  status; period (decimal ms) repeats\n"
		"                           until a key is pressed\n"
		"  power              (pw)  show the sensed main 5V rail\n"
		"  i2c                (ic)  report HPD and scan the HDMI DDC bus\n"
#if !USB_HOST_MODE
		"  usb <args>         (ub)  send words to the USB CDC guest console\n"
#endif
		"  verbose            (vb)  echo HID keys and clicks to this console\n"
		"  flood              (fl)  feed the input FIFO continuously (measurement)\n"
		"  help               (h)   this text\n"
		"\nThe peripheral follows the main 5V rail: bus and video serve only\n"
		"while it is up; on power loss the firmware restarts into standby\n"
		"(console only) and waits for power to return.\n\n");
		break;

	case CMD_POWER:
		print_power();
		break;

	case CMD_I2C_SCAN:
		i2c_scan();
		break;

	case CMD_EXAMINE:
		registers::dump();
		break;

	case CMD_MODIFY: {
		const uint8_t off   = cmd.modify.start & registers::register_mask;
		const uint8_t count = cmd.modify.count;
		for (uint8_t i = 0; i < count; i++) {
			const uint8_t loc = (off + i) & registers::register_mask;
			const uint8_t val = cmd.modify.data[i];
			registers::write(loc) = val;	// write side
			reg[loc] = val;			// mirror to read side
			bus_engine_inject_write(loc, val); // Core 1 dispatch sees it
			printf("%02X: %02X\n", loc, val);
		}
		break;
	}

	case CMD_FILL: {
		const uint8_t val = cmd.fill.value;
		for (uint16_t i = 0u; i < registers::register_length; i++) {
			registers::write(i) = val;
			reg[i] = val;
		}
		printf("Filled register window with %02X\n", val);
		break;
	}

#if !USB_HOST_MODE
	case CMD_USB: {
		/* Send each space-separated word as a separate USB CDC write. */
		char args[sizeof(cmd.usb.args)];
		memcpy(args, cmd.usb.args, sizeof(args));
		char *ptr = args;
		char *word;
		while ((word = strsep(&ptr, " \t")) != nullptr) {
			if (*word == '\0')
				continue;
			snprintf(reinterpret_cast<char *>(buffer), sizeof(buffer), "%s\r\n", word);
			usb_cdc_write_n(0, buffer, strlen(reinterpret_cast<char *>(buffer)));
		}
		usb_cdc_flush_n(0);
		break;
	}
#endif /* !USB_HOST_MODE */

	default:
		break;
	}
}

static constexpr char prompt[] = "==> ";
static bool console_prompted = false;

// One console iteration: show the prompt if needed, pump USB, handle at most
// one input character. Non-blocking, so the caller's loop can interleave
// other Core 0 work (power sensing in particular) between characters. The
// console runs in standby too — it must stay alive when main power is off.
void
console_poll(void)
{
	static char buf[COMMAND_BUFFER_LEN];
	static int idx = 0;

	if (!console_prompted) {
		printf(prompt);
		console_prompted = true;
	}
	// Poll USB before reading a character so that all chars delivered by
	// this tud_task() call are visible to getchar_timeout_us().
	usb_poll();
	const int ch = getchar_timeout_us(0);
	if (ch != PICO_ERROR_TIMEOUT) {
		if (ch == '\r' || ch == '\n') {
			putchar('\n');
			buf[idx] = '\0';
			process_command(buf);
			idx = 0;
			console_prompted = false;
		} else if (ch == '\b' || ch == 0x7F) {
			if (idx > 0) {
				putchar('\b');
				putchar(' ');
				putchar('\b');
				buf[--idx] = '\0';
			}
		} else if (idx < static_cast<int>(COMMAND_BUFFER_LEN) - 1 &&
			   ch >= ' ' && ch < 0x7F) {
			putchar(ch);
			buf[idx++] = ch;
		}
	}
}


// Force the prompt to be reprinted, after something else has written to the
// console (the standby banner, for one).
void
console_reprompt(void)
{
	console_prompted = false;
}

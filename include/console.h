/* console.h -- the interactive serial console (UART0).
 *
 * Copyright (c) 2026 Mark R V Murray.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* One console iteration: prompt if needed, pump USB, handle at most one input
 * character. Non-blocking, so the caller's loop can interleave the rest of
 * Core 0's work between characters. Runs in standby too, when the bus engine
 * has never been started. */
void console_poll(void);

/* Reprint the prompt, after something else has written to the console. */
void console_reprompt(void);

/* Print the aon-timer calendar time; boot announces it once. */
void console_print_time(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */

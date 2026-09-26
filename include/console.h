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

/* TRUE once since the last call if the console did something that blocked this
 * core for a human-visible time -- ran a command, or printed a page. The
 * caller uses it to re-baseline its own service-interval measurement, because
 * an operator asking for two kilobytes of status at 115200 baud stalls the
 * loop for most of a fifth of a second and that is not a rendering fault. It
 * was being recorded as one: a worst gap of 465 ms, with LATE beside it, on a
 * board doing nothing else at all. */
bool console_consumed_time(void);

/* Reprint the prompt, after something else has written to the console. */
void console_reprompt(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */

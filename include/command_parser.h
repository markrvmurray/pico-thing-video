/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cmd_tag {
	CMD_NONE = 0,
	CMD_SYNTAX_ERROR,

	CMD_HELP,
	CMD_VERBOSE,
	CMD_FLOOD,
	CMD_PRIORITY,
	CMD_PSRAM,
	CMD_SELFTEST,
	CMD_TRACE,
	CMD_STATUS,
	CMD_POWER,
	CMD_I2C_SCAN,

	CMD_EXAMINE,
	CMD_MODIFY,
	CMD_FILL,

	CMD_USB,
} cmd_tag_t;

typedef struct parsed_cmd {
	cmd_tag_t tag;
	union {
		/* CMD_STATUS */
		struct {
			uint32_t period; /* repeat period in ms; 0 = once */
		} status;
		struct { uint32_t count; } trace;

		/* CMD_MODIFY */
		struct {
			uint16_t start;
			uint8_t  data[64];
			uint8_t  count;
		} modify;

		/* CMD_FILL */
		struct {
			uint8_t value;
		} fill;

		/* CMD_USB */
		struct {
			char args[128]; /* space-separated remainder of line */
		} usb;
	};
} parsed_cmd_t;

/*
 * Parse one command line into *out.
 * Returns false (and prints a message) on syntax error.
 * The input buffer may be modified by the lexer.
 */
bool cmd_parse(char *input, parsed_cmd_t *out);

#ifdef __cplusplus
}
#endif

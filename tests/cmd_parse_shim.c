/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host-side cmd_parse() shim.
 *
 * The firmware defines cmd_parse() in src/pico_thing_video.cpp, which pulls in
 * the whole Pico SDK and cannot be built on the host.  This shim reproduces the
 * exact same parser-driver logic against the natively-compiled flex/bison
 * scanner so the grammar can be unit-tested with Catch2.  Keep it in lockstep
 * with the firmware implementation.
 */

#include <stdbool.h>

#include "command_parser.h"

/* flex/bison generated scanner API (prefix cmd_yy). */
struct yy_buffer_state;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
extern YY_BUFFER_STATE cmd_yy_scan_string(const char *);
extern void            cmd_yy_delete_buffer(YY_BUFFER_STATE);
extern int             cmd_yyparse(parsed_cmd_t *);
extern void            cmd_yy_reset_str_pool(void);

bool
cmd_parse(char *input, parsed_cmd_t *out)
{
	out->tag = CMD_NONE;
	cmd_yy_reset_str_pool();
	YY_BUFFER_STATE buf = cmd_yy_scan_string(input);
	int rc = cmd_yyparse(out);
	cmd_yy_delete_buffer(buf);
	/* YYERROR in grammar actions does not invoke yyerror(), so out->tag may
	 * remain CMD_NONE on failure.  Normalise to CMD_SYNTAX_ERROR. */
	if (rc != 0 && out->tag == CMD_NONE)
		out->tag = CMD_SYNTAX_ERROR;
	return rc == 0;
}

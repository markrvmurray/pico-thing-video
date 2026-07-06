/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bison LALR(1) grammar for the pico-thing-video interactive command parser.
 *
 * Design notes:
 *   - All symbols prefixed with cmd_yy via %define api.prefix.
 *   - Result is passed in via %parse-param so the grammar is reentrant
 *     with respect to the result struct (lexer is still global/stateful).
 *   - Number arguments are carried as raw strings (TOK_NUMBER) and
 *     converted in actions via parse_hex().
 *   - The byte accumulator array (_bytes) is used for the variadic modify
 *     argument list; it is reset at the start of the rule that uses it.
 *   - Range/validity checks are performed in actions; on failure, the
 *     action calls YYERROR (which triggers yyerror() and returns 1).
 */

/*
 * %code requires is placed into the generated header, ensuring that
 * parsed_cmd_t is visible to both the parser and the lexer (which
 * includes the generated header).
 */
%code requires {
#include "command_parser.h"
}

%{
/*
 * %{ %} is placed before #include "command_parser.tab.h" in the generated .c
 * file, so we explicitly include our project header here to make parsed_cmd_t
 * visible for the yyerror forward declaration below.
 */
#include "command_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Forward declaration of lexer function */
extern int  cmd_yylex(void);
void        cmd_yyerror(parsed_cmd_t *result, const char *msg);

/* Parse a hex string (no 0x prefix) into a uint32_t.
 * Returns UINT32_MAX on invalid input (non-hex character found). */
static uint32_t
parse_hex(const char *s)
{
	char *end;
	uint32_t v = (uint32_t)strtoul(s, &end, 16);
	if (*end != '\0')
		return UINT32_MAX;
	return v;
}

/* Static accumulator for the modify byte list */
static uint8_t  _bytes[64];
static int      _byte_count;
%}

/* -----------------------------------------------------------------------
 * Bison directives
 * ----------------------------------------------------------------------- */
%define api.prefix {cmd_yy}

/* Pass the result struct pointer into yyparse() and yyerror() */
%parse-param { parsed_cmd_t *result }

/* Verbose error messages */
%define parse.error verbose

%union {
	char  *str;
	float  fval;
}

/* -----------------------------------------------------------------------
 * Token declarations
 * ----------------------------------------------------------------------- */

/* Command keywords */
%token TOK_VERBOSE
%token TOK_STATUS
%token TOK_EXAMINE
%token TOK_MODIFY
%token TOK_FILL
%token TOK_USB

/* Value tokens */
%token <str>  TOK_NUMBER
%token <str>  TOK_USB_ARGS
%token <str>  TOK_WORD

%%

/* -----------------------------------------------------------------------
 * Top-level: a command line is a single command
 * ----------------------------------------------------------------------- */
start:
	command
  ;

command:
	  verbose_cmd
	| status_cmd
	| examine_cmd
	| modify_cmd
	| fill_cmd
	| usb_cmd
	| TOK_WORD        { printf("Unknown command \"%s\"\n", $1); YYERROR; }
	;

/* -----------------------------------------------------------------------
 * Simple no-argument commands
 * ----------------------------------------------------------------------- */
verbose_cmd:
	TOK_VERBOSE { result->tag = CMD_VERBOSE; }
	;

status_cmd:
	TOK_STATUS { result->tag = CMD_STATUS; }
	;

examine_cmd:
	TOK_EXAMINE { result->tag = CMD_EXAMINE; }
	;

/* -----------------------------------------------------------------------
 * modify <offset> <byte> [<byte> ...]   (offset 0..7, 1..8 bytes)
 * ----------------------------------------------------------------------- */
modify_cmd:
	TOK_MODIFY TOK_NUMBER { _byte_count = 0; } byte_list_ne {
		uint32_t s = parse_hex($2);
		if (s == UINT32_MAX || s > 0x7u) {
			printf("Invalid hex offset '%s' (valid: 0 .. 7)\n", $2);
			YYERROR;
		}
		if (_byte_count > 8) {
			printf("Too many bytes (maximum 8)\n");
			YYERROR;
		}
		result->tag          = CMD_MODIFY;
		result->modify.start = (uint16_t)s;
		result->modify.count = (uint8_t)_byte_count;
		memcpy(result->modify.data, _bytes, (size_t)_byte_count);
	}
	;

byte_list_ne:
	  byte_item
	| byte_list_ne byte_item
	;

byte_item:
	TOK_NUMBER {
		uint32_t v = parse_hex($1);
		if (v == UINT32_MAX || v > 0xFFu) {
			printf("Invalid byte value '%s'\n", $1);
			YYERROR;
		}
		if (_byte_count == 0)
			memset(_bytes, 0, sizeof(_bytes));
		if (_byte_count < 64)
			_bytes[_byte_count] = (uint8_t)v;
		_byte_count++;
	}
	;

/* -----------------------------------------------------------------------
 * fill [ <value> ]   (defaults to 00)
 * ----------------------------------------------------------------------- */
fill_cmd:
	  TOK_FILL {
		result->tag        = CMD_FILL;
		result->fill.value = 0x00u;
	}
	| TOK_FILL TOK_NUMBER {
		uint32_t v = parse_hex($2);
		if (v == UINT32_MAX || v > 0xFFu) {
			printf("Invalid fill byte '%s'\n", $2);
			YYERROR;
		}
		result->tag        = CMD_FILL;
		result->fill.value = (uint8_t)v;
	}
	;

/* -----------------------------------------------------------------------
 * usb <args...>
 * ----------------------------------------------------------------------- */
usb_cmd:
	  TOK_USB TOK_USB_ARGS {
		result->tag = CMD_USB;
		strncpy(result->usb.args, $2, sizeof(result->usb.args) - 1u);
		result->usb.args[sizeof(result->usb.args) - 1u] = '\0';
	}
	| TOK_USB {
		printf("Usage:\n\n> usb <arg> [...]\n\n");
		YYERROR;
	}
	;

%%

/* -----------------------------------------------------------------------
 * yyerror
 * ----------------------------------------------------------------------- */
void
cmd_yyerror(parsed_cmd_t *r, const char *msg)
{
	printf("Parse error: %s\n", msg);
	if (r)
		r->tag = CMD_SYNTAX_ERROR;
}

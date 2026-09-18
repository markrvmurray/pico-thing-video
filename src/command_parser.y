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
#include <limits.h>

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

/* Parse a decimal string into a long.
 * Returns LONG_MIN on invalid input (non-decimal character found). */
static long
parse_dec(const char *s)
{
	char *end;
	long v = strtol(s, &end, 10);
	if (*end != '\0')
		return LONG_MIN;
	return v;
}

/* Static accumulator for the modify byte list */
static uint8_t  _bytes[64];
static int      _byte_count;

/* Static accumulators for the time argument list */
static int _t_year, _t_month, _t_day, _t_hour, _t_minute, _t_second, _t_count;
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
%token TOK_HELP
%token TOK_I2C
%token TOK_POWER
%token TOK_TIME
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
	  help_cmd
	| i2c_cmd
	| power_cmd
	| time_cmd
	| verbose_cmd
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
help_cmd:
	TOK_HELP { result->tag = CMD_HELP; }
	;

power_cmd:
	TOK_POWER { result->tag = CMD_POWER; }
	;

i2c_cmd:
	TOK_I2C { result->tag = CMD_I2C_SCAN; }
	;

verbose_cmd:
	TOK_VERBOSE { result->tag = CMD_VERBOSE; }
	;

/* -----------------------------------------------------------------------
 * status [ <period> ]   (period in decimal ms; repeats until a key)
 * ----------------------------------------------------------------------- */
status_cmd:
	  TOK_STATUS {
		result->tag           = CMD_STATUS;
		result->status.period = 0u;
	}
	| TOK_STATUS TOK_NUMBER {
		long v = parse_dec($2);
		if (v == LONG_MIN || v < 0) {
			printf("Invalid period '%s' (decimal ms)\n", $2);
			YYERROR;
		}
		result->tag           = CMD_STATUS;
		result->status.period = (uint32_t)v;
	}
	;

/* -----------------------------------------------------------------------
 * time [ <year> [<month> [<day> [<hour> [<minute> [<second>]]]]] ]
 * All arguments are decimal.
 * ----------------------------------------------------------------------- */
time_cmd:
	  TOK_TIME {
		result->tag = CMD_TIME_QUERY;
	}
	| TOK_TIME time_args {
		result->tag         = CMD_TIME_SET;
		result->time.year   = _t_year;
		result->time.month  = _t_month;
		result->time.day    = _t_day;
		result->time.hour   = _t_hour;
		result->time.minute = _t_minute;
		result->time.second = _t_second;
		result->time.count  = _t_count;
	}
	;

time_args:
	  time_year
	| time_year time_month
	| time_year time_month time_day
	| time_year time_month time_day time_hour
	| time_year time_month time_day time_hour time_minute
	| time_year time_month time_day time_hour time_minute time_second
	;

time_year:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 2025 || v > 2061) {
			printf("Invalid year '%s' (valid: 2025-2061)\n", $1);
			YYERROR;
		}
		_t_year = (int)v; _t_month = 0; _t_day = 1;
		_t_hour = 0; _t_minute = 0; _t_second = 0; _t_count = 1;
	}
	;

time_month:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 1 || v > 12) {
			printf("Invalid month '%s' (valid: 1-12)\n", $1);
			YYERROR;
		}
		_t_month = (int)(v - 1); /* convert to 0-based for tm_mon */
		_t_count = 2;
	}
	;

time_day:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 1 || v > 31) {
			printf("Invalid day '%s' (valid: 1-31)\n", $1);
			YYERROR;
		}
		_t_day = (int)v; _t_count = 3;
	}
	;

time_hour:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 0 || v > 23) {
			printf("Invalid hour '%s' (valid: 0-23)\n", $1);
			YYERROR;
		}
		_t_hour = (int)v; _t_count = 4;
	}
	;

time_minute:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 0 || v > 59) {
			printf("Invalid minute '%s' (valid: 0-59)\n", $1);
			YYERROR;
		}
		_t_minute = (int)v; _t_count = 5;
	}
	;

time_second:
	TOK_NUMBER {
		long v = parse_dec($1);
		if (v == LONG_MIN || v < 0 || v > 59) {
			printf("Invalid second '%s' (valid: 0-59)\n", $1);
			YYERROR;
		}
		_t_second = (int)v; _t_count = 6;
	}
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

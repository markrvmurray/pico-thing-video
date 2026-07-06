/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host unit tests for the interactive command parser (src/command_lexer.l +
 * src/command_parser.y).  Exercises every command, its two-letter alias,
 * case-insensitivity, argument parsing, and the boundary/rejection cases.
 */

#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "command_parser.h"

namespace {

struct Result {
	bool         ok;
	parsed_cmd_t cmd;
};

// Parse a command line through a mutable buffer (the lexer may write to it).
Result
run(const std::string &line)
{
	char buf[256];
	std::strncpy(buf, line.c_str(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	parsed_cmd_t cmd;
	std::memset(&cmd, 0, sizeof(cmd));
	bool ok = cmd_parse(buf, &cmd);
	return {ok, cmd};
}

} // namespace

TEST_CASE("no-argument commands map to their tags", "[parser]")
{
	CHECK(run("power").cmd.tag   == CMD_POWER);
	CHECK(run("verbose").cmd.tag == CMD_VERBOSE);
	CHECK(run("status").cmd.tag  == CMD_STATUS);
	CHECK(run("examine").cmd.tag == CMD_EXAMINE);

	CHECK(run("power").ok);
	CHECK(run("status").ok);
}

TEST_CASE("power on / off subcommands", "[parser]")
{
	CHECK(run("power on").cmd.tag  == CMD_POWER_ON);
	CHECK(run("power off").cmd.tag == CMD_POWER_OFF);
	CHECK(run("power on").ok);
	CHECK(run("power off").ok);
}

TEST_CASE("two-letter aliases resolve to the same tags", "[parser]")
{
	CHECK(run("pw").cmd.tag     == CMD_POWER);
	CHECK(run("vb").cmd.tag     == CMD_VERBOSE);
	CHECK(run("st").cmd.tag     == CMD_STATUS);
	CHECK(run("ex").cmd.tag     == CMD_EXAMINE);
	CHECK(run("mo 0 1").cmd.tag == CMD_MODIFY);
	CHECK(run("fi").cmd.tag     == CMD_FILL);
	CHECK(run("ub reset").cmd.tag == CMD_USB);

	CHECK(run("pw on").cmd.tag  == CMD_POWER_ON);
	CHECK(run("pw off").cmd.tag == CMD_POWER_OFF);
}

TEST_CASE("keywords and on/off are case-insensitive", "[parser]")
{
	CHECK(run("POWER").cmd.tag   == CMD_POWER);
	CHECK(run("Status").cmd.tag  == CMD_STATUS);
	CHECK(run("Verbose").cmd.tag == CMD_VERBOSE);
	CHECK(run("PW ON").cmd.tag   == CMD_POWER_ON);
	CHECK(run("power Off").cmd.tag == CMD_POWER_OFF);
}

TEST_CASE("modify: offset and a single byte", "[parser][modify]")
{
	Result r = run("modify 10 ff");
	REQUIRE(r.ok);
	REQUIRE(r.cmd.tag == CMD_MODIFY);
	CHECK(r.cmd.modify.start == 0x10);
	CHECK(r.cmd.modify.count == 1);
	CHECK(r.cmd.modify.data[0] == 0xFF);
}

TEST_CASE("modify: numbers are hexadecimal", "[parser][modify]")
{
	Result r = run("modify a 1f");
	REQUIRE(r.ok);
	CHECK(r.cmd.modify.start == 0x0A);
	CHECK(r.cmd.modify.data[0] == 0x1F);
}

TEST_CASE("modify: multiple bytes preserve order", "[parser][modify]")
{
	Result r = run("modify 0 de ad be ef");
	REQUIRE(r.ok);
	REQUIRE(r.cmd.modify.count == 4);
	CHECK(r.cmd.modify.start == 0x00);
	CHECK(r.cmd.modify.data[0] == 0xDE);
	CHECK(r.cmd.modify.data[1] == 0xAD);
	CHECK(r.cmd.modify.data[2] == 0xBE);
	CHECK(r.cmd.modify.data[3] == 0xEF);
}

TEST_CASE("modify: offset boundary 3F ok, 40 rejected", "[parser][modify]")
{
	Result ok = run("modify 3f 1");
	REQUIRE(ok.ok);
	CHECK(ok.cmd.modify.start == 0x3F);

	Result bad = run("modify 40 1");
	CHECK_FALSE(bad.ok);
	CHECK(bad.cmd.tag == CMD_SYNTAX_ERROR);
}

TEST_CASE("modify: byte value boundary FF ok, 100 rejected", "[parser][modify]")
{
	CHECK(run("modify 0 ff").ok);

	Result bad = run("modify 0 100");
	CHECK_FALSE(bad.ok);
	CHECK(bad.cmd.tag == CMD_SYNTAX_ERROR);
}

TEST_CASE("modify: at least one byte is required", "[parser][modify]")
{
	Result r = run("modify 10");
	CHECK_FALSE(r.ok);
}

TEST_CASE("modify: 64 bytes accepted, 65 rejected", "[parser][modify]")
{
	std::string base = "modify 0";
	std::string sixtyfour = base;
	for (int i = 0; i < 64; i++)
		sixtyfour += " 1";
	Result r64 = run(sixtyfour);
	REQUIRE(r64.ok);
	CHECK(r64.cmd.modify.count == 64);

	std::string sixtyfive = sixtyfour + " 1";
	CHECK_FALSE(run(sixtyfive).ok);
}

TEST_CASE("fill defaults to 00 and accepts a value", "[parser][fill]")
{
	Result def = run("fill");
	REQUIRE(def.ok);
	REQUIRE(def.cmd.tag == CMD_FILL);
	CHECK(def.cmd.fill.value == 0x00);

	Result val = run("fill 5a");
	REQUIRE(val.ok);
	CHECK(val.cmd.fill.value == 0x5A);

	CHECK(run("fill ff").cmd.fill.value == 0xFF);
}

TEST_CASE("fill: value over FF is rejected", "[parser][fill]")
{
	Result bad = run("fill 100");
	CHECK_FALSE(bad.ok);
	CHECK(bad.cmd.tag == CMD_SYNTAX_ERROR);
}

TEST_CASE("usb captures the remainder of the line verbatim", "[parser][usb]")
{
	Result one = run("usb reset");
	REQUIRE(one.ok);
	REQUIRE(one.cmd.tag == CMD_USB);
	CHECK(std::string(one.cmd.usb.args) == "reset");

	Result many = run("usb bulk 1 2 3");
	REQUIRE(many.ok);
	CHECK(std::string(many.cmd.usb.args) == "bulk 1 2 3");
}

TEST_CASE("usb with no arguments is rejected", "[parser][usb]")
{
	CHECK_FALSE(run("usb").ok);
}

TEST_CASE("unknown commands are rejected", "[parser]")
{
	Result r = run("frobnicate");
	CHECK_FALSE(r.ok);
	CHECK(r.cmd.tag == CMD_SYNTAX_ERROR);

	CHECK_FALSE(run("xyzzy").ok);
}

TEST_CASE("trailing garbage after a command is rejected", "[parser]")
{
	CHECK_FALSE(run("status junk").ok);
	CHECK_FALSE(run("verbose on").ok);
}

TEST_CASE("empty and whitespace-only input is not a valid command", "[parser]")
{
	CHECK_FALSE(run("").ok);
	CHECK_FALSE(run("   ").ok);
}

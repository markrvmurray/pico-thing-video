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
	CHECK(run("verbose").cmd.tag == CMD_VERBOSE);
	CHECK(run("status").cmd.tag  == CMD_STATUS);
	CHECK(run("examine").cmd.tag == CMD_EXAMINE);

	CHECK(run("verbose").ok);
	CHECK(run("status").ok);
}

TEST_CASE("two-letter aliases resolve to the same tags", "[parser]")
{
	CHECK(run("vb").cmd.tag     == CMD_VERBOSE);
	CHECK(run("st").cmd.tag     == CMD_STATUS);
	CHECK(run("ex").cmd.tag     == CMD_EXAMINE);
	CHECK(run("mo 0 1").cmd.tag == CMD_MODIFY);
	CHECK(run("fi").cmd.tag     == CMD_FILL);
	CHECK(run("ub reset").cmd.tag == CMD_USB);
}

TEST_CASE("keywords are case-insensitive", "[parser]")
{
	CHECK(run("STATUS").cmd.tag  == CMD_STATUS);
	CHECK(run("Status").cmd.tag  == CMD_STATUS);
	CHECK(run("Verbose").cmd.tag == CMD_VERBOSE);
	CHECK(run("EXAMINE").cmd.tag == CMD_EXAMINE);
}

TEST_CASE("time is no longer a command", "[parser]")
{
	// The video device has no timekeeping: there is nothing on this board
	// that needs a wall clock, and the aon-timer cost a peripheral and a
	// seeded date at every boot to answer a question nobody asked. Removed
	// 2026-09-23. Asserted here because the command set is what these
	// tests gate -- the previous entry for "power" sat wrong for months
	// because nothing checked it.
	CHECK_FALSE(run("time").ok);
	CHECK_FALSE(run("ti").ok);
	CHECK_FALSE(run("time 2026 9 23").ok);
}

TEST_CASE("power reports, and takes no arguments", "[parser]")
{
	// "power" was removed with the rest of the supervisor role during the
	// peripheral pivot, then came back as a REPORT-ONLY command when the
	// board gained 5V-rail sensing. This case asserted its absence and was
	// never updated, so it outlived the truth it was testing.
	CHECK(run("power").ok);
	CHECK(run("power").cmd.tag == CMD_POWER);
	CHECK(run("pw").ok);
	CHECK(run("pw").cmd.tag == CMD_POWER);
	CHECK(run("POWER").ok);

	// There is no power CONTROL on this peripheral -- it follows the host
	// rail -- so arguments must still be rejected.
	CHECK_FALSE(run("power on").ok);
	CHECK_FALSE(run("power off").ok);
}

TEST_CASE("modify: offset and a single byte", "[parser][modify]")
{
	Result r = run("modify 5 ff");
	REQUIRE(r.ok);
	REQUIRE(r.cmd.tag == CMD_MODIFY);
	CHECK(r.cmd.modify.start == 0x5);
	CHECK(r.cmd.modify.count == 1);
	CHECK(r.cmd.modify.data[0] == 0xFF);
}

TEST_CASE("modify: byte values are hexadecimal", "[parser][modify]")
{
	Result r = run("modify 7 1f");
	REQUIRE(r.ok);
	CHECK(r.cmd.modify.start == 0x7);
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

TEST_CASE("modify: offset boundary 7 ok, 8 rejected", "[parser][modify]")
{
	Result ok = run("modify 7 1");
	REQUIRE(ok.ok);
	CHECK(ok.cmd.modify.start == 0x7);

	Result bad = run("modify 8 1");
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
	Result r = run("modify 5");
	CHECK_FALSE(r.ok);
}

TEST_CASE("modify: 8 bytes accepted, 9 rejected", "[parser][modify]")
{
	std::string base = "modify 0";
	std::string eight = base;
	for (int i = 0; i < 8; i++)
		eight += " 1";
	Result r8 = run(eight);
	REQUIRE(r8.ok);
	CHECK(r8.cmd.modify.count == 8);

	std::string nine = eight + " 1";
	CHECK_FALSE(run(nine).ok);
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

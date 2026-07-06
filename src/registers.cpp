/**
 * Copyright (c) 2025-2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <cctype>
#include <cstdio>

#include "pico_thing_video.h"
#include "registers.h"

registers &reg = registers::getInstance();

void
registers::copy_out(uint8_t *dst, uint16_t addr, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++)
		dst[i] = read_registers[addr + i];
}

void
registers::copy_out_write(uint8_t *dst, uint16_t addr, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++)
		dst[i] = write_registers[addr + i];
}

void
registers::copy_in(uint16_t addr, const uint8_t *src, uint16_t len)
{
	for (uint16_t i = 0u; i < len; i++)
		read_registers[addr + i] = src[i];
}

void
registers::dump()
{
	// One row of register_length (8) bytes per block, addressed from
	// REGISTER_BASE. The trailing byte leaves room for the ascii gutter.
	static uint8_t printables[register_length + 1u];

	auto print_block = [](const char *label,
	                      void (*fetch)(uint8_t *, uint16_t, uint16_t)) {
		fetch(printables, 0u, register_length);
		printf("%s\n%04X:", label, REGISTER_BASE);
		for (uint16_t j = 0u; j < register_length; j++) {
			auto ch = static_cast<unsigned char>(printables[j]) & 0x7F;
			printf(" %02X", printables[j]);
			printables[j] = std::isprint(ch) ? static_cast<char>(ch) : '.';
		}
		printf(" |%.*s|\n", static_cast<int>(register_length),
		       reinterpret_cast<char *>(printables));
	};

	print_block("Write block:", copy_out_write);
	print_block("Read block:", copy_out);
	printf("--\n");
}

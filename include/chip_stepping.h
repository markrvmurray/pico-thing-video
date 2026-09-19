/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * RP2350 stepping identification, following the main pico's approach:
 * steppings are NAMED BY BOOTROM VERSION, because CHIP_ID.REVISION cannot
 * do it alone — A4 changed only the bootrom, and a real A4 reads revision
 * 0x3 like an A3 (the datasheet says 0x8; a chip that disagrees shows its
 * own). Used to decide erratum applicability at runtime, so one firmware
 * image runs correctly on mixed silicon.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>

#include "pico/platform.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/address_mapped.h"

typedef struct chip_stepping {
	uint8_t rom_version;
	uint8_t revision;	/* datasheet CHIP_ID.REVISION for this stepping */
	const char *name;
} chip_stepping_t;

/* nullptr for a bootrom version the datasheet does not list. */
static inline const chip_stepping_t *
chip_stepping_lookup(uint8_t rom_version)
{
	static const chip_stepping_t steppings[] = {
		{ 2u, 0x2u, "A2" },
		{ 3u, 0x3u, "A3" },
		{ 4u, 0x8u, "A4" },
	};
	for (unsigned i = 0; i < sizeof(steppings) / sizeof(steppings[0]); i++)
		if (steppings[i].rom_version == rom_version)
			return &steppings[i];
	return (const chip_stepping_t *)0;
}

static inline uint8_t
chip_id_revision(void)
{
	const uint32_t chip_id =
	    *(io_ro_32 *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET);
	return (uint8_t)((chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >>
	    SYSINFO_CHIP_ID_REVISION_LSB);
}

/* Erratum E9 (input-buffer leakage latches internally-pulled-down inputs
 * at ~2.1V): fixed in the A3 silicon — A4 changed only the bootrom. Treat
 * A2 and anything older/unknown-below-A3 as affected. */
static inline bool
chip_has_erratum_e9(void)
{
	return rp2350_rom_version() < 3u;
}

/* One-line identification, in the main pico's format. */
static inline void
chip_stepping_print(void)
{
	const uint8_t rom = rp2350_rom_version();
	const uint8_t rev = chip_id_revision();
	const uint32_t package_sel =
	    *(io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET);
	const char part = (package_sel & SYSINFO_PACKAGE_SEL_BITS) ? 'A' : 'B';
	const chip_stepping_t *s = chip_stepping_lookup(rom);

	if (s != (const chip_stepping_t *)0) {
		if (s->revision == rev)
			printf("Chip:        RP2350%c %s (bootrom %u)\n",
			    part, s->name, rom);
		else
			printf("Chip:        RP2350%c %s (bootrom %u, CHIP_ID revision %X)\n",
			    part, s->name, rom, rev);
	} else {
		printf("Chip:        RP2350%c, unknown bootrom %u (CHIP_ID revision %X)\n",
		    part, rom, rev);
	}
}

/**
 * Copyright (c) 2026 Mark R V Murray.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * I2C service on the HDMI DDC pins (I2C1, SDA GP10 / SCL GP11). The DDC
 * lines run straight from the PGA2350 to the HDMI connector (no board
 * pull-ups or level shifting); the attached sink provides the DDC pull-ups
 * to its +5V, which the RP2350's 5V-tolerant inputs accept, and the weak
 * internal pull-ups keep the bus idle-high when nothing is attached.
 * For now this is bus bring-up only: init plus a console-driven scan.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Claim I2C1 on the DDC pins at the 100 kHz DDC rate. */
void i2c_service_init(void);

/* Scan 7-bit addresses 0x08-0x77 and report every one that ACKs, with a
 * note for the addresses HDMI assigns (EDID, segment pointer, SCDC). */
void i2c_scan(void);

#ifdef __cplusplus
}
#endif

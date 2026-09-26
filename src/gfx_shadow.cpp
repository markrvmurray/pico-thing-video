/* gfx_shadow.cpp – FreeBSD style
 *
 * The graphics shadow lives in PSRAM. See gfx_shadow.h for why it exists and
 * why it cannot live in SRAM.
 */

#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "hardware/regs/addressmap.h"

#include "gfx_shadow.h"
#include "win_store.h"		/* shares the probe's verdict */

/* Word-aligned EXPLICITLY. The text blitter reads the plane as 32-bit words at
 * cell boundaries; left to section placement it happened to land on a
 * megabyte boundary, which is luck rather than a guarantee. */
static uint8_t __uninitialized_psram("gfxshadow") shadow[GFX_SHADOW_BYTES]
    __attribute__((aligned(4)));

uint8_t *
gfx_shadow_base(void)
{
	/* The parked-window probe is what proves this part answers at all,
	 * and it runs before Core 1 starts. Trusting it here rather than
	 * probing again keeps one verdict rather than two that could
	 * disagree. */
	if (win_store_slots() == 0u)
		return NULL;

	/* Uncached, for the same reason the window screens are: the XIP cache
	 * is not write-through for PSRAM and the per-core caches are not
	 * coherent. Derived from the cached window rather than written as a
	 * constant, because the same offset applies to both chip selects. */
	return (uint8_t *)((uintptr_t)shadow +
	    (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

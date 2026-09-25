/* gfx_bufs.cpp – FreeBSD style
 *
 * The guest's get/put buffers live in PSRAM. See gfx_bufs.h.
 */

#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"

#include "gfx_bufs.h"
#include "win_store.h"		/* shares the probe's verdict */

static uint8_t __uninitialized_psram("gfxbufs") pool[GFX_BUFPOOL_BYTES];

uint8_t *
gfx_bufpool_base(void)
{
	/* One verdict on whether this part answers, taken by the window
	 * probe before Core 1 starts. Probing again here could only produce
	 * a second answer that disagreed with the first. */
	if (win_store_slots() == 0u)
		return NULL;

	/* Uncached: the XIP cache is not write-through for PSRAM and the
	 * per-core caches are not coherent. Derived rather than written as a
	 * constant, because the same offset serves both chip selects. */
	return (uint8_t *)((uintptr_t)pool +
	    (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

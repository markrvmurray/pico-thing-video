/* fill_log.cpp – FreeBSD style
 *
 * The flood-fill span log lives in PSRAM. See fill_log.h.
 */

#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"

#include "fill_log.h"
#include "win_store.h"		/* shares the probe's verdict */

static uint32_t __uninitialized_psram("filllog") spans[FILL_LOG_SPANS];
static uint32_t __uninitialized_psram("fillseeds") seeds[FILL_SEEDS];

uint32_t *
fill_log_base(void)
{
	if (win_store_slots() == 0u)
		return NULL;
	return (uint32_t *)((uintptr_t)spans +
	    (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

uint32_t *
fill_seeds_base(void)
{
	if (win_store_slots() == 0u)
		return NULL;
	return (uint32_t *)((uintptr_t)seeds +
	    (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

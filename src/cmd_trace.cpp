/* cmd_trace.cpp – FreeBSD style
 *
 * The command trace lives in PSRAM. See cmd_trace.h.
 */

#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"

#include "cmd_trace.h"
#include "win_store.h"		/* shares the probe's verdict */

static uint8_t __uninitialized_psram("cmdtrace") ring[CMD_TRACE_BYTES]
    __attribute__((aligned(4)));

uint8_t *
cmd_trace_base(void)
{
	/* One verdict on whether the part answers, taken by the window probe
	 * before Core 1 starts; asking again could only disagree with it. */
	if (win_store_slots() == 0u)
		return NULL;

	/* Uncached, like every other PSRAM user here: the XIP cache is not
	 * write-through for PSRAM and the per-core caches are not coherent. */
	return (uint8_t *)((uintptr_t)ring +
	    (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

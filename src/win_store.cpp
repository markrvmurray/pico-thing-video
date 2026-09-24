/* win_store.cpp – FreeBSD style
 *
 * Parked text screens, in PSRAM. See win_store.h for why this is an interface.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/address_mapped.h"
#include "hardware/psram.h"
#include "hardware/regs/addressmap.h"

#include "win_store.h"

/* How many parked screens to reserve. The part is 8 MB and a screen is 7,680
 * bytes, so the limit is not memory -- it is that every one of these is probed
 * at bring-up and a switched-shell machine has no use for hundreds. */
#define WIN_STORE_MAX_SLOTS	16u

static uint8_t __uninitialized_psram("winstore")
    slots[WIN_STORE_MAX_SLOTS][WIN_STORE_SLOT_BYTES];

static unsigned usable;		/* proven by the probe, not claimed */
static bool     probed;
static size_t   reported_size;

/* The cached XIP window and its no-allocate alias are a fixed distance apart,
 * and the same distance applies to flash (CS0) and PSRAM (CS1). Deriving it
 * beats hardcoding 0x15000000, which is only correct for one of the two. */
#define XIP_UNCACHED_DELTA	(XIP_NOCACHE_NOALLOC_BASE - XIP_BASE)

static inline uint8_t *
uncached(void *p)
{
	return (uint8_t *)((uintptr_t)p + XIP_UNCACHED_DELTA);
}

/* A pattern that depends on WHERE it is written, which is what turns this from
 * "does the memory work" into "is the memory the size it claims". A part
 * smaller than advertised aliases its address lines, so a later slot lands on
 * an earlier one; identical patterns everywhere would read back perfectly and
 * prove nothing. */
static inline uint32_t
pattern_for(unsigned slot, unsigned word)
{
	return 0xA5000000u ^ (slot * 0x9E3779B9u) ^ word;
}

bool
win_store_init(void)
{
	if (probed)
		return usable > 0u;
	probed = true;
	reported_size = psram_get_size();

	if (reported_size < sizeof(slots)) {
		usable = 0u;
		return false;
	}

	/* Write every slot before reading any of them. Writing and checking one
	 * at a time would pass on an aliasing part, because each read would
	 * find the value its own write had just put there. */
	for (unsigned i = 0u; i < WIN_STORE_MAX_SLOTS; i++) {
		volatile uint32_t *p = (volatile uint32_t *)uncached(slots[i]);
		p[0] = pattern_for(i, 0u);
		p[(WIN_STORE_SLOT_BYTES / 4u) - 1u] =
		    pattern_for(i, (WIN_STORE_SLOT_BYTES / 4u) - 1u);
	}

	for (unsigned i = 0u; i < WIN_STORE_MAX_SLOTS; i++) {
		volatile uint32_t *p = (volatile uint32_t *)uncached(slots[i]);
		const unsigned last = (WIN_STORE_SLOT_BYTES / 4u) - 1u;
		if (p[0] != pattern_for(i, 0u) ||
		    p[last] != pattern_for(i, last))
			break;		/* stop at the first slot that lied */
		usable = i + 1u;
	}

	return usable > 0u;
}

unsigned
win_store_slots(void)
{
	return usable;
}

uint8_t *
win_store_slot(unsigned i)
{
	if (i >= usable)
		return NULL;
	return uncached(slots[i]);
}

/* Turn "read-back failed" into evidence. Three things can produce that from
 * the outside and they want different fixes: the part is not answering at all,
 * the writes are being dropped (the XIP window is not writable), or one alias
 * works and the other does not (the address arithmetic is wrong). Printing
 * what was written beside what came back through BOTH aliases tells them
 * apart in one flash instead of three. */
static void
win_store_diagnose(void)
{
	volatile uint32_t *cached = (volatile uint32_t *)slots[0];
	volatile uint32_t *nocache = (volatile uint32_t *)uncached(slots[0]);
	const uint32_t probe = 0x5A5AA5A5u;

	printf("             probe: slot0 cached %p, uncached %p, in range %s\n",
	       (void *)cached, (void *)nocache,
	       psram_check_address((void *)slots[0]) ? "yes" : "NO");

	*nocache = probe;
	printf("             wrote %08lX uncached -> reads %08lX uncached,"
	       " %08lX cached\n", (unsigned long)probe,
	       (unsigned long)*nocache, (unsigned long)*cached);

	*cached = ~probe;
	printf("             wrote %08lX cached   -> reads %08lX uncached,"
	       " %08lX cached\n", (unsigned long)~probe,
	       (unsigned long)*nocache, (unsigned long)*cached);
}

void
win_store_print_status(void)
{
	printf("Windows:     %u parked screens of %u B", usable,
	       WIN_STORE_SLOT_BYTES);
	if (usable == 0u) {
		/* Say which way it failed, and say it ACCURATELY. The first
		 * version of this reported "smaller than required" whenever
		 * the size was non-zero, so a part that reported 8 MB and then
		 * failed read-back printed a line accusing it of being too
		 * small -- alongside the two numbers that disproved the
		 * accusation. A diagnostic that names the wrong cause is worse
		 * than one that admits it does not know. */
		printf("  — NONE: ");
		if (reported_size == 0u)
			printf("PSRAM absent or not initialised\n");
		else if (reported_size < sizeof(slots))
			printf("PSRAM too small: %u B, need %u B\n",
			       (unsigned)reported_size,
			       (unsigned)sizeof(slots));
		else
			printf("read-back FAILED though %u B was reported"
			       " — check the chip select\n",
			       (unsigned)reported_size);
		printf("             root window only; the console is"
		       " unaffected\n");
		win_store_diagnose();
		return;
	}
	printf(" in %u B of PSRAM\n", (unsigned)reported_size);
}

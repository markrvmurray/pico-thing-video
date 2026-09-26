/* psram_bench.cpp – FreeBSD style
 *
 * Time the PSRAM access patterns the renderer actually uses.
 *
 * This exists because the block-blit cost was reasoned about before it was
 * measured, and the reasoning nearly went wrong in a way a bandwidth figure
 * encourages. PSRAM is quoted around 53 MB/s, which is 19 ns a byte -- but
 * every access through the UNCACHED alias is its own QSPI transaction
 * (command, address, dummy cycles, then data), so a single byte costs the
 * whole preamble and lands nearer 150 ns. A bandwidth number describes a
 * burst; the renderer was making single-byte accesses. Those are different
 * problems with different fixes, and only a measurement separates them.
 *
 * It is also the missing check on a STANDING PROJECT RULE: anything
 * configured before main() raises clk_sys is suspect. PSRAM timing has
 * already been wrong once that way -- derived at the boot clock in a pre-main
 * hook, leaving the part driven at 160 MHz against a 133 MHz maximum once
 * clk_sys reached 320. Detection ran in the same hook and cheerfully reported
 * the right size, so the instrument looked healthy while every later access
 * returned garbage. A size is not a proof of function. A timing is.
 *
 * So this reports ns per byte for each width, and verifies the data as it
 * goes: a part that is fast and wrong is worse than one that is slow.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"

#include "gfx_shadow.h"

/* Measured over a middling run: long enough that the timer's granularity is
 * irrelevant, short enough not to stall the console for a human-noticeable
 * time. 64 KB at the pessimistic 150 ns a byte is about 10 ms a pass. */
#define BENCH_BYTES	(64u * 1024u)

static void
report(const char *what, uint32_t us, uint32_t bytes, unsigned accesses)
{
	/* ns per byte is the figure that decides a design, and ns per ACCESS
	 * is the one that explains it: if the two diverge as the width grows,
	 * the cost is per-transaction overhead and the fix is wider or fewer
	 * accesses. If they track, it is genuinely bandwidth and no amount of
	 * restructuring will help. */
	const uint64_t ns = (uint64_t)us * 1000u;

	printf("  %-26s %6lu us   %5llu ns/byte  %6llu ns/access\n", what,
	    (unsigned long)us,
	    (unsigned long long)(bytes ? ns / bytes : 0),
	    (unsigned long long)(accesses ? ns / accesses : 0));
}

void
psram_bench(void)
{
	volatile uint8_t *p8 = gfx_shadow_base();

	if (p8 == NULL) {
		printf("PSRAM:       no storage — the probe found nothing,"
		       " so there is nothing to time\n");
		return;
	}
	if (GFX_SHADOW_BYTES < BENCH_BYTES) {
		printf("PSRAM:       shadow too small to time\n");
		return;
	}

	printf("PSRAM:       timing %u KB per pass through the uncached"
	       " alias\n", (unsigned)(BENCH_BYTES / 1024u));

	/* Write a known pattern byte-wise, and time it. */
	uint32_t t0 = time_us_32();
	for (uint32_t i = 0; i < BENCH_BYTES; i++)
		p8[i] = (uint8_t)(i & 0xFFu);
	report("write, 8-bit", time_us_32() - t0, BENCH_BYTES, BENCH_BYTES);

	/* Read it back byte-wise, CHECKING it: a timing taken over data that
	 * came back wrong measures nothing worth knowing. */
	uint32_t bad = 0;
	t0 = time_us_32();
	for (uint32_t i = 0; i < BENCH_BYTES; i++)
		if (p8[i] != (uint8_t)(i & 0xFFu))
			bad++;
	report("read, 8-bit (verified)", time_us_32() - t0, BENCH_BYTES,
	    BENCH_BYTES);
	if (bad != 0u)
		printf("  *** %lu of %u BYTES WRONG — the part is not"
		       " reliable at this clock; timings below are"
		       " meaningless\n", (unsigned long)bad,
		       (unsigned)BENCH_BYTES);

	/* 32-bit accesses. If the preamble dominates, this costs about the
	 * same per ACCESS and therefore a quarter per byte. That ratio is the
	 * whole justification for copying rows instead of pixels. */
	volatile uint32_t *p32 = (volatile uint32_t *)(void *)p8;
	t0 = time_us_32();
	for (uint32_t i = 0; i < BENCH_BYTES / 4u; i++)
		p32[i] = 0xA5A5A5A5u;
	report("write, 32-bit", time_us_32() - t0, BENCH_BYTES,
	    BENCH_BYTES / 4u);

	t0 = time_us_32();
	uint32_t acc = 0;
	for (uint32_t i = 0; i < BENCH_BYTES / 4u; i++)
		acc += p32[i];
	report("read, 32-bit", time_us_32() - t0, BENCH_BYTES,
	    BENCH_BYTES / 4u);

	/* memcpy both ways -- what the renderer now actually does, AT THE
	 * SIZES IT ACTUALLY USES. Timing a big block would flatter the result:
	 * per-call overhead amortises away over a kilobyte, and the renderer
	 * never copies a kilobyte. A 32x32 sprite row is 32 bytes and the
	 * region blocks are 120, so those are the figures that predict the
	 * real gain. If 32 bytes costs nearly what 120 does, the remaining
	 * cost is per-CALL and the next lever is copying several rows at once
	 * -- which only a measurement at both sizes can tell us.
	 *
	 * The buffer is 256 bytes and not a kilobyte because SRAM here is
	 * genuinely full: a 1 KB static pushed .heap into Core 1's stack and
	 * the device-role build would not link. That is also why the block
	 * blitter stages through the framebuffer row it has to write anyway
	 * rather than through a scratch array of its own. */
	static uint8_t sram[256];
	static const unsigned sizes[] = { 32u, 120u, 256u };

	for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
		const unsigned n = sizes[si];
		const unsigned iters = BENCH_BYTES / n;
		char label[40];

		t0 = time_us_32();
		for (unsigned r = 0; r < iters; r++)
			memcpy(sram, (const void *)&p8[r * n], n);
		snprintf(label, sizeof(label), "memcpy PSRAM->SRAM, %u B", n);
		report(label, time_us_32() - t0, iters * n, iters);

		t0 = time_us_32();
		for (unsigned r = 0; r < iters; r++)
			memcpy((void *)&p8[r * n], sram, n);
		snprintf(label, sizeof(label), "memcpy SRAM->PSRAM, %u B", n);
		report(label, time_us_32() - t0, iters * n, iters);
	}

	/* Defeat the dead-store elimination that would otherwise delete the
	 * 32-bit read loop outright and report it as infinitely fast. An
	 * instrument that measures nothing reads exactly like one measuring
	 * something excellent. */
	if (acc == 0xDEADBEEFu)
		printf("  (unreachable; keeps the read loop alive)\n");

	/* The shadow was just overwritten with benchmark data. Every row is
	 * still marked real, so the renderer would happily show it as
	 * graphics -- say so rather than leave a screenful of pattern to be
	 * discovered later and mistaken for corruption. */
	printf("  the graphics shadow now holds benchmark data;"
	       " clear the screen before trusting what text erases to\n");
}

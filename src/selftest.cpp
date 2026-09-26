/* selftest.cpp – FreeBSD style
 *
 * On-hardware self-tests. See selftest.h for what these are for and why they
 * are not debug-only.
 */

#if SELFTEST

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/psram.h"

#include "selftest.h"
#include "gfx_shadow.h"
#include "gfx_bufs.h"
#include "win_store.h"
#include "video.h"
#include "video_render.h"

static unsigned tests_run, tests_failed, canary_state;

static void result(const char *name, bool ok, const char *detail);

/* THE CANARY. Everything else in this file is verified by sabotage before it
 * is trusted -- but these tests run only on hardware, so whoever writes them
 * cannot execute them, and a harness that silently reports PASS for
 * everything is indistinguishable from a board in perfect health. That is the
 * single most expensive failure mode this project has: an instrument reading
 * zero because it never ran.
 *
 * So the harness proves itself, in the only place it can. One check is
 * deliberately FALSE and must come out FAIL. It is counted separately so it
 * does not spoil the verdict, and the verdict states outright that it must
 * have failed. If the canary ever reads PASS, nothing else on the report
 * means anything, however green it looks. */
static void
test_canary(void)
{
	const unsigned before = tests_failed;
	/* volatile so neither compiler folds the comparison away, or warns
	 * about it being tautological -- the point is a runtime FALSE. */
	volatile int one = 1;

	result("canary (MUST say FAIL)", one == 2, "proves FAIL is reachable");
	/* Un-count it: it is a test OF the harness, not of the board. */
	canary_state = (tests_failed == before + 1u) ? 1u : 2u;
	tests_failed = before;
	tests_run--;
}

/* A result line. The MEASUREMENT goes on the line whether it passed or not:
 * "PASS" alone tells the next person nothing about how much margin there
 * was, and a figure drifting towards its threshold is the warning that a
 * bare pass throws away. */
static void
result(const char *name, bool ok, const char *detail)
{
	tests_run++;
	if (!ok)
		tests_failed++;
	printf("  %-4s %-30s %s\n", ok ? "PASS" : "FAIL", name,
	    detail ? detail : "");
}

/* ------------------------------------------------------------------------
 * PSRAM layout: everything we declared fits, and nothing overlaps.
 *
 * Cheap, instant, and it catches a class of fault that otherwise presents as
 * inexplicable corruption a long way from its cause. Three regions are
 * carved out of PSRAM by separate translation units; if a size is raised
 * past the part, or two regions were to be placed on top of each other, the
 * symptom is a guest buffer changing under the guest, or text erasing to the
 * wrong thing -- and nobody would look here.
 * ------------------------------------------------------------------------ */
static void
test_psram_layout(void)
{
	const uint8_t *shadow = gfx_shadow_base();
	const uint8_t *bufs = gfx_bufpool_base();
	const size_t detected = psram_get_size();
	char d[96];

	if (shadow == NULL || bufs == NULL) {
		result("psram regions present", false,
		    "no storage - the probe found nothing");
		return;
	}
	snprintf(d, sizeof(d), "%u KB detected", (unsigned)(detected / 1024u));
	result("psram detected", detected != 0u, d);

	/* Declared sizes against the part. */
	const size_t want = GFX_SHADOW_BYTES + GFX_BUFPOOL_BYTES +
	    (size_t)win_store_slots() * WIN_STORE_SLOT_BYTES;

	snprintf(d, sizeof(d), "need %u KB of %u KB",
	    (unsigned)(want / 1024u), (unsigned)(detected / 1024u));
	result("declared regions fit", want <= detected, d);

	/* Overlap. Pointer comparison across separately-declared objects is
	 * formally dodgy in C, which is why this compares INTEGERS -- and it
	 * is worth doing at all because the thing being checked is exactly
	 * the assumption the language will not let us state. */
	const uintptr_t s0 = (uintptr_t)shadow, s1 = s0 + GFX_SHADOW_BYTES;
	const uintptr_t b0 = (uintptr_t)bufs, b1 = b0 + GFX_BUFPOOL_BYTES;
	const bool overlap = (s0 < b1) && (b0 < s1);

	snprintf(d, sizeof(d), "shadow %08lx+%uK, bufs %08lx+%uK",
	    (unsigned long)s0, (unsigned)(GFX_SHADOW_BYTES / 1024u),
	    (unsigned long)b0, (unsigned)(GFX_BUFPOOL_BYTES / 1024u));
	result("shadow vs buffer pool", !overlap, d);
}

/* ------------------------------------------------------------------------
 * PSRAM addressing: every address is its own.
 *
 * THE fault this project has already been bitten by, in a different guise.
 * PSRAM was once configured, reported at the right size, and completely
 * inert, because the SDK only assigns the chip-select pin inside its
 * auto-detect block. A size is not a proof of function. The generalisation
 * of that failure is ALIASING: if an address line does not reach the part,
 * two different addresses are the same cell, and a size check still passes
 * because the highest address answers -- with the contents of a lower one.
 *
 * So write a value DERIVED FROM THE ADDRESS at a stride across the whole
 * region and read it all back. Aliasing shows up as a value from the wrong
 * address, which is far more informative than a mismatch against a constant:
 * the value names the address it actually came from.
 * ------------------------------------------------------------------------ */
static void
test_psram_addressing(void)
{
	volatile uint32_t *p = (volatile uint32_t *)(void *)gfx_shadow_base();
	char d[96];

	if (p == NULL) {
		result("psram addressing", false, "no storage");
		return;
	}

	/* One word every 1 KB: enough points to catch any plausible stuck or
	 * missing address line without spending a second on it. */
	const size_t words = GFX_SHADOW_BYTES / sizeof(uint32_t);
	const size_t stride = 1024u / sizeof(uint32_t);
	size_t bad = 0, first_bad = 0, got_from = 0;

	for (size_t i = 0; i < words; i += stride)
		p[i] = (uint32_t)i ^ 0x5A5A0000u;
	for (size_t i = 0; i < words; i += stride) {
		const uint32_t v = p[i];

		if (v != ((uint32_t)i ^ 0x5A5A0000u)) {
			if (bad == 0) {
				first_bad = i;
				got_from = (size_t)(v ^ 0x5A5A0000u);
			}
			bad++;
		}
	}
	if (bad == 0)
		snprintf(d, sizeof(d), "%u points over %u KB",
		    (unsigned)((words + stride - 1) / stride),
		    (unsigned)(GFX_SHADOW_BYTES / 1024u));
	else
		snprintf(d, sizeof(d),
		    "%u bad; word %u read back as word %u",
		    (unsigned)bad, (unsigned)first_bad, (unsigned)got_from);
	result("psram addressing unique", bad == 0, d);
}

/* ------------------------------------------------------------------------
 * PSRAM retention: it still holds what it held a moment ago.
 *
 * PSRAM is DRAM behind a serial interface and refreshes itself, so a
 * mis-derived refresh interval loses data with nothing to say it did. The
 * window is deliberately long compared with any plausible refresh period,
 * and the delay is spent doing nothing so the part is not being accessed
 * (an access refreshes the row it touches, which would hide the fault).
 * ------------------------------------------------------------------------ */
static void
test_psram_retention(void)
{
	volatile uint32_t *p = (volatile uint32_t *)(void *)gfx_shadow_base();
	char d[64];

	if (p == NULL) {
		result("psram retention", false, "no storage");
		return;
	}

	const size_t words = GFX_SHADOW_BYTES / sizeof(uint32_t);
	const size_t stride = 4096u / sizeof(uint32_t);

	for (size_t i = 0; i < words; i += stride)
		p[i] = (uint32_t)i ^ 0xA5A5FFFFu;
	sleep_ms(250);
	size_t bad = 0;
	for (size_t i = 0; i < words; i += stride)
		if (p[i] != ((uint32_t)i ^ 0xA5A5FFFFu))
			bad++;
	snprintf(d, sizeof(d), "%u bad after 250 ms idle", (unsigned)bad);
	result("psram retention", bad == 0, d);
}

/* ------------------------------------------------------------------------
 * Timing. REPORTED always, ASSERTED only where the figure means something.
 *
 * A -O0 build measures a binary that will never be flashed, so a threshold
 * applied to it would be a pass nobody earned. __OPTIMIZE__ is the right
 * test for that -- not NDEBUG, which says nothing about optimisation.
 * ------------------------------------------------------------------------ */
#ifdef __OPTIMIZE__
#define TIMED_ASSERT	1
#else
#define TIMED_ASSERT	0
#endif

static void
timed(const char *name, uint32_t us, uint32_t budget_us)
{
	char d[96];

	if (TIMED_ASSERT) {
		snprintf(d, sizeof(d), "%lu us (budget %lu)",
		    (unsigned long)us, (unsigned long)budget_us);
		result(name, us <= budget_us, d);
	} else {
		/* Counted as run so the totals stay honest, never as failed.
		 * The line says plainly that nothing was checked, because a
		 * reader skimming for FAIL would otherwise take this for a
		 * pass. */
		tests_run++;
		printf("  %-4s %-30s %lu us (NOT CHECKED: -O0 build)\n",
		    "----", name, (unsigned long)us);
	}
}

/* The block blit is timed because its cost model is the whole reason it was
 * rewritten, and because it is the one figure usim structurally cannot
 * produce: the cost is QSPI transactions, and a host has none. Budgets are
 * set well above the measured figure -- this is a regression guard against
 * something going back to a byte at a time, not a benchmark to tune against.
 */
/* Time one full text refresh, and PROVE it blitted every cell. vidterm_clear()
 * paints the panel itself and leaves nothing dirty for a blank opaque glyph, so
 * a refresh after it can legitimately do nothing -- and would then report a
 * superb time for no work. The blitter counts the cells it draws, so the
 * refresh is only believed if that count moved by exactly one screen. */
static void
timed_refresh(const char *name, uint32_t budget_us)
{
	uint32_t before, after, worst;

	vidterm_blit_stats(&before, &worst);
	const uint32_t t0 = time_us_32();
	vidterm_refresh();
	const uint32_t us = time_us_32() - t0;
	vidterm_blit_stats(&after, &worst);

	if (after - before != (uint32_t)(TEXT_COLS * TEXT_ROWS)) {
		char d[80];

		snprintf(d, sizeof(d), "blitted %lu cells, not %u - not a"
		    " full refresh, timing meaningless",
		    (unsigned long)(after - before),
		    (unsigned)(TEXT_COLS * TEXT_ROWS));
		result(name, false, d);
		return;
	}
	timed(name, us, budget_us);
}

static void
test_blit_timing(void)
{
	uint32_t t0;

	/* Clip and origin have to be the whole panel or the stamps below are
	 * clipped away and the test times almost nothing. */
	vidterm_set_origin(0, 0);
	vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);

	/* A full-panel fill. Now nearly free: the whole panel makes the
	 * graphics shadow uniform by definition, so it is a framebuffer memset
	 * and one flag rather than ~960 KB across the QMI. It measured
	 * 24,148 us before that was fixed, against a budget of 25,000 -- which
	 * is how narrowly something costing a frame and a half passed for
	 * healthy, and the reason this budget is now tight enough to bite. */
	t0 = time_us_32();
	vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00u);
	timed("full-panel fill", time_us_32() - t0, 5000u);

	/* A full-WIDTH, part-height bar still goes per row, so it still pays
	 * the shadow twice a row: once to materialise it, once to fill it.
	 * Timed separately because it is the case the panel-wide shortcut
	 * cannot help, and therefore the one that would benefit from a
	 * per-row uniform colour. Keeping it here stops the cheap path from
	 * being mistaken for a general fix. */
	t0 = time_us_32();
	vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT / 2, 0x12u);
	timed("half-panel bar (per row)", time_us_32() - t0, 20000u);

	/* FULL TEXT REFRESHES, three ways, because one figure hid the cause.
	 *
	 * A single "full text refresh" measured 33 ms on one run and 24 ms on
	 * the next, and those runs differed in how much of the graphics shadow
	 * was real (the fill before it had materialised all of it, then half)
	 * -- so the figure mixed the blitter's own cost with its PSRAM reads
	 * and could not say which dominated. It also did not move at all when
	 * the C library's byte-wise memcpy was replaced, which says the cell
	 * copy never went through memcpy and the cost is elsewhere.
	 *
	 * So separate the three things a cell can cost:
	 *   opaque                 -- the glyph and its background only;
	 *   transparent, implicit  -- plus restoring a background that is one
	 *                             uniform colour (no PSRAM);
	 *   transparent, resident  -- plus reading that background from a
	 *                             real shadow row in PSRAM.
	 * The differences between them attribute the cost; the first is the
	 * floor any faster blitter has to beat. 'M' rather than a space, so
	 * the glyph path actually draws pixels. The console is transparent by
	 * default, so the last two are what an ordinary screen of text pays.
	 *
	 * Budgets are deliberately loose for now: they exist to catch a
	 * regression once a baseline is known, and there is not one yet. */
	vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, 0x00u);	/* implicit */
	(void)vidterm_clear('M', 0x00u);
	timed_refresh("text refresh, opaque", 40000u);

	(void)vidterm_clear('M', 0x01u);
	timed_refresh("text refresh, transp, implicit", 40000u);

	/* Two part-height bars make every shadow row real: a single
	 * full-panel fill would take the shortcut and leave them implicit. */
	vidterm_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT / 2, 0x12u);
	vidterm_fill_rect(0, FB_HEIGHT / 2, FB_WIDTH, FB_HEIGHT - FB_HEIGHT / 2,
	    0x12u);
	(void)vidterm_clear('M', 0x01u);
	timed_refresh("text refresh, transp, resident", 60000u);

	/* Forty 32x32 stamps -- the wgfx gem scene. Before the row rewrite
	 * this was about 300 us a stamp, 12 ms for the forty, because every
	 * pixel was two QSPI transactions; measured 4240 us afterwards.
	 *
	 * A REAL buffer, and the result is CHECKED. An earlier draft of this
	 * passed a buffer id that does not exist, so gfx_find returned NULL,
	 * PutBlk returned immediately and the test timed nothing at all while
	 * reporting a very good number. That is the exact failure this
	 * project keeps paying for, written by the person complaining about
	 * it, so the pixel check below is not belt-and-braces -- it is the
	 * part that makes the timing mean anything. */
	static const uint8_t G = 9u, ID = 1u;
	static const uint32_t SIDE = 32u;
	static const uint8_t MARK = 0xE3u;

	vidterm_buf_define(G, ID, SIDE * SIDE);
	vidterm_buf_shape(G, ID, (uint16_t)SIDE, (uint16_t)SIDE);
	bool loaded = true;
	for (uint32_t i = 0; i < SIDE * SIDE; i++)
		if (!vidterm_buf_poke(G, ID, i, MARK))
			loaded = false;
	if (!loaded) {
		result("40 block stamps", false,
		    "buffer would not load - no pool, nothing timed");
		vidterm_buf_kill(G, ID);
		return;
	}

	/* TWICE, and the two passes measure different things. The first runs
	 * over a shadow whose rows are all implicit, so it pays to materialise
	 * each row it touches -- a 1 KB PSRAM write per row, once. The second
	 * finds them real and measures stamping alone. Reporting one figure
	 * would have hidden whichever cost grew, and the split matters because
	 * the panel-wide fill above now deliberately leaves every row
	 * implicit: a guest that clears and then draws pays the first figure,
	 * which is the honest one to quote for wgfx. */
	for (int pass = 0; pass < 2; pass++) {
		t0 = time_us_32();
		for (int i = 0; i < 40; i++)
			vidterm_buf_put(G, ID, 40 + (i % 10) * 96,
			    60 + (i / 10) * 100);
		const uint32_t us = time_us_32() - t0;

		/* Did it actually draw? The last stamp's corners, so a stamp
		 * that landed but was clipped to nothing cannot pass either. */
		if (video_framebuffer[360][904] != MARK ||
		    video_framebuffer[391][935] != MARK) {
			result("40 block stamps", false,
			    "stamps did not appear - timing is meaningless");
			break;
		}
		timed(pass == 0 ? "40 stamps, shadow implicit" :
		    "40 stamps, shadow resident", us, pass == 0 ? 20000u :
		    8000u);
	}
	vidterm_buf_kill(G, ID);
}

void
selftest_run(void)
{
	tests_run = 0;
	tests_failed = 0;

	printf("Self-test: DESTRUCTIVE — overwrites the graphics shadow, and"
	       " the screen.\n");
	printf("           Clear the display and reset the link afterwards.\n");
	if (!TIMED_ASSERT)
		printf("           -O0 build: timings are reported but NOT"
		       " checked against budgets.\n");

	test_psram_layout();
	test_psram_addressing();
	test_psram_retention();
	test_blit_timing();

	/* The shadow now holds test patterns and every row is marked real, so
	 * the renderer would show it as graphics. Put it back to a known
	 * state rather than leaving a screenful of noise to be discovered
	 * later and mistaken for a fault. */
	vidterm_clear_graphics(0x00u);

	/* The canary runs LAST, so a harness broken by anything above is
	 * still caught, and so its FAIL line sits next to the verdict that
	 * explains it. */
	test_canary();

	printf("Self-test: %u run, %u failed — %s\n", tests_run, tests_failed,
	    tests_failed == 0u ? "all good" : "SEE ABOVE");
	if (canary_state != 1u)
		printf("           *** THE CANARY DID NOT FAIL — this harness"
		       " is not checking anything, and every PASS above is"
		       " worthless ***\n");
	else
		printf("           (the canary's FAIL is expected: it is what"
		       " proves a real failure would be reported)\n");
}

#endif /* SELFTEST */

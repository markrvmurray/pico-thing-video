/* vid_console.cpp – FreeBSD style
 *
 * The glass TTY: the M1 consumer of the guest command stream. PORTABLE —
 * this TU compiles into the firmware and, unchanged, into the USim
 * simulator, so the console the 6809 sees is bit-identical in both.
 *
 * M1 scope (agreed with the NitrOS-9 side): printable glyphs at the
 * cursor with advance/wrap, scroll at the bottom, CR and LF. The rest of
 * the control-code set (BS, clear/home, cursor-XY, ...) is M2, and the
 * $1B graphics escapes are M3 — unrecognised bytes are ignored here so
 * an M2/M3 guest degrades gracefully against M1 firmware.
 *
 * The screen holds the power-on test pattern until the first console
 * byte arrives; then the console lazily claims the display (clears the
 * graphics plane and text grid) and renders from a homed cursor. A link
 * soft reset re-arms that lazy claim and homes the cursor.
 */

#include <stdbool.h>
#include <stdint.h>

#include "video.h"
#include "vid_link.h"

/* White foreground on an OPAQUE black background: console overwrites and
 * scrolls must erase what was under them. (Transparent attrs are for the
 * M3+ text-over-graphics modes.) */
#define CONSOLE_ATTR  0xE0u

static int  cur_col;
static int  cur_row;
static bool claimed;		/* console has taken the screen */
static bool dirty;		/* something rendered since last flush */

static void
claim_screen(void)
{
	vidterm_clear_graphics(0x00u);
	vidterm_clear(' ', CONSOLE_ATTR);
	cur_col = 0;
	cur_row = 0;
	claimed = true;
}

static void
line_feed(void)
{
	if (cur_row < TEXT_ROWS - 1)
		cur_row++;
	else
		vidterm_scroll_up(' ', CONSOLE_ATTR);
}

extern "C" void
vid_link_consume(uint8_t b)
{
	if (!claimed)
		claim_screen();

	if (b == 0x0Du) {		/* carriage return */
		cur_col = 0;
	} else if (b == 0x0Au) {	/* line feed */
		line_feed();
	} else if (b >= 0x20u) {	/* printable glyph */
		vidterm_putch(cur_col, cur_row, b, CONSOLE_ATTR);
		if (++cur_col >= TEXT_COLS) {
			cur_col = 0;
			line_feed();
		}
	}
	/* Other control bytes: M2. Ignored for now. */
	dirty = true;
}

extern "C" void
vid_link_consume_flush(void)
{
	if (dirty) {
		dirty = false;
		vidterm_refresh();
	}
}

extern "C" void
vid_link_consume_reset(void)
{
	/* Re-arm the lazy claim; the next byte clears and homes. */
	claimed = false;
	cur_col = 0;
	cur_row = 0;
	dirty = false;
}

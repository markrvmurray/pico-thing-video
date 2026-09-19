/* vid_console.cpp – FreeBSD style
 *
 * The glass TTY: the M1 consumer of the guest command stream. PORTABLE —
 * this TU compiles into the firmware and, unchanged, into the USim
 * simulator, so the console the 6809 sees is bit-identical in both.
 *
 * Implements the CoCo3/OS-9 text control-code layer (the protocol's M2
 * scope): printable glyphs with advance/wrap/scroll, CR/LF, home, clear,
 * cursor positioning ($02 col+$20 row+$20), the erase codes, cursor
 * left/right/up, and cursor on/off (tracked; no visible cursor glyph
 * yet). The $1B graphics escapes are M3 — unrecognised bytes are ignored
 * so a newer guest degrades gracefully.
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

/* Multi-byte control sequences ($02 x y cursor position, $05 arg cursor
 * on/off) collect their parameter bytes through this little machine. */
enum param_state {
	ST_NORMAL,
	ST_XY_COL,		/* $02 seen; next byte is column + $20 */
	ST_XY_ROW,		/* column taken; next byte is row + $20 */
	ST_CURSOR_ARG		/* $05 seen; next byte is $20 off / $21 on */
};

static enum param_state state;
static int  pend_col;
static int  cur_col;
static int  cur_row;
static bool cursor_shown = true;/* tracked; no visible cursor glyph yet */
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

/* Erase [from, to) on one row with blank cells. */
static void
erase_span(int row, int from, int to)
{
	for (int c = from; c < to; c++)
		vidterm_putch(c, row, ' ', CONSOLE_ATTR);
}

/* Clamp a $02 positioning parameter: the wire value is coordinate + $20,
 * and anything out of range pins to the nearest edge. */
static int
xy_param(uint8_t b, int limit)
{
	int v = (int)b - 0x20;

	if (v < 0)
		return 0;
	if (v >= limit)
		return limit - 1;
	return v;
}

extern "C" void
vid_link_consume(uint8_t b)
{
	if (!claimed)
		claim_screen();
	dirty = true;

	/* Parameter bytes for a pending sequence come first, whatever their
	 * value: the wire format says they follow unconditionally, and
	 * consuming them keeps the stream in sync. */
	switch (state) {
	case ST_XY_COL:
		pend_col = xy_param(b, TEXT_COLS);
		state = ST_XY_ROW;
		return;
	case ST_XY_ROW:
		cur_col = pend_col;
		cur_row = xy_param(b, TEXT_ROWS);
		state = ST_NORMAL;
		return;
	case ST_CURSOR_ARG:
		cursor_shown = (b == 0x21u);	/* $20 off / $21 on */
		state = ST_NORMAL;
		return;
	case ST_NORMAL:
		break;
	}

	/* The CoCo3/OS-9 text control set (docs/copico-video-protocol.md in
	 * the nitros9 tree; matches grfdrv/vtio). $1B graphics escapes: M3. */
	switch (b) {
	case 0x01u:			/* home cursor */
		cur_col = 0;
		cur_row = 0;
		break;
	case 0x02u:			/* position cursor: $02 col+$20 row+$20 */
		state = ST_XY_COL;
		break;
	case 0x03u:			/* erase line (cursor column keeps) */
		erase_span(cur_row, 0, TEXT_COLS);
		break;
	case 0x04u:			/* erase to end of line */
		erase_span(cur_row, cur_col, TEXT_COLS);
		break;
	case 0x05u:			/* cursor off/on: $05 $20/$21 */
		state = ST_CURSOR_ARG;
		break;
	case 0x06u:			/* cursor right (no glyph) */
		if (++cur_col >= TEXT_COLS) {
			cur_col = 0;
			line_feed();
		}
		break;
	case 0x07u:			/* bell — no beeper on this device */
		break;
	case 0x08u:			/* cursor left (non-destructive) */
		if (cur_col > 0)
			cur_col--;
		break;
	case 0x09u:			/* cursor up (no reverse scroll) */
		if (cur_row > 0)
			cur_row--;
		break;
	case 0x0Au:			/* line feed / cursor down */
		line_feed();
		break;
	case 0x0Bu:			/* erase to end of window */
		erase_span(cur_row, cur_col, TEXT_COLS);
		for (int r = cur_row + 1; r < TEXT_ROWS; r++)
			erase_span(r, 0, TEXT_COLS);
		break;
	case 0x0Cu:			/* clear window and home */
		vidterm_clear(' ', CONSOLE_ATTR);
		cur_col = 0;
		cur_row = 0;
		break;
	case 0x0Du:			/* carriage return */
		cur_col = 0;
		break;
	default:
		if (b >= 0x20u) {	/* printable glyph */
			vidterm_putch(cur_col, cur_row, b, CONSOLE_ATTR);
			if (++cur_col >= TEXT_COLS) {
				cur_col = 0;
				line_feed();
			}
		}
		/* $0E-$1F: not in the set; ignored. */
		break;
	}
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
	state = ST_NORMAL;
	cursor_shown = true;
	cur_col = 0;
	cur_row = 0;
	dirty = false;
}

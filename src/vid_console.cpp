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
 * yet) — plus the $1B graphics dispatcher (M3): every sub-code in the
 * protocol table is framed by its fixed parameter count so nothing can
 * desynchronise the stream, and the starter subset draws on the plane
 * (draw pointer, Point/Line/Box/Bar with relative variants, FColor/
 * BColor/Palette/DefPal with GIME 6-bit colourValues LUT-expanded to
 * RGB332). Unimplemented commands are consumed and ignored.
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
	ST_CURSOR_ARG,		/* $05 seen; next byte is $20 off / $21 on */
	ST_ESC_OP,		/* $1B seen; next byte is the sub-code */
	ST_ESC_PARAM,		/* collecting a graphics command's params */
	ST_ESC_SKIP		/* swallowing GPLoad's streamed payload */
};

static enum param_state state;
static int  pend_col;

/* ── $1B graphics command framing ─────────────────────────────────────────
 * Fixed parameter count per sub-code ($20-$55), from the protocol table
 * (docs/copico-video-protocol.md). EVERY opcode is framed — including the
 * ones this milestone does not act on and the reserved slots — so an
 * unimplemented command never desynchronises the stream. GPLoad ($2B) is
 * the one variable-length message: 9 header bytes then a bufSize payload,
 * swallowed via ST_ESC_SKIP.
 *
 * DELIBERATE DIVERGENCE FROM CoCo3, agreed with the NitrOS-9 side
 * 2026-09-24: DWSet takes 8 params, not 7, and Select takes 1, not 0 -- in
 * both cases a leading WINDOW ID. On a CoCo3 window identity rides on the
 * device path (/w1, /w2) and never appears in the byte stream, so Select
 * needs no operand. This link is deliberately ONE transparent pipe with no
 * per-path state, so identity has nowhere else to come from: without the
 * id, "select window 3" cannot be said at all, and neither can "select the
 * panel". The guest owns the id space outright, so there is no allocation
 * protocol and no read-back of an assigned id, and DWSet stays re-definable
 * for an id it has already used. ID 0 IS THE ROOT -- the physical panel --
 * which is what keeps the surface dimensions askable at VID.Info after a
 * window has been selected. The id leads in both, so identity parses before
 * geometry. Revisit only if the CoCo3 co-module branch is ever revived, when
 * exact framing would matter again. */
static const uint8_t esc_params[0x56 - 0x20] = {
	8, 1, 7, 0, 0, 4,	/* $20 DWSet Select OWSet OWEnd DWEnd CWArea */
	0, 0, 0,		/* $26-$28 reserved */
	4, 2, 9, 10, 6,		/* $29 DefGPB KillBuf GPLoad GetBlk PutBlk */
	2, 1,			/* $2E PSet LSet */
	0, 2, 1, 1, 1,		/* $30 DefPal Palette FColor BColor Border */
	1, 1,			/* $35 ScaleSw DWProtSw */
	0, 0,			/* $37-$38 reserved */
	2, 2,			/* $39 GCSet Font */
	0,			/* $3B reserved */
	1, 1,			/* $3C TCharSw Bold */
	0,			/* $3E reserved */
	1,			/* $3F PropSw */
	4, 4, 4, 4, 4, 4, 4, 4,	/* $40-$47 SetDPtr..RLineM */
	4, 4, 4, 4,		/* $48-$4B Box RBox Bar RBar */
	0, 0,			/* $4C-$4D reserved */
	4, 0,			/* $4E PutGC, $4F FFill */
	2, 4, 12, 2, 4,		/* $50 Circle Ellipse Arc FCircle FEllipse */
	0			/* $55 reserved */
};

static uint8_t  esc_op;			/* sub-code being collected */
static uint8_t  esc_need;		/* params still to collect */
static uint8_t  esc_buf[12];		/* largest fixed count: Arc */
static uint8_t  esc_have;
static uint16_t esc_skip;		/* GPLoad payload countdown */

/* ── graphics state ── */
static int      dp_x, dp_y;		/* the draw pointer */
static uint8_t  palette[16];		/* palette registers, stored as RGB332 */
static uint8_t  fcolor_reg;		/* foreground palette register index */
static uint8_t  bcolor_reg;

/* colourValue on the wire is the CoCo3 GIME 6-bit RGB byte: bit5=R1
 * bit4=G1 bit3=B1 bit2=R0 bit1=G0 bit0=B0. Expand each 2-bit channel
 * onto the RGB332 plane. */
static uint8_t
gime_to_332(uint8_t v)
{
	static const uint8_t two_to_three[4] = { 0, 2, 5, 7 };
	uint8_t r = (uint8_t)((((v >> 5) & 1u) << 1) | ((v >> 2) & 1u));
	uint8_t g = (uint8_t)((((v >> 4) & 1u) << 1) | ((v >> 1) & 1u));
	uint8_t b = (uint8_t)((((v >> 3) & 1u) << 1) | ((v >> 0) & 1u));
	return (uint8_t)((two_to_three[r] << 5) | (two_to_three[g] << 2) | b);
}

static void
default_palette(void)
{
	/* RGBI-flavoured placeholder defaults (0-7 bright BGR order per the
	 * GIME bit pairs, 8-15 dim): guests programme what they need via
	 * Palette; refine to grfdrv's exact table when compatibility work
	 * starts. */
	static const uint8_t defs[16] = {
		0, 9, 18, 27, 36, 45, 54, 63,
		0, 1,  2,  3,  4,  5,  6,  7
	};
	for (int i = 0; i < 16; i++)
		palette[i] = gime_to_332(defs[i]);
	fcolor_reg = 7;			/* white */
	bcolor_reg = 0;			/* black */
}
static int  cur_col;
static int  cur_row;
static bool cursor_shown = true;/* tracked; no visible cursor glyph yet */
static bool claimed;		/* console has taken the screen */
static bool dirty;		/* something rendered since last flush */

/* Full clear: both layers, cursor home — and refreshed IMMEDIATELY, in
 * stream order. Deferring the blit to the end-of-burst flush would let a
 * whole-screen clear repaint its (opaque) blank cells OVER graphics drawn
 * later in the same burst. After an immediate refresh no cells are dirty,
 * so the burst's flush touches only cells written after the clear. */
static void
clear_all(void)
{
	vidterm_clear_graphics(palette[bcolor_reg & 0x0Fu]);
	vidterm_clear(' ', CONSOLE_ATTR);
	vidterm_refresh();
	cur_col = 0;
	cur_row = 0;
}

static void
claim_screen(void)
{
	clear_all();
	claimed = true;
	vidterm_cursor_enable(cursor_shown);
}

static void
line_feed(void)
{
	if (cur_row < TEXT_ROWS - 1)
		cur_row++;
	else
		vidterm_scroll_up(' ', CONSOLE_ATTR);
}

/* 16-bit big-endian coordinate/parameter fetch from the ESC buffer. */
static int
p16(int i)
{
	return (esc_buf[i] << 8) | esc_buf[i + 1];
}

static int
s16(int i)
{
	return (int16_t)((esc_buf[i] << 8) | esc_buf[i + 1]);
}

/* Execute a fully-collected $1B command. The starter subset draws; the
 * rest of the table is framed and ignored until the windowing milestone.
 * Coordinates are raw pixels on the 1024x480 plane for now (ScaleSw is
 * accepted but scaling is not yet applied); primitives clip per pixel. */
static void
esc_exec(void)
{
	const uint8_t fg = palette[fcolor_reg & 0x0Fu];

	switch (esc_op) {
	case 0x30u:			/* DefPal */
		default_palette();
		break;
	case 0x31u:			/* Palette: register, 6-bit colourValue */
		palette[esc_buf[0] & 0x0Fu] = gime_to_332(esc_buf[1] & 0x3Fu);
		break;
	case 0x32u:			/* FColor: register index */
		fcolor_reg = esc_buf[0] & 0x0Fu;
		break;
	case 0x33u:			/* BColor: register index */
		bcolor_reg = esc_buf[0] & 0x0Fu;
		break;
	case 0x40u:			/* SetDPtr */
		dp_x = p16(0);
		dp_y = p16(2);
		break;
	case 0x41u:			/* RSetDPtr */
		dp_x += s16(0);
		dp_y += s16(2);
		break;
	case 0x42u:			/* Point (pointer follows) */
		dp_x = p16(0);
		dp_y = p16(2);
		vidterm_plot(dp_x, dp_y, fg);
		break;
	case 0x43u:			/* RPoint */
		dp_x += s16(0);
		dp_y += s16(2);
		vidterm_plot(dp_x, dp_y, fg);
		break;
	case 0x44u:			/* Line: pointer -> X,Y; pointer stays */
		vidterm_line(dp_x, dp_y, p16(0), p16(2), fg);
		break;
	case 0x45u:			/* RLine */
		vidterm_line(dp_x, dp_y, dp_x + s16(0), dp_y + s16(2), fg);
		break;
	case 0x46u:			/* LineM: line then move the pointer */
		vidterm_line(dp_x, dp_y, p16(0), p16(2), fg);
		dp_x = p16(0);
		dp_y = p16(2);
		break;
	case 0x47u:			/* RLineM */
		vidterm_line(dp_x, dp_y, dp_x + s16(0), dp_y + s16(2), fg);
		dp_x += s16(0);
		dp_y += s16(2);
		break;
	case 0x48u:			/* Box: outline pointer -> X,Y */
	case 0x49u:			/* RBox */
	case 0x4Au:			/* Bar: filled pointer -> X,Y */
	case 0x4Bu: {			/* RBar */
		const bool rel = (esc_op & 1u) != 0;
		int x1 = rel ? dp_x + s16(0) : p16(0);
		int y1 = rel ? dp_y + s16(2) : p16(2);
		int xl = dp_x < x1 ? dp_x : x1, xr = dp_x < x1 ? x1 : dp_x;
		int yt = dp_y < y1 ? dp_y : y1, yb = dp_y < y1 ? y1 : dp_y;
		if (esc_op >= 0x4Au) {
			vidterm_fill_rect(xl, yt, xr - xl + 1, yb - yt + 1, fg);
		} else {
			vidterm_hline(xl, yt, xr - xl + 1, fg);
			vidterm_hline(xl, yb, xr - xl + 1, fg);
			vidterm_vline(xl, yt, yb - yt + 1, fg);
			vidterm_vline(xr, yt, yb - yt + 1, fg);
		}
		break;
	}
	case 0x4Eu:			/* PutGC: guest places the graphics cursor */
		vidterm_pointer_set(p16(0), p16(2));
		break;
	case 0x2Bu:			/* GPLoad: swallow the streamed payload */
		esc_skip = (uint16_t)p16(7);
		if (esc_skip > 0u)
			state = ST_ESC_SKIP;
		break;
	default:
		/* Framed but not yet implemented (window lifecycle, buffers,
		 * fonts, circles/ellipses/arc/ffill, reserved slots). */
		break;
	}
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
vid_link_consume_begin(void)
{
	/* Lift the cursor before anything draws: it lives in the framebuffer,
	 * so a glyph or graphics primitive landing on it would be undone when
	 * the saved pixels were put back. */
	vidterm_cursor_hide();
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
	case ST_ESC_OP:
		state = ST_NORMAL;
		if (b >= 0x20u && b <= 0x55u) {
			esc_op = b;
			esc_need = esc_params[b - 0x20u];
			esc_have = 0;
			if (esc_need == 0u)
				esc_exec();
			else
				state = ST_ESC_PARAM;
		}
		/* An out-of-range sub-code is dropped (nothing to frame). */
		return;
	case ST_ESC_PARAM:
		esc_buf[esc_have++] = b;
		if (esc_have >= esc_need) {
			state = ST_NORMAL;	/* exec may re-enter SKIP */
			esc_exec();
		}
		return;
	case ST_ESC_SKIP:
		if (--esc_skip == 0u)
			state = ST_NORMAL;
		return;
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
	case 0x1Bu:			/* graphics escape: sub-code follows */
		state = ST_ESC_OP;
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
	case 0x0Cu:			/* clear window and home (both layers) */
		clear_all();
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
	/* Park the cursor wherever the burst left it. Doing this per burst
	 * rather than per byte keeps a fast-moving cursor from flickering
	 * through every intermediate cell of a line of output. */
	if (claimed) {
		vidterm_cursor_set(cur_col, cur_row);
		vidterm_cursor_enable(cursor_shown);
	}
}

extern "C" void
vid_link_consume_reset(void)
{
	/* Re-arm the lazy claim; the next byte clears and homes. */
	claimed = false;
	state = ST_NORMAL;
	cursor_shown = true;
	vidterm_cursor_enable(false);	/* nothing owns the screen again */
	cur_col = 0;
	cur_row = 0;
	dirty = false;
	dp_x = 0;
	dp_y = 0;
	default_palette();
}

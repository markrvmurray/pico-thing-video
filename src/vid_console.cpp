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

#include <stddef.h>
#include <string.h>

#include "video.h"
#include "vid_link.h"
#include "win_store.h"

/* White foreground on an OPAQUE black background: console overwrites and
 * scrolls must erase what was under them. (Transparent attrs are for the
 * M3+ text-over-graphics modes.) */
/* White on TRANSPARENT. Transparent no longer means "skip the pixel" -- the
 * renderer restores the graphics plane there instead -- so text erases
 * correctly and whatever is drawn underneath shows through the gaps in each
 * glyph. Opaque black was hiding graphics, and doing it so thoroughly that a
 * correctly drawn box once looked like a broken feature. */
#define CONSOLE_ATTR_BASE  0xE0u
/* The attribute text is written with: the console's base, plus inverse when
 * the TARGET window has it on ($1F $20) -- per window, as GrfDrv keeps it in
 * its window table, and applying to erases as well as glyphs, since GrfDrv
 * rewrites the window's default attribute. */
#define CONSOLE_ATTR       ((uint8_t)(console_attr | \
    (windows[win_target].inverse ? VIDTERM_ATTR_INVERSE : 0u)))

/* Multi-byte control sequences ($02 x y cursor position, $05 arg cursor
 * on/off) collect their parameter bytes through this little machine. */
enum param_state {
	ST_NORMAL,
	ST_XY_COL,		/* $02 seen; next byte is column + $20 */
	ST_XY_ROW,		/* column taken; next byte is row + $20 */
	ST_CURSOR_ARG,		/* $05 seen; next byte is $20 off / $21 on */
	ST_ATTR_ARG,		/* $1F seen; next byte is its sub-code */
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
	1, 0, 0,		/* $26 DWShow, $27-$28 reserved */
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
static uint8_t  load_group, load_id;	/* the buffer that payload is filling */
static uint32_t load_at;		/* how far into it we are */
static bool     load_ok;		/* header understood; otherwise drop */

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
/* The window table. Entry 0 is the root and is never allocated a parked
 * screen: the root's text IS the live layer whenever it is selected, and when
 * it is not, it parks like any other. Identity is the guest's to choose, so
 * this is indexed by id directly rather than searched -- an id beyond the
 * table simply does not exist, which the read-back reports. */
#define WIN_MAX		8u

struct window {
	bool	defined;
	int	col0, row0, cols, rows;	/* clamped to the panel at DWSet */
	/* The WORKING AREA: a sub-rectangle of the window, relative to its
	 * corner, that text and graphics are actually confined to. CWArea
	 * narrows it; DWSet resets it to the whole window. Kept separately
	 * from the window's own geometry so narrowing is reversible and a
	 * switch restores what the window had. */
	int	wa_col, wa_row, wa_cols, wa_rows;
	bool	protect;		/* false: graphics may leave the window */
	int	cur_col, cur_row;	/* cursor WITHIN the window */
	int	slot;			/* parked screen, or -1 for none */
	bool	inverse;		/* $1F $20/$21: reverse video */
};

static struct window windows[WIN_MAX];

/* Two ideas that used to be one. Select chooses where text GOES; DWShow
 * chooses what is ON SCREEN. Conflating them means a background shell writing
 * a single line steals the display, which is the opposite of what windows are
 * for -- so the target may be a window that nobody is looking at, and its text
 * accumulates in its parked screen until someone shows it. */
static unsigned      win_target;	/* Select: where text is written */
static unsigned      win_shown;		/* DWShow: what the panel displays */

static inline bool
target_is_visible(void)
{
	return win_target == win_shown;
}

/* The target window's parked screen, or NULL when it has none (the root
 * never does, and a window gets none if the store ran out). Writing to a
 * window with no backing and no screen has nowhere to go, and is dropped --
 * visibly, in that nothing appears when it is later shown. */
static uint8_t *
target_backing(void)
{
	const int slot = windows[win_target].slot;

	return slot < 0 ? NULL : win_store_slot((unsigned)slot);
}

/* The current text window, in cells, and the cursor WITHIN it. Every text
 * operation below is bounded by this rectangle rather than by the panel, so
 * the coordinate model is the window's from here on.
 *
 * The root window is the whole panel, which is what makes this safe to land
 * before any window can be created: with these at their initial values every
 * operation reduces to exactly what it did before. */
static uint8_t console_attr = CONSOLE_ATTR_BASE | 0x01u;

static int  win_col0;
static int  win_row0;
static int  win_cols = TEXT_COLS;
static int  win_rows = TEXT_ROWS;

static inline bool
win_is_root(void)
{
	return win_col0 == 0 && win_row0 == 0 &&
	    win_cols == TEXT_COLS && win_rows == TEXT_ROWS;
}

/* Write a cell in WINDOW coordinates. The single place the origin is added,
 * so nothing below has to remember to do it. */
static inline void
put_cell(int col, int row, uint8_t ch, uint8_t attr)
{
	if (target_is_visible()) {
		vidterm_putch(win_col0 + col, win_row0 + row, ch, attr);
		return;
	}
	uint8_t *p = target_backing();
	if (p == NULL || col < 0 || col >= win_cols ||
	    row < 0 || row >= win_rows)
		return;
	p[((size_t)row * (size_t)win_cols + (size_t)col) * 2u] = ch;
	p[((size_t)row * (size_t)win_cols + (size_t)col) * 2u + 1u] = attr;
}

/* Scroll and clear have to work on whichever surface the target lives on.
 * Off screen there is no dirty tracking and no blitting to do -- the parked
 * screen is just bytes, and its contents reach the panel only when the window
 * is shown. */
static void
surface_clear(int col, int row, int cols, int rows)
{
	if (target_is_visible()) {
		vidterm_clear_region(win_col0 + col, win_row0 + row, cols,
		    rows, ' ', CONSOLE_ATTR);
		return;
	}
	for (int r = row; r < row + rows; r++)
		for (int c = col; c < col + cols; c++)
			put_cell(c, r, ' ', CONSOLE_ATTR);
}

static void
surface_scroll(void)
{
	if (target_is_visible()) {
		if (win_is_root())
			vidterm_scroll_up(' ', CONSOLE_ATTR);
		else
			vidterm_scroll_region(win_col0, win_row0, win_cols,
			    win_rows, ' ', CONSOLE_ATTR);
		return;
	}
	uint8_t *p = target_backing();
	if (p == NULL)
		return;
	const size_t stride = (size_t)win_cols * 2u;
	memmove(p, p + stride, stride * (size_t)(win_rows - 1));
	for (int c = 0; c < win_cols; c++)
		put_cell(c, win_rows - 1, ' ', CONSOLE_ATTR);
}

static int  cur_col;
static int  cur_row;

/* Insert ($1F $30) or delete ($1F $31) a line at the cursor's row, within the
 * target window only, leaving the cursor where it is. Insert moves the
 * cursor's row and everything below it down one and loses the bottom row;
 * delete moves the rows below up one and clears the bottom row. On the
 * bottom row either one just clears it. */
static void
surface_line(bool insert)
{
	const int n = win_rows - cur_row;	/* rows from the cursor down */

	if (n <= 0)
		return;
	if (target_is_visible()) {
		if (insert)
			vidterm_scroll_region_down(win_col0, win_row0 + cur_row,
			    win_cols, n, ' ', CONSOLE_ATTR);
		else
			vidterm_scroll_region(win_col0, win_row0 + cur_row,
			    win_cols, n, ' ', CONSOLE_ATTR);
		return;
	}
	uint8_t *p = target_backing();
	if (p == NULL)
		return;
	const size_t stride = (size_t)win_cols * 2u;
	uint8_t *const at = p + (size_t)cur_row * stride;
	if (insert)
		memmove(at + stride, at, stride * (size_t)(n - 1));
	else
		memmove(at, at + stride, stride * (size_t)(n - 1));
	const int blank = insert ? cur_row : win_rows - 1;
	for (int c = 0; c < win_cols; c++)
		put_cell(c, blank, ' ', CONSOLE_ATTR);
}

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
	/* Graphics is a single global plane that does not belong to any one
	 * window and survives every switch, so only the root -- which IS the
	 * panel -- may clear it. A window clearing itself must not wipe the
	 * drawing every other window shares. */
	if (win_is_root() && target_is_visible()) {
		/* The text clear paints the panel itself when the cell is
		 * blank and opaque, which is the ordinary case. A
		 * whole-screen write costs about 16 ms here, so clearing the
		 * graphics plane as well would spend most of another frame
		 * painting pixels that are about to be painted again. Fill it
		 * only when the text will NOT cover it. */
		if (!vidterm_clear(' ', CONSOLE_ATTR))
			vidterm_clear_graphics(palette[bcolor_reg & 0x0Fu]);
	} else {
		surface_clear(0, 0, win_cols, win_rows);
	}
	if (target_is_visible())
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
	if (cur_row < win_rows - 1)
		cur_row++;
	else
		surface_scroll();
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
/* Park the live text layer into a window's slot, and restore another's.
 * Copying rather than pointer-swapping because a window is a RECTANGLE of the
 * one grid, not a grid of its own: its rows are not contiguous, so there is
 * nothing to repoint. */
static void
win_park(const struct window *w)
{
	if (w->slot < 0)
		return;
	uint8_t *p = win_store_slot((unsigned)w->slot);
	if (p == NULL)
		return;
	for (int r = 0; r < w->rows; r++)
		vidterm_read_row(w->col0, w->row0 + r, w->cols,
		    &p[(size_t)r * (size_t)w->cols * 2u]);
}

static void
win_unpark(const struct window *w)
{
	uint8_t *p = w->slot < 0 ? NULL : win_store_slot((unsigned)w->slot);

	if (p == NULL) {
		/* Never parked, or no store: show it empty rather than
		 * showing whatever the previous window left behind. */
		vidterm_clear_region(w->col0, w->row0, w->cols, w->rows,
		    ' ', CONSOLE_ATTR);
		return;
	}
	for (int r = 0; r < w->rows; r++)
		vidterm_write_row(w->col0, w->row0 + r, w->cols,
		    &p[(size_t)r * (size_t)w->cols * 2u]);
}

/* Adopt a window as the current one: its rectangle becomes the coordinate
 * model and its cursor becomes the cursor. */
/* Push the target window's origin and bounds into the renderer, and tell the
 * guest what it is drawing on. Separate from adopting a window because
 * DWProtSw changes only these: routing it through the full adopt would also
 * reload the window's STORED cursor over the live one, teleporting the caret
 * for a command that has nothing to say about it. */
static void
win_apply_view(void)
{
	const struct window *w = &windows[win_target];

	win_col0 = w->col0 + w->wa_col;
	win_row0 = w->row0 + w->wa_row;
	win_cols = w->wa_cols;
	win_rows = w->wa_rows;

	/* Graphics coordinates become window-relative and bounded here. The
	 * plane itself stays global and shared -- this says where a primitive
	 * lands, not which surface it lands on, so drawing survives any
	 * window switch exactly as it did before. */
	vidterm_set_origin(win_col0 * CHAR_W, win_row0 * CHAR_H);
	if (w->protect)
		vidterm_set_clip(win_col0 * CHAR_W, win_row0 * CHAR_H,
		    win_cols * CHAR_W, win_rows * CHAR_H);
	else
		/* Protection off: coordinates still measure from the window,
		 * but the pixels may go anywhere on the panel. That is what
		 * asking to draw outside your window means; it does not mean
		 * drawing somewhere else. */
		vidterm_set_clip(0, 0, FB_WIDTH, FB_HEIGHT);

	/* The WORKING AREA, not the window: what the guest can actually draw
	 * on is what a read-back has to describe, or CWArea would narrow the
	 * drawing surface without telling anyone. */
	vid_link_info_set((uint16_t)(win_cols * CHAR_W),
	    (uint16_t)(win_rows * CHAR_H), (uint8_t)win_cols,
	    (uint8_t)win_rows);
}

static void
win_adopt(unsigned id)
{
	win_target = id;
	cur_col = windows[id].cur_col;
	cur_row = windows[id].cur_row;
	win_apply_view();
}

static void
win_stash_cursor(void)
{
	windows[win_target].cur_col = cur_col;
	windows[win_target].cur_row = cur_row;
}

/* Clamp a requested rectangle to the panel. The guest is TOLD what it got
 * rather than refused: a clamped window stays usable, and VID.Info reports
 * the geometry actually built, so the guest's hit-testing cannot drift from
 * ours. Refusing would need an error channel this link does not have. */
static void
win_define(unsigned id, int x, int y, int cols, int rows)
{
	struct window *w = &windows[id];

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > TEXT_COLS - 1) x = TEXT_COLS - 1;
	if (y > TEXT_ROWS - 1) y = TEXT_ROWS - 1;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	if (x + cols > TEXT_COLS) cols = TEXT_COLS - x;
	if (y + rows > TEXT_ROWS) rows = TEXT_ROWS - y;

	/* A slot is claimed once and kept: redefining a window's geometry must
	 * not leak the screen it was already using. */
	if (!w->defined)
		w->slot = (id == 0u) ? -1 : (int)(id - 1u);
	if (w->slot >= 0 && (unsigned)w->slot >= win_store_slots())
		w->slot = -1;		/* no store for this one */

	w->defined = true;
	w->col0 = x;
	w->row0 = y;
	w->cols = cols;
	w->rows = rows;
	w->wa_col = 0;
	w->wa_row = 0;
	w->wa_cols = cols;
	w->wa_rows = rows;
	w->protect = true;		/* windows contain their own drawing */
	w->inverse = false;		/* a (re)defined window starts plain */
	w->cur_col = 0;
	w->cur_row = 0;
}

static void
esc_exec(void)
{
	const uint8_t fg = palette[fcolor_reg & 0x0Fu];

	switch (esc_op) {
	case 0x20u: {			/* DWSet: id, type, x, y, sx, sy, fg, bg */
		const unsigned id = esc_buf[0];
		if (id >= WIN_MAX)
			break;		/* no such window; read-back says so */
		win_define(id, esc_buf[2], esc_buf[3], esc_buf[4], esc_buf[5]);
		/* Defining a window makes it current, as it does on a CoCo3
		 * where DWSet acts on the path it arrived on. */
		win_stash_cursor();
		win_adopt(id);
		/* Defining a window clears it, wherever it lives. It does NOT
		 * put itself on screen: visibility is DWShow's job alone, and
		 * a guest that defines several windows up front should not
		 * watch them flicker past. */
		surface_clear(0, 0, win_cols, win_rows);
		if (target_is_visible())
			dirty = true;
		break;
	}
	case 0x21u: {			/* Select: id — OUTPUT TARGET only */
		const unsigned id = esc_buf[0];
		if (id >= WIN_MAX || !windows[id].defined)
			break;
		if (id == win_target)
			break;
		/* No parking, no unparking, nothing on screen moves. A
		 * descriptor may Select before every write burst, so this has
		 * to be free of visible effect or a background shell would
		 * flicker the display once per line. */
		win_stash_cursor();
		win_adopt(id);
		break;
	}
	case 0x25u: {			/* CWArea: x, y, sx, sy — within the window */
		struct window *w = &windows[win_target];
		int x = esc_buf[0], y = esc_buf[1];
		int cw = esc_buf[2], ch = esc_buf[3];

		/* Clamped to the WINDOW, exactly as DWSet is clamped to the
		 * panel, and reported the same way: the guest reads back what
		 * it got rather than what it asked for. */
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		if (x > w->cols - 1) x = w->cols - 1;
		if (y > w->rows - 1) y = w->rows - 1;
		if (cw < 1) cw = 1;
		if (ch < 1) ch = 1;
		if (x + cw > w->cols) cw = w->cols - x;
		if (y + ch > w->rows) ch = w->rows - y;

		w->wa_col = x;
		w->wa_row = y;
		w->wa_cols = cw;
		w->wa_rows = ch;
		win_apply_view();	/* re-derive the coordinate model */
		cur_col = 0;		/* a new area starts at its own corner */
		cur_row = 0;
		break;
	}
	case 0x36u:			/* DWProtSw: 0 off, non-zero on */
		/* GRAPHICS only. Text stays inside its window whatever this
		 * says, because wrap and scroll are DEFINED by the window --
		 * unprotected text would have no edge to wrap at and no region
		 * to scroll. A CoCo3 protects the boundary generally; here the
		 * switch means what it can usefully mean. */
		windows[win_target].protect = esc_buf[0] != 0u;
		win_apply_view();	/* view only: the cursor is not its business */
		break;
	case 0x26u: {			/* DWShow: id — VISIBILITY only */
		const unsigned id = esc_buf[0];
		if (id >= WIN_MAX || !windows[id].defined)
			break;
		if (id == win_shown)
			break;
		/* The displayed window's text is the live layer, so showing a
		 * different one is where the swap happens: park what is up,
		 * restore what is coming. Cursor state belongs to the target,
		 * which may be neither of them. */
		win_park(&windows[win_shown]);
		win_shown = id;
		win_unpark(&windows[id]);
		dirty = true;
		break;
	}
	case 0x24u:			/* DWEnd: close the target window */
		if (win_target != 0u) {
			const unsigned gone = win_target;
			windows[gone].defined = false;
			if (win_shown == gone) {
				/* Its rectangle is nobody's now; blank it and
				 * fall back to the root, which always exists. */
				vidterm_clear_region(windows[gone].col0,
				    windows[gone].row0, windows[gone].cols,
				    windows[gone].rows, ' ', CONSOLE_ATTR);
				win_shown = 0u;
				win_unpark(&windows[0]);
				dirty = true;
			}
			win_adopt(0u);
		}
		break;
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
	case 0x39u:			/* GCSet: group, buffer — cursor sprite */
		vidterm_select_cursor(esc_buf[0], esc_buf[1]);
		break;
	case 0x3Au:			/* Font: group, buffer — text face */
		vidterm_select_font(esc_buf[0], esc_buf[1]);
		break;
	case 0x3Cu:			/* TCharSw: transparent text on/off */
		/* The console has drawn on a transparent background since
		 * the graphics shadow made erasing possible; this lets a
		 * guest ask for the opaque behaviour back, which is what it
		 * wants if it is about to overwrite a busy screen and does
		 * not care what was underneath. */
		console_attr = esc_buf[0] != 0u ?
		    (uint8_t)(CONSOLE_ATTR_BASE | 0x01u) :
		    (uint8_t)(CONSOLE_ATTR_BASE & ~0x01u);
		break;
	case 0x3Du:			/* Bold: on/off */
		vidterm_set_bold(esc_buf[0] != 0u);
		break;
	case 0x3Fu:
		/* PropSw: proportional spacing. Accepted and deliberately
		 * does nothing. This is a CELL grid -- the cursor, wrapping,
		 * scrolling, the dirty map and the parked window screens are
		 * all indexed by column, and a variable-width glyph has no
		 * column to be in. Honouring it would mean a different text
		 * layer, not a flag in this one. */
		break;
	case 0x29u:			/* DefGPB: group, buffer, len */
		vidterm_buf_define(esc_buf[0], esc_buf[1],
		    (uint32_t)p16(2));
		break;
	case 0x2Au:			/* KillBuf: group, buffer */
		vidterm_buf_kill(esc_buf[0], esc_buf[1]);
		break;
	case 0x2Cu:			/* GetBlk: group, buffer, x, y, w, h */
		vidterm_buf_get(esc_buf[0], esc_buf[1], p16(2), p16(4),
		    p16(6), p16(8));
		break;
	case 0x2Du:			/* PutBlk: group, buffer, x, y */
		vidterm_buf_put(esc_buf[0], esc_buf[1], p16(2), p16(4));
		break;
	case 0x2Eu:			/* PSet: group, buffer — fill pattern */
		vidterm_set_pattern(esc_buf[0], esc_buf[1]);
		break;
	case 0x2Fu:			/* LSet: raster mix mode, persists */
		/* Applies to the GUEST's drawing only. Text is left alone:
		 * a console whose glyphs XORed with the screen would be
		 * unreadable, and the transparent-background path already
		 * has a defined relationship with the graphics plane that a
		 * mix mode would contradict. Recorded as a divergence so the
		 * protocol document can say so. */
		vidterm_set_logic(esc_buf[0]);
		break;
	case 0x4Fu:			/* FFill: flood from the draw pointer */
		/* STARTED, not run: a large fill costs more than a frame, so
		 * it continues a slice per render pass (consume_flush), and
		 * the link drains nothing further until it is done
		 * (consume_hold) -- the fill finds its way by the colours on
		 * screen, so nothing may draw underneath it. */
		(void)vidterm_flood_fill_start(dp_x, dp_y, fg);
		break;
	case 0x50u:			/* Circle: radius, centred on the pointer */
		vidterm_ellipse(dp_x, dp_y, p16(0), p16(0), fg);
		break;
	case 0x53u:			/* FCircle */
		vidterm_fill_ellipse(dp_x, dp_y, p16(0), p16(0), fg);
		break;
	case 0x51u:			/* Ellipse: x radius, y radius */
		vidterm_ellipse(dp_x, dp_y, p16(0), p16(2), fg);
		break;
	case 0x54u:			/* FEllipse */
		vidterm_fill_ellipse(dp_x, dp_y, p16(0), p16(2), fg);
		break;
	case 0x34u:
		/* Border: accepted and deliberately does nothing. A CoCo3 has
		 * a border region around its active display and this panel has
		 * none -- 1024x480 is all active, edge to edge. Answering it
		 * silently beats leaving it in the unimplemented default,
		 * where it would look like an oversight rather than a
		 * decision. */
		break;
	case 0x4Eu:			/* PutGC: guest places the graphics cursor */
		/* Translated by hand: every other primitive coordinate is
		 * window-relative because the renderer's clip rectangle is
		 * also its origin, but the pointer is a screen-wide overlay
		 * and deliberately bypasses that. The guest should not have
		 * to know which of its coordinates are special. */
		vidterm_pointer_set(win_col0 * CHAR_W + p16(0),
		    win_row0 * CHAR_H + p16(2));
		break;
	case 0x2Bu:			/* GPLoad: header, then bufSize bytes */
		/* screenType 1 is the only defined layout: raw RGB332,
		 * row-major, no padding. Zero is reserved precisely so an
		 * all-zero or truncated header is rejected here rather than
		 * rendered as whatever it happens to contain. The payload is
		 * still counted and swallowed either way, because the byte
		 * stream has to stay in step whatever we think of the
		 * header. */
		load_group = esc_buf[0];
		load_id = esc_buf[1];
		load_ok = (esc_buf[2] == 1u);
		load_at = 0u;
		if (load_ok)
			vidterm_buf_shape(load_group, load_id,
			    (uint16_t)p16(3), (uint16_t)p16(5));
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
		put_cell(c, row, ' ', CONSOLE_ATTR);
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
		/* GPLoad's payload. Taken into the buffer when the header was
		 * one we understood, counted and dropped when it was not --
		 * the stream stays in step either way, which is the whole
		 * reason the length is in the header. */
		if (load_ok)
			(void)vidterm_buf_poke(load_group, load_id,
			    load_at++, b);
		if (--esc_skip == 0u)
			state = ST_NORMAL;
		return;
	case ST_XY_COL:
		pend_col = xy_param(b, win_cols);
		state = ST_XY_ROW;
		return;
	case ST_XY_ROW:
		cur_col = pend_col;
		cur_row = xy_param(b, win_rows);
		state = ST_NORMAL;
		return;
	case ST_CURSOR_ARG:
		cursor_shown = (b == 0x21u);	/* $20 off / $21 on */
		state = ST_NORMAL;
		return;
	case ST_ATTR_ARG:
		/* The CoCo3 $1F family (GrfDrv L1483): exactly two bytes,
		 * $1F then a sub-code. An unknown sub-code is consumed and
		 * does nothing, as GrfDrv simply returns -- it must never
		 * desynchronise the stream. */
		state = ST_NORMAL;
		switch (b) {
		case 0x20u: windows[win_target].inverse = true;  break;
		case 0x21u: windows[win_target].inverse = false; break;
		case 0x30u: surface_line(true);  break;	/* insert line */
		case 0x31u: surface_line(false); break;	/* delete line */
		/* $22/$23 underline and $24/$25 blink: accepted and
		 * ignored. The attribute byte's one free bit went to
		 * inverse, and a second attribute plane is 3,840 bytes
		 * of SRAM the device build does not have. */
		default: break;
		}
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
	case 0x1Fu:			/* text attribute / line op: sub-code */
		state = ST_ATTR_ARG;
		break;
	case 0x03u:			/* erase line (cursor column keeps) */
		erase_span(cur_row, 0, win_cols);
		break;
	case 0x04u:			/* erase to end of line */
		erase_span(cur_row, cur_col, win_cols);
		break;
	case 0x05u:			/* cursor off/on: $05 $20/$21 */
		state = ST_CURSOR_ARG;
		break;
	case 0x06u:			/* cursor right (no glyph) */
		if (++cur_col >= win_cols) {
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
		erase_span(cur_row, cur_col, win_cols);
		for (int r = cur_row + 1; r < win_rows; r++)
			erase_span(r, 0, win_cols);
		break;
	case 0x0Cu:			/* clear window and home (both layers) */
		clear_all();
		break;
	case 0x0Du:			/* carriage return */
		cur_col = 0;
		break;
	default:
		if (b >= 0x20u) {	/* printable glyph */
			put_cell(cur_col, cur_row, b, CONSOLE_ATTR);
			if (++cur_col >= win_cols) {
				cur_col = 0;
				line_feed();
			}
		}
		/* $0E-$1A, $1C-$1E: not in the set; ignored. */
		break;
	}
}

extern "C" uint8_t
vid_link_consume_class(void)
{
	/* Mapped explicitly, not by enum order: the parameter states are
	 * several, and a new one added later must not silently become a
	 * different class. */
	switch (state) {
	case ST_NORMAL:
		return VID_BYTE_START;
	case ST_ESC_OP:
		return VID_BYTE_SUBCODE;
	case ST_ESC_SKIP:
		return VID_BYTE_PAYLOAD;
	case ST_XY_COL:
	case ST_XY_ROW:
	case ST_CURSOR_ARG:
	case ST_ATTR_ARG:
	case ST_ESC_PARAM:
		return VID_BYTE_PARAM;
	}
	return VID_BYTE_PARAM;
}

extern "C" bool
vid_link_consume_busy(void)
{
	return dirty || vidterm_fill_pending();
}

extern "C" bool
vid_link_consume_hold(void)
{
	return vidterm_fill_pending();
}

extern "C" void
vid_link_consume_flush(void)
{
	/* A flood fill in progress goes first and alone. It was started by a
	 * byte that came before any text still dirty was drawn, and in one
	 * go it ran before that text was refreshed, so it still does; and
	 * the cursor is not parked on a screen that is half filled. */
	if (vidterm_fill_continue())
		return;
	/* Bounded: a full screen of text can cost more than a frame, so the
	 * refresh does a slice and `dirty` stays set until it is all drawn.
	 * The link keeps calling back (consume_busy) until then. */
	if (dirty)
		dirty = vidterm_refresh_bounded();
	/* Park the cursor wherever the burst left it. Doing this per burst
	 * rather than per byte keeps a fast-moving cursor from flickering
	 * through every intermediate cell of a line of output. */
	if (claimed && target_is_visible()) {
		vidterm_cursor_set(win_col0 + cur_col, win_row0 + cur_row);
		vidterm_cursor_enable(cursor_shown);
	}
}

extern "C" void
vid_link_consume_reset(void)
{
	/* A fill in progress belongs to the stream being thrown away. */
	vidterm_fill_abort();
	/* The root window IS the panel, and it must be a real table entry
	 * before anything can Select back to it -- an uninitialised entry 0
	 * would hand out a zero-sized rectangle and somebody else's parked
	 * screen. It owns no slot: when the root is selected its text is the
	 * live layer, and it parks like any other only once it is not. */
	for (unsigned i = 0u; i < WIN_MAX; i++)
		windows[i].defined = false;
	windows[0].defined = true;
	windows[0].col0 = 0;
	windows[0].row0 = 0;
	windows[0].cols = TEXT_COLS;
	windows[0].rows = TEXT_ROWS;
	windows[0].wa_col = 0;
	windows[0].wa_row = 0;
	windows[0].wa_cols = TEXT_COLS;
	windows[0].wa_rows = TEXT_ROWS;
	windows[0].protect = true;
	windows[0].cur_col = 0;
	windows[0].cur_row = 0;
	windows[0].slot = -1;
	windows[0].inverse = false;
	win_shown = 0u;
	win_adopt(0u);

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
	vidterm_set_logic(0u);		/* replace: the mode nobody pays for */
	vidterm_set_pattern(0u, 0u);	/* and no fill pattern */
	vidterm_select_font(0u, 0u);	/* the built-in face */
	vidterm_select_cursor(0u, 0u);	/* and the built-in arrow */
	vidterm_set_bold(false);
	console_attr = CONSOLE_ATTR_BASE | 0x01u;
	default_palette();
}

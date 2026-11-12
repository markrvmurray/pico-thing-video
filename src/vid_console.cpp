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
#include <stdio.h>
#include <string.h>

#include "video.h"
#include "vid_link.h"
#include "win_store.h"

/* The attribute text is written with comes from the target's frame: its
 * foreground and background PALETTE REGISTERS (FColor/BColor, or DWSet's),
 * kept as GrfDrv keeps them in a hardware-text attribute -- three bits each,
 * the foreground drawn from registers 8-15 and the background from 0-7 (see
 * text_colours_push()) -- plus inverse ($1F $20) and transparency (TCharSw).
 * Per frame, as GrfDrv keeps them in its window table, and applying to erases
 * as well as glyphs, since an erase writes the window's default attribute. */
#define CONSOLE_ATTR	text_attr()

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
	ST_ESC_SKIP,		/* swallowing GPLoad's streamed payload */
	ST_CSI			/* $1B [ seen: an ANSI control sequence */
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
	1, 4, 0,		/* $26 DWShow, $27 Tone, $28 reserved */
	4, 2, 9, 10, 6,		/* $29 DefGPB KillBuf GPLoad GetBlk PutBlk */
	2, 1,			/* $2E PSet LSet */
	0, 2, 1, 1, 1,		/* $30 DefPal Palette FColor BColor Border */
	1, 1,			/* $35 ScaleSw DWProtSw */
	0, 0,			/* $37 DECSC, $38 DECRC (ESC 7 / ESC 8) */
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

/* DefPal: CoWin's default palette (level2/coco3/modules/cowin.asm, L02F3):
 * white, blue, black, green, red, yellow, magenta, cyan, the same eight in
 * registers 8-15. A guest written for a CoCo 3 that never loads a palette
 * expects exactly these -- DynaStar's FColor 0 / BColor 1 is white on blue.
 * DefPal resets the registers only; it leaves every window's FColor and
 * BColor alone, as on a CoCo 3. */
static void
default_palette(void)
{
	static const uint8_t defs[8] = {
		0x3F, 0x09, 0x00, 0x12, 0x24, 0x36, 0x2D, 0x1B
	};
	for (int i = 0; i < 16; i++)
		palette[i] = gime_to_332(defs[i & 7]);
}

/* The window table. Entry 0 is the root and is never allocated a parked
 * screen: the root's text IS the live layer whenever it is selected, and when
 * it is not, it parks like any other. Identity is the guest's to choose, so
 * this is indexed by id directly rather than searched -- an id beyond the
 * table simply does not exist, which the read-back reports. */
#define WIN_MAX		8u

/* A FRAME is one level of text surface: what a window's text is written into,
 * where its cursor is, and how its text is drawn. Every window has one, and
 * everything the text and graphics code below needs to know about "the window
 * being written to" is in it, reached through tf().
 *
 * Bytes throughout, because the panel is 128 x 30 cells, and that is what pays
 * for the table: eight windows of ints cost 448 bytes of SRAM that is nearly
 * gone; these cost 128. Arithmetic is still done in int -- the fields only
 * STORE the numbers. */
struct frame {
	uint8_t	col0, row0, cols, rows;	/* the rectangle, in panel cells */
	/* The WORKING AREA: a sub-rectangle of the frame, relative to its
	 * corner, that text and graphics are actually confined to. CWArea
	 * narrows it; defining the frame resets it to the whole. Kept apart
	 * from the frame's own geometry so narrowing is reversible and a
	 * switch restores what the window had. */
	uint8_t	wa_col, wa_row, wa_cols, wa_rows;
	uint8_t	cur_col, cur_row;	/* cursor WITHIN the working area */
	/* ESC 7 / ESC 8 (DECSC/DECRC). Per frame, not one slot for the
	 * console: several windows share this one stream, so a single slot
	 * would let a background shell's save clobber the foreground's. */
	uint8_t	saved_col, saved_row;
	uint8_t	fg, bg;			/* palette registers, FColor / BColor */
	uint8_t	flags;			/* FR_* below */
};

#define FR_PROTECT	0x01u	/* graphics confined to the frame (DWProtSw) */
#define FR_INVERSE	0x02u	/* $1F $20/$21: reverse video */
#define FR_TRANSPARENT	0x04u	/* TCharSw: the graphics plane shows behind */

struct window {
	bool		defined;
	int8_t		slot;		/* parked screen, or -1 for none */
	uint8_t		top;		/* innermost overlay, or OVL_NONE */
	uint8_t		lost;		/* OWSets refused, owed an OWEnd each */
	struct frame	base;		/* the window's own text surface */
};

static struct window windows[WIN_MAX];

/* OVERLAY WINDOWS (OWSet $22 / OWEnd $23), as CoCo 3 GrfDrv has them: a
 * rectangle opened INSIDE a window that takes over its output until it is
 * closed, nesting, each one a frame of its own -- its own cursor, colours,
 * working area -- so that cursor addressing, clears and erases are all
 * relative to it. Placed relative to the WINDOW's corner (GrfDrv measures
 * against the device window, not the overlay beneath, and ignores CWArea),
 * and clamped to it as DWSet is clamped to the panel, because this link has
 * no error channel to refuse with.
 *
 * With the save switch on, the cells under the overlay are copied out when it
 * opens and put back when it closes. The copy goes to a spare parked-screen
 * slot in PSRAM -- the window store has room for more screens than there are
 * windows -- so an overlay costs SRAM only for its record. TEXT is saved, not
 * pixels: the graphics plane belongs to no window, and an overlay's text,
 * being opaque, covers the drawing without changing it, so closing the
 * overlay and redrawing the cells beneath brings the drawing back too.
 *
 * Records are a small pool shared by all windows, linked through `below`. */
#define OVL_MAX		12u
#define OVL_NONE	0xFFu

struct overlay {
	uint8_t		below;		/* the overlay underneath, or OVL_NONE */
	int8_t		save;		/* slot holding what it covers, or -1 */
	bool		want_save;	/* OWSet asked for the save switch */
	struct frame	f;
};

static struct overlay ovl[OVL_MAX];
static uint16_t ovl_used;		/* bit i: ovl[i] is in use */
static uint16_t ovl_slots_used;		/* bit i: store slot i saves an overlay */
static uint32_t stat_ovl_refused;	/* OWSets with no record free */
static uint32_t stat_ovl_unsaved;	/* save switch on, nowhere to save */

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

/* The frame a window's text goes to: its innermost overlay, if it has one. */
static inline struct frame *
frame_of(unsigned id)
{
	struct window *w = &windows[id];

	return w->top == OVL_NONE ? &w->base : &ovl[w->top].f;
}

/* The frame text is written into: the Select target's. */
static inline struct frame *
tf(void)
{
	return frame_of(win_target);
}

static inline uint8_t
frame_attr(const struct frame *f)
{
	return (uint8_t)(((f->fg & 7u) << 5) | ((f->bg & 7u) << 1) |
	    ((f->flags & FR_TRANSPARENT) != 0u ? 0x01u : 0u) |
	    ((f->flags & FR_INVERSE) != 0u ? VIDTERM_ATTR_INVERSE : 0u));
}

static inline uint8_t
text_attr(void)
{
	return frame_attr(tf());
}

/* Tell the renderer what the attribute's colour indices are, from the
 * palette: the foreground from registers 8-15 and the background from 0-7,
 * as the GIME draws hardware text. Text already on screen takes the change
 * -- a CoCo 3 recolours its text when the palette changes, and so does this.
 * Returns whether anything changed. */
static bool
text_colours_push(void)
{
	return vidterm_text_colours(&palette[8], &palette[0]);
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
static int  win_col0;
static int  win_row0;
static int  win_cols = TEXT_COLS;
static int  win_rows = TEXT_ROWS;

/* The target is the whole panel, as a window rather than as an overlay: an
 * overlay the size of the panel is still only covering it. */
static inline bool
win_is_root(void)
{
	return tf() == &windows[win_target].base &&
	    win_col0 == 0 && win_row0 == 0 &&
	    win_cols == TEXT_COLS && win_rows == TEXT_ROWS;
}

/* A cell of the target's PARKED screen, addressed in working-area
 * coordinates, or NULL when it has no parked screen or the cell is outside the
 * working area.
 *
 * The parked screen is laid out as the WHOLE WINDOW -- win_park() copies the
 * window's rectangle row by row at the window's own width -- while text is
 * addressed relative to the working area, which CWArea may have moved in from
 * the window's corner and narrowed. So the working-area position is turned
 * back into a panel position and then into the window's. Indexing the parked
 * screen with the working area's width put text written to a hidden,
 * narrowed window in the wrong cells. */
static uint8_t *
backing_cell(int col, int row)
{
	const struct frame *w = &windows[win_target].base;
	uint8_t *p = target_backing();

	if (p == NULL || col < 0 || col >= win_cols ||
	    row < 0 || row >= win_rows)
		return NULL;
	const size_t wc = (size_t)(win_col0 + col - w->col0);
	const size_t wr = (size_t)(win_row0 + row - w->row0);
	return &p[(wr * (size_t)w->cols + wc) * 2u];
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
	uint8_t *c = backing_cell(col, row);
	if (c == NULL)
		return;
	c[0] = ch;
	c[1] = attr;
}

/* Copy working-area row `from` over row `to` in the parked screen. A row of
 * the working area is contiguous there, but consecutive rows are a WINDOW's
 * width apart, so a region moves a row at a time. */
static void
backing_row_copy(int to, int from)
{
	uint8_t *d = backing_cell(0, to);
	const uint8_t *s = backing_cell(0, from);

	if (d != NULL && s != NULL)
		memmove(d, s, (size_t)win_cols * 2u);
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
	for (int r = 0; r < win_rows - 1; r++)
		backing_row_copy(r, r + 1);
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
	if (insert)
		for (int r = win_rows - 1; r > cur_row; r--)
			backing_row_copy(r, r - 1);
	else
		for (int r = cur_row; r < win_rows - 1; r++)
			backing_row_copy(r, r + 1);
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
			vidterm_clear_graphics(palette[tf()->bg & 0x0Fu]);
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
	(void)text_colours_push();	/* the power-on screen used its own */
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
win_park(const struct window *win)
{
	const struct frame *w = &win->base;

	if (win->slot < 0)
		return;
	uint8_t *p = win_store_slot((unsigned)win->slot);
	if (p == NULL)
		return;
	for (int r = 0; r < w->rows; r++)
		vidterm_read_row(w->col0, w->row0 + r, w->cols,
		    &p[(size_t)r * (size_t)w->cols * 2u]);
}

static void
win_unpark(const struct window *win)
{
	const struct frame *w = &win->base;
	uint8_t *p = win->slot < 0 ? NULL :
	    win_store_slot((unsigned)win->slot);

	if (p == NULL) {
		/* Never parked, or no store: show it empty rather than
		 * showing whatever the previous window left behind -- empty
		 * in ITS colours, not the Select target's, which may be some
		 * other window altogether. */
		vidterm_clear_region(w->col0, w->row0, w->cols, w->rows,
		    ' ', frame_attr(w));
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
	const struct frame *w = tf();

	win_col0 = w->col0 + w->wa_col;
	win_row0 = w->row0 + w->wa_row;
	win_cols = w->wa_cols;
	win_rows = w->wa_rows;

	/* Graphics coordinates become window-relative and bounded here. The
	 * plane itself stays global and shared -- this says where a primitive
	 * lands, not which surface it lands on, so drawing survives any
	 * window switch exactly as it did before. */
	vidterm_set_origin(win_col0 * CHAR_W, win_row0 * CHAR_H);
	if ((w->flags & FR_PROTECT) != 0u)
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
	cur_col = tf()->cur_col;
	cur_row = tf()->cur_row;
	win_apply_view();
}

static void
win_stash_cursor(void)
{
	tf()->cur_col = (uint8_t)cur_col;
	tf()->cur_row = (uint8_t)cur_row;
}

/* Clamp a requested rectangle to the panel. The guest is TOLD what it got
 * rather than refused: a clamped window stays usable, and VID.Info reports
 * the geometry actually built, so the guest's hit-testing cannot drift from
 * ours. Refusing would need an error channel this link does not have. */
/* Set a frame to a fresh rectangle: working area the whole of it, cursor and
 * saved cursor home, graphics protected, plain video. */
static void
frame_init(struct frame *f, int x, int y, int cols, int rows, uint8_t fg,
    uint8_t bg)
{
	f->col0 = (uint8_t)x;
	f->row0 = (uint8_t)y;
	f->cols = (uint8_t)cols;
	f->rows = (uint8_t)rows;
	f->wa_col = 0;
	f->wa_row = 0;
	f->wa_cols = (uint8_t)cols;
	f->wa_rows = (uint8_t)rows;
	f->cur_col = 0;
	f->cur_row = 0;
	f->saved_col = 0;		/* ESC 8 before any ESC 7 homes */
	f->saved_row = 0;
	f->fg = fg & 0x0Fu;
	f->bg = bg & 0x0Fu;
	/* Graphics contained; text TRANSPARENT, showing the graphics plane
	 * behind it, so that erasing text puts a drawing back. A CoCo 3
	 * starts opaque; this console has drawn transparent text since the
	 * graphics shadow made that possible. */
	f->flags = FR_PROTECT | FR_TRANSPARENT;
}

static void ovl_discard(struct window *w);

static void
win_define(unsigned id, int x, int y, int cols, int rows, uint8_t fg,
    uint8_t bg)
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
		w->slot = (int8_t)((id == 0u) ? -1 : (int)(id - 1u));
	if (w->slot >= 0 && (unsigned)w->slot >= win_store_slots())
		w->slot = -1;		/* no store for this one */

	ovl_discard(w);
	w->defined = true;
	frame_init(&w->base, x, y, cols, rows, fg, bg);
}

/* ── overlays ─────────────────────────────────────────────────────────── */

/* A store slot for an overlay's save, or -1. The windows hold slots 0 to
 * WIN_MAX - 2 (window id - 1); the rest of what the probe proved is spare. */
static int
ovl_slot_alloc(void)
{
	for (unsigned i = WIN_MAX - 1u; i < win_store_slots() && i < 16u; i++)
		if ((ovl_slots_used & (1u << i)) == 0u) {
			ovl_slots_used |= (uint16_t)(1u << i);
			return (int)i;
		}
	return -1;
}

/* Copy an overlay's rectangle out of the target window's text into `buf`, or
 * back from it, wherever that text lives: the live layer when the window is
 * shown, its parked screen when it is not (laid out at the WINDOW's width).
 * `buf` is laid out at the rectangle's own width. */
static void
ovl_copy(const struct frame *f, uint8_t *buf, bool restore)
{
	const struct frame *b = &windows[win_target].base;
	const bool live = target_is_visible();
	uint8_t *p = live ? NULL : target_backing();

	if (buf == NULL || (!live && p == NULL))
		return;			/* the window's text is going nowhere */
	for (int r = 0; r < f->rows; r++) {
		uint8_t *row = &buf[(size_t)r * f->cols * 2u];

		if (live) {
			if (restore)
				vidterm_write_row(f->col0, f->row0 + r,
				    f->cols, row);
			else
				vidterm_read_row(f->col0, f->row0 + r,
				    f->cols, row);
			continue;
		}
		uint8_t *at = &p[((size_t)(f->row0 + r - b->row0) *
		    b->cols + (size_t)(f->col0 - b->col0)) * 2u];
		if (restore)
			memcpy(at, row, (size_t)f->cols * 2u);
		else
			memcpy(row, at, (size_t)f->cols * 2u);
	}
}

/* Blank an overlay's rectangle in the target window's text, in `attr`. */
static void
ovl_fill(const struct frame *f, uint8_t attr)
{
	const struct frame *b = &windows[win_target].base;
	uint8_t *p;

	if (target_is_visible()) {
		vidterm_clear_region(f->col0, f->row0, f->cols, f->rows, ' ',
		    attr);
		return;
	}
	if ((p = target_backing()) == NULL)
		return;
	for (int r = 0; r < f->rows; r++)
		for (int c = 0; c < f->cols; c++) {
			uint8_t *at = &p[((size_t)(f->row0 + r - b->row0) *
			    b->cols + (size_t)(f->col0 + c - b->col0)) * 2u];

			at[0] = ' ';
			at[1] = attr;
		}
}

/* Unlink a window's innermost overlay and free what it held, restoring
 * nothing. */
static void
ovl_pop(struct window *w)
{
	struct overlay *o = &ovl[w->top];

	if (o->save >= 0)
		ovl_slots_used &= (uint16_t)~(1u << o->save);
	ovl_used &= (uint16_t)~(1u << w->top);
	w->top = o->below;
}

/* Forget every overlay a window has: it is being redefined or closed, and
 * what was under its overlays goes with it. */
static void
ovl_discard(struct window *w)
{
	while (w->top != OVL_NONE)
		ovl_pop(w);
	w->lost = 0;
}

/* OWSet: save switch, x, y, width, height, foreground, background. */
static void
ovl_open(void)
{
	struct window *w = &windows[win_target];
	const struct frame *b = &w->base;
	int x = esc_buf[1], y = esc_buf[2];
	int cols = esc_buf[3], rows = esc_buf[4];
	unsigned i;

	if (x > b->cols - 1) x = b->cols - 1;
	if (y > b->rows - 1) y = b->rows - 1;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	if (x + cols > b->cols) cols = b->cols - x;
	if (y + rows > b->rows) rows = b->rows - y;

	for (i = 0u; i < OVL_MAX; i++)
		if ((ovl_used & (1u << i)) == 0u)
			break;
	if (i == OVL_MAX) {
		/* Nothing to open it with. Its OWEnd must then close nothing,
		 * not the overlay beneath, so the refusal is remembered. */
		if (w->lost != 0xFFu)
			w->lost++;
		stat_ovl_refused++;
		return;
	}

	win_stash_cursor();		/* the parent's, kept in its frame */
	struct overlay *o = &ovl[i];
	ovl_used |= (uint16_t)(1u << i);
	frame_init(&o->f, b->col0 + x, b->row0 + y, cols, rows, esc_buf[5],
	    esc_buf[6]);
	/* GrfDrv copies the device window's switches, so protection and
	 * inverse carry over. The text is OPAQUE whatever the window's is: an
	 * overlay is there to cover what is beneath it, in its own background
	 * colour, and a transparent one would show the drawing through it. */
	o->f.flags = (uint8_t)(b->flags & (FR_PROTECT | FR_INVERSE));
	o->below = w->top;
	o->want_save = esc_buf[0] != 0u;
	o->save = -1;
	if (o->want_save) {
		/* Saved, then cleared in the overlay's colours; with the switch
		 * off GrfDrv does neither, and the overlay opens on whatever
		 * was there. */
		o->save = (int8_t)ovl_slot_alloc();
		if (o->save >= 0)
			ovl_copy(&o->f, win_store_slot((unsigned)o->save),
			    false);
		else
			stat_ovl_unsaved++;
		ovl_fill(&o->f, frame_attr(&o->f));
		if (target_is_visible())
			dirty = true;
	}
	w->top = (uint8_t)i;
	win_adopt(win_target);		/* its view, its cursor: home */
}

/* OWEnd: close the target's innermost overlay. */
static void
ovl_close(void)
{
	struct window *w = &windows[win_target];

	if (w->lost != 0u) {
		w->lost--;		/* the one that never opened */
		return;
	}
	if (w->top == OVL_NONE)
		return;			/* GrfDrv errors; there is no channel */

	const struct overlay *o = &ovl[w->top];
	const struct frame covered = o->f;
	const int save = o->save;
	const bool want = o->want_save;

	if (save >= 0)
		ovl_copy(&covered, win_store_slot((unsigned)save), true);
	ovl_pop(w);
	/* Asked to save and could not: the area is cleared in the colours of
	 * what is now on top rather than left showing the overlay. Without
	 * the switch nothing is restored, as on a CoCo 3. */
	if (save < 0 && want)
		ovl_fill(&covered, frame_attr(tf()));
	if (target_is_visible())
		dirty = true;
	win_adopt(win_target);		/* the parent's view and cursor */
}

static void
esc_exec(void)
{
	const uint8_t fg = palette[tf()->fg & 0x0Fu];

	switch (esc_op) {
	case 0x20u: {			/* DWSet: id, type, x, y, sx, sy, fg, bg */
		const unsigned id = esc_buf[0];
		if (id >= WIN_MAX)
			break;		/* no such window; read-back says so */
		win_define(id, esc_buf[2], esc_buf[3], esc_buf[4], esc_buf[5],
		    esc_buf[6], esc_buf[7]);
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
	case 0x22u:			/* OWSet: open an overlay */
		ovl_open();
		break;
	case 0x23u:			/* OWEnd: close the innermost */
		ovl_close();
		break;
	case 0x25u: {			/* CWArea: x, y, sx, sy — within the window */
		struct frame *w = tf();
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

		w->wa_col = (uint8_t)x;
		w->wa_row = (uint8_t)y;
		w->wa_cols = (uint8_t)cw;
		w->wa_rows = (uint8_t)ch;
		win_apply_view();	/* re-derive the coordinate model */
		cur_col = 0;		/* a new area starts at its own corner */
		cur_row = 0;
		break;
	}
	case 0x37u:			/* ESC 7: save the cursor (DECSC) */
		/* The POSITION only. A VT100 also saves the rendition and
		 * character set; the analogue here would be inverse, which a
		 * CoCo3 program sets with $1F $20/$21 and would not expect an
		 * ESC 8 to undo. What these two are for is the terminal size
		 * query (ESC 7, ESC [ 999;999 H, ESC [ 6 n, ESC 8), which has
		 * to leave the cursor where it found it -- and any VT100
		 * program that saves and restores around a status line. */
		tf()->saved_col = (uint8_t)cur_col;
		tf()->saved_row = (uint8_t)cur_row;
		break;
	case 0x38u:			/* ESC 8: restore the cursor (DECRC) */
		/* Clamped, because CWArea may have narrowed the working area
		 * since the save. Before any save it homes, as a VT100 does. */
		cur_col = tf()->saved_col;
		cur_row = tf()->saved_row;
		if (cur_col >= win_cols)
			cur_col = win_cols - 1;
		if (cur_row >= win_rows)
			cur_row = win_rows - 1;
		break;
	case 0x36u:			/* DWProtSw: 0 off, non-zero on */
		/* GRAPHICS only. Text stays inside its window whatever this
		 * says, because wrap and scroll are DEFINED by the window --
		 * unprotected text would have no edge to wrap at and no region
		 * to scroll. A CoCo3 protects the boundary generally; here the
		 * switch means what it can usefully mean. */
		if (esc_buf[0] != 0u)
			tf()->flags |= FR_PROTECT;
		else
			tf()->flags &= (uint8_t)~FR_PROTECT;
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
			ovl_discard(&windows[gone]);
			windows[gone].defined = false;
			if (win_shown == gone) {
				/* Its rectangle is nobody's now; blank it and
				 * fall back to the root, which always exists. */
				const struct frame *g = &windows[gone].base;

				vidterm_clear_region(g->col0, g->row0, g->cols,
				    g->rows, ' ', CONSOLE_ATTR);
				win_shown = 0u;
				win_unpark(&windows[0]);
				dirty = true;
			}
			win_adopt(0u);
		}
		break;
	case 0x30u:			/* DefPal */
		default_palette();
		if (text_colours_push())
			dirty = true;		/* recolour what is on screen */
		break;
	case 0x31u:			/* Palette: register, 6-bit colourValue */
		palette[esc_buf[0] & 0x0Fu] = gime_to_332(esc_buf[1] & 0x3Fu);
		if (text_colours_push())
			dirty = true;
		break;
	case 0x32u:			/* FColor: register, for text and drawing */
		/* The window's, as on a CoCo 3: the colour primitives draw in
		 * and the colour its text is written in from now on. Text
		 * already written keeps the colour it was written in. */
		tf()->fg = esc_buf[0] & 0x0Fu;
		break;
	case 0x33u:			/* BColor: register */
		tf()->bg = esc_buf[0] & 0x0Fu;
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
		if (esc_buf[0] != 0u)
			tf()->flags |= FR_TRANSPARENT;
		else
			tf()->flags &= (uint8_t)~FR_TRANSPARENT;
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
	case 0x27u:			/* Tone: pitch(2), ticks, volume */
		/* A Pico-Thing addition in a slot CoWin leaves free, carrying
		 * CoCo 3 SS.Tone values unchanged so a driver can forward the
		 * SetStat as it stands. It starts the sound and returns: the
		 * guest's driver sleeps out the duration itself, which is what
		 * gives SS.Tone its CoCo behaviour of returning when the note
		 * ends. One voice, no queue. */
		vid_link_tone(p16(0), esc_buf[2], esc_buf[3]);
		break;
	case 0x52u:			/* Arc: radii, then the clip line */
		/* The clip line's ends are SIGNED offsets from the centre
		 * (GrfDrv accepts negatives), the radii plain sizes. */
		vidterm_arc(dp_x, dp_y, p16(0), p16(2),
		    s16(4), s16(6), s16(8), s16(10), fg);
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
		/* Framed and deliberately inert: ScaleSw ($35) and the
		 * reserved slots. Their parameters were consumed by the
		 * framing, so the stream stays in step. */
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

/* ── ANSI/VT100 control sequences ($1B $5B, "ESC [") ─────────────────────
 * Not part of the CoCo3 set: these exist so a guest on a SERIAL console,
 * which has no register window and so no VID.Info, can learn the screen's
 * size the way xterm's resize does -- move the cursor somewhere absurd, ask
 * where it ended up, and read the answer as input. Over the register window
 * the same queries work too, which is what lets one utility serve both.
 *
 * $5B lies outside the $20-$55 sub-code range, so it cannot collide with a
 * CoCo3 command; it used to be dropped as out of range, and the rest of the
 * sequence then printed as text.
 *
 * The shape is ECMA-48's: parameter bytes $30-$3F, intermediate bytes
 * $20-$2F, one final byte $40-$7E. Every sequence of that shape is consumed
 * WHOLE, whether or not it is one we act on, for the same reason every $1B
 * sub-code is framed: an unrecognised sequence must never leave bytes behind
 * to be printed or, worse, parsed as something else. A byte outside all
 * three ranges ends the sequence early and is then acted on as usual -- a
 * VT100 does the same with C0 controls, and it means a $1B arriving inside
 * a sequence starts a fresh escape rather than being lost. */
#define CSI_PARAMS_MAX	4u	/* stored; later ones are consumed unread */
#define CSI_VALUE_MAX	9999u	/* a value saturates rather than wraps */

static uint16_t csi_param[CSI_PARAMS_MAX];
static uint8_t  csi_n;		/* index of the parameter being read */
static bool     csi_foreign;	/* private marker or intermediate: not ours */

static void
csi_begin(void)
{
	for (unsigned i = 0; i < CSI_PARAMS_MAX; i++)
		csi_param[i] = 0u;
	csi_n = 0u;
	csi_foreign = false;
	state = ST_CSI;
}

/* Append v in decimal; returns the new length. */
static unsigned
put_dec(uint8_t *buf, unsigned at, unsigned v)
{
	char digits[6];
	unsigned n = 0;

	do {
		digits[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u && n < sizeof(digits));
	while (n != 0u)
		buf[at++] = (uint8_t)digits[--n];
	return at;
}

static void
csi_exec(uint8_t final)
{
	/* A private marker ($3C-$3F, as in ESC [ ? 25 l) or an intermediate byte
	 * makes this a different function from any we implement -- ESC [ ? 6 n
	 * is DEC's extended report, for one, with a different reply. Consumed,
	 * deliberately not answered. */
	if (csi_foreign)
		return;

	/* An omitted parameter and an explicit 0 both mean the default, which
	 * is 1 for a position, exactly as on a VT100. */
	const unsigned nparams = (unsigned)csi_n + 1u;
	uint8_t reply[16];
	unsigned len = 0;

	switch (final) {
	case 'H':			/* CUP: cursor position */
	case 'f': {			/* HVP: the same function */
		unsigned row = csi_param[0] ? csi_param[0] : 1u;
		unsigned col = csi_param[1] ? csi_param[1] : 1u;

		/* Clamped, not rejected: ESC [ 999;999 H is how the size query
		 * finds the bottom right, so the clamp IS the feature. */
		if (row > (unsigned)win_rows)
			row = (unsigned)win_rows;
		if (col > (unsigned)win_cols)
			col = (unsigned)win_cols;
		cur_row = (int)row - 1;
		cur_col = (int)col - 1;
		return;
	}
	case 'n':			/* DSR: only 6, the cursor report */
		if (nparams != 1u || csi_param[0] != 6u)
			return;
		reply[len++] = 0x1Bu;
		reply[len++] = '[';
		len = put_dec(reply, len, (unsigned)cur_row + 1u);
		reply[len++] = ';';
		len = put_dec(reply, len, (unsigned)cur_col + 1u);
		reply[len++] = 'R';
		break;
	case 't':			/* xterm window ops: only 18, the size */
		/* Exactly one parameter. ESC [ 8 ; rows ; cols t is the REQUEST
		 * to resize a window, and must not be mistaken for a query. */
		if (nparams != 1u || csi_param[0] != 18u)
			return;
		reply[len++] = 0x1Bu;
		reply[len++] = '[';
		reply[len++] = '8';
		reply[len++] = ';';
		len = put_dec(reply, len, (unsigned)win_rows);
		reply[len++] = ';';
		len = put_dec(reply, len, (unsigned)win_cols);
		reply[len++] = 't';
		break;
	default:
		return;
	}
	/* The current window's working area throughout: the cursor is held in
	 * those terms, and win_rows/win_cols are the same figures VID.Info
	 * publishes, so a guest that asks both ways gets one answer. */
	(void)vid_link_input_reply(reply, len);
}

extern "C" void
vid_link_consume_begin(void)
{
	/* Lift the cursor before anything draws: it lives in the framebuffer,
	 * so a glyph or graphics primitive landing on it would be undone when
	 * the saved pixels were put back. */
	vidterm_cursor_hide();
	/* The colours are the console's to say. Anything else that drives
	 * the renderer directly -- the self-test, a test harness -- may have
	 * left its own; asserting ours each pass costs a 16-byte compare, and
	 * a change recolours the screen as a Palette would. */
	if (claimed && text_colours_push())
		dirty = true;
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
		if (b == 0x5Bu) {	/* ESC [ : an ANSI control sequence */
			csi_begin();
			return;
		}
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
	case ST_CSI:
		if (b >= '0' && b <= '9') {
			if (csi_n < CSI_PARAMS_MAX) {
				unsigned v = (unsigned)csi_param[csi_n] * 10u +
				    (unsigned)(b - '0');

				csi_param[csi_n] = (uint16_t)(v > CSI_VALUE_MAX ?
				    CSI_VALUE_MAX : v);
			}
			return;
		}
		if (b == ';') {
			if (csi_n != 0xFFu)
				csi_n++;
			return;
		}
		if (b >= 0x20u && b <= 0x3Fu) {
			/* Private markers ($3C-$3F), ':' and the intermediates
			 * ($20-$2F): still part of the sequence, never ours. */
			csi_foreign = true;
			return;
		}
		state = ST_NORMAL;
		if (b >= 0x40u && b <= 0x7Eu) {
			csi_exec(b);
			return;
		}
		/* Not a sequence byte at all: the sequence ends here and this
		 * byte is acted on normally, below. */
		break;
	case ST_ATTR_ARG:
		/* The CoCo3 $1F family (GrfDrv L1483): exactly two bytes,
		 * $1F then a sub-code. An unknown sub-code is consumed and
		 * does nothing, as GrfDrv simply returns -- it must never
		 * desynchronise the stream. */
		state = ST_NORMAL;
		switch (b) {
		case 0x20u: tf()->flags |= FR_INVERSE; break;
		case 0x21u: tf()->flags &= (uint8_t)~FR_INVERSE; break;
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
	case 0x07u:			/* bell: the platform makes it a sound */
		vid_link_bell();
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
	case ST_CSI:
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
	for (unsigned i = 0u; i < WIN_MAX; i++) {
		windows[i].defined = false;
		windows[i].top = OVL_NONE;
		windows[i].lost = 0u;
	}
	ovl_used = 0u;
	ovl_slots_used = 0u;
	windows[0].defined = true;
	windows[0].slot = -1;
	/* White on black in CoWin's palette: FColor 0, BColor 2. */
	frame_init(&windows[0].base, 0, 0, TEXT_COLS, TEXT_ROWS, 0u, 2u);
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
	default_palette();
}

extern "C" void
vid_link_consume_print_status(void)
{
	/* Silent while both are zero, which is the only good reading. */
	if (stat_ovl_refused != 0u)
		printf("             %lu overlays REFUSED -- all %u records in"
		    " use\n", (unsigned long)stat_ovl_refused,
		    (unsigned)OVL_MAX);
	if (stat_ovl_unsaved != 0u)
		printf("             %lu overlays opened UNSAVED -- no store"
		    " slot; closing one clears it\n",
		    (unsigned long)stat_ovl_unsaved);
}

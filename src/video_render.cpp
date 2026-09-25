/* video_render.cpp – FreeBSD style
 *
 * The PORTABLE half of the video engine: framebuffer + text grid state and
 * every drawing/compositing routine. No pico or hardware headers — this TU
 * must compile on a desktop host unchanged, so a simulator (USim video
 * window) renders with bit-identical code to the firmware. The hardware
 * scan-out half (HSTX, ping-pong DMA, vsync) lives in video.cpp and touches
 * this TU only through video_render.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "video.h"
#include "video_render.h"
#include "gfx_shadow.h"
#include "gfx_bufs.h"
#include "font_spleen_8x16.h"

/* ───────────────────────── compile-time parameters ──────────────────────── */

/* Screen geometry (FB_WIDTH/FB_HEIGHT/CHAR_W/CHAR_H/TEXT_COLS/TEXT_ROWS) is the
 * single source of truth in video.h, so consumers cannot drift from it. */

#define TRANSPARENT_BG  0x00

/* Force -O3 on hot rendering functions regardless of the build type. Only GCC
 * implements the optimize function attribute; clang-based toolchains (ATfE)
 * reject it, so there the macro is empty and the build's own level applies. */
#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMIZE_O3 __attribute__((optimize("O3")))
#else
#define OPTIMIZE_O3
#endif

/* GPIO_HSTX_BASE / GPIO_HSTX_COUNT come from the shared pin map. */

/* ──────────────────────────── framebuffer (SRAM) ──────────────────────── */

// RGB332 framebuffer in SRAM. DMA reads directly from here.
uint8_t video_framebuffer[FB_HEIGHT][FB_WIDTH] __attribute__((aligned(4)));

/* ─────────────────────────── text layer (SRAM) ──────────────────────────── */

static uint8_t text_cells[TEXT_ROWS][TEXT_COLS];
static uint8_t text_attrs[TEXT_ROWS][TEXT_COLS];
#define DIRTY_WORDS ((TEXT_COLS + 31) / 32)  /* ceil(cols/32) */
static uint32_t dirty_rows[TEXT_ROWS][DIRTY_WORDS];

/* ──────────────────────────── RGB332 helpers ────────────────────────────── */

/* RGB332: RRRGGGBB — 3 bits red, 3 bits green, 2 bits blue */
static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0x07) << 5) | ((g & 0x07) << 2) | (b & 0x03);
}

static inline uint8_t attr_to_fg(uint8_t attr)
{
    uint8_t r = (attr >> 7) & 1;
    uint8_t g = (attr >> 6) & 1;
    uint8_t b = (attr >> 5) & 1;
    return rgb332(r ? 7 : 0, g ? 7 : 0, b ? 3 : 0);
}

static inline uint8_t attr_to_bg(uint8_t attr)
{
    uint8_t r = (attr >> 3) & 1;
    uint8_t g = (attr >> 2) & 1;
    uint8_t b = (attr >> 1) & 1;

    if (attr & 0x01) return TRANSPARENT_BG;

    return rgb332(r ? 2 : 0, g ? 2 : 0, b ? 1 : 0);
}

/* ─────────────────────────────── 8×16 font ─────────────────────────────── */

/* A guest-supplied face, or the built-in one. The layout is the built-in
 * one's exactly -- 256 glyphs of 16 bytes, one byte per row, glyph N at N*16
 * -- so a loaded face drops into this blitter with no repacking at all.
 *
 * Resolved ONCE when selected rather than looked up per glyph: a full-screen
 * refresh asks for 3840 cells and a table scan each would be felt. The
 * selection is dropped if the buffer holding it is killed, which is the only
 * way the pointer could go stale. */
static const uint8_t *font_face;
static uint32_t       font_face_len;
static bool           text_bold;

/* Which buffer each selection came from, so a kill can drop it. */
static uint8_t sel_font_group, sel_font_id;
static uint8_t sel_ptr_group, sel_ptr_id;
static bool    sel_font_on, sel_ptr_on;

void vidterm_set_cursor_sprite(const uint8_t *data, int w, int h);
const uint8_t *vidterm_buf_data(uint8_t group, uint8_t id, uint32_t *len,
                                uint16_t *xs, uint16_t *ys);

void vidterm_set_font(const uint8_t *data, uint32_t len)
{
    font_face = data;
    font_face_len = len;
}

/* Called when a buffer is killed: if either selection came from it, put that
 * selection back to the built-in default rather than leaving a pointer into
 * storage the pool may hand out again. */
static void vidterm_selection_dropped(uint8_t group, uint8_t id)
{
    if (sel_font_on && sel_font_group == group && sel_font_id == id) {
        sel_font_on = false;
        vidterm_set_font(NULL, 0);
    }
    if (sel_ptr_on && sel_ptr_group == group && sel_ptr_id == id) {
        sel_ptr_on = false;
        vidterm_set_cursor_sprite(NULL, 0, 0);
    }
}

void vidterm_select_font(uint8_t group, uint8_t id)
{
    uint32_t len = 0;
    const uint8_t *p = (id == 0u) ? NULL :
        vidterm_buf_data(group, id, &len, NULL, NULL);

    sel_font_on = (p != NULL);
    sel_font_group = group;
    sel_font_id = id;
    vidterm_set_font(p, len);
}

void vidterm_select_cursor(uint8_t group, uint8_t id)
{
    uint16_t xs = 0, ys = 0;
    const uint8_t *p = (id == 0u) ? NULL :
        vidterm_buf_data(group, id, NULL, &xs, &ys);

    sel_ptr_on = (p != NULL);
    sel_ptr_group = group;
    sel_ptr_id = id;
    vidterm_set_cursor_sprite(p, xs, ys);
}

void vidterm_set_bold(bool on)
{
    text_bold = on;
}

static inline uint8_t font_row(uint8_t ch, int row)
{
    const int r = row & (CHAR_H - 1);
    uint8_t v;

    if (font_face != NULL) {
        const uint32_t at = (uint32_t)ch * CHAR_H + (uint32_t)r;

        /* A short face cannot address every glyph. The ones it cannot reach
         * render BLANK rather than reading past the end of the buffer --
         * "undefined above" is how a buffer overrun gets written down as a
         * feature. */
        v = (at < font_face_len) ? font_face[at] : 0u;
    } else {
        v = font_spleen_8x16[ch][r];
    }

    /* Bold by smearing one pixel right, which is what a bitmap font can do
     * without a second face. It thickens strokes and closes nothing, since
     * the glyph cell has a blank column at its right in this face. */
    return text_bold ? (uint8_t)(v | (v >> 1)) : v;
}

/* ──────────────────────── get/put buffers ───────────────────────────────── */

/* Off-screen bitmaps the guest defines and fills, identified by a (group,
 * buffer) pair it chooses. Both are its namespace, not ours -- we only have
 * to find them again.
 *
 * ALLOCATION IS A BUMP AND A RESET. Storage is handed out in order and a
 * freed buffer's bytes are not reused until EVERY buffer is freed, at which
 * point the pool starts again from the beginning. That is a deliberate
 * refusal to write an allocator: a first-fit free list is a fragmentation
 * bug waiting for the one guest that allocates and frees in an awkward
 * order, and there are two megabytes here for buffers measured in kilobytes.
 * The cost is that a guest cycling buffers forever eventually exhausts the
 * pool; the count and the high-water mark are reported so that is visible
 * rather than mysterious. */
#define GFX_BUF_MAX	32u

struct gfx_buf {
    bool     used;
    uint8_t  group, id;
    uint16_t xsize, ysize;
    uint32_t offset, len;
};

static void vidterm_selection_dropped(uint8_t group, uint8_t id);

static struct gfx_buf gfx_bufs[GFX_BUF_MAX];
static uint32_t       gfx_pool_used;
static uint32_t       gfx_pool_high;

static struct gfx_buf *gfx_find(uint8_t group, uint8_t id)
{
    for (unsigned i = 0; i < GFX_BUF_MAX; i++)
        if (gfx_bufs[i].used && gfx_bufs[i].group == group &&
            gfx_bufs[i].id == id)
            return &gfx_bufs[i];
    return NULL;
}

static uint8_t *gfx_buf_bytes(const struct gfx_buf *b)
{
    uint8_t *base = gfx_bufpool_base();

    return (base == NULL || b == NULL) ? NULL : &base[b->offset];
}

void vidterm_buf_define(uint8_t group, uint8_t id, uint32_t len)
{
    struct gfx_buf *b = gfx_find(group, id);

    /* Redefining an existing buffer keeps its storage if the new size fits,
     * because a guest reusing an id for the same purpose is the common case
     * and should not consume the pool twice. */
    if (b != NULL && len <= b->len) {
        b->len = len;
        return;
    }
    if (b == NULL) {
        for (unsigned i = 0; i < GFX_BUF_MAX; i++)
            if (!gfx_bufs[i].used) {
                b = &gfx_bufs[i];
                break;
            }
    }
    if (b == NULL || len == 0 || len > GFX_BUFPOOL_BYTES - gfx_pool_used)
        return;			/* no descriptor, or no room: unanswered */

    b->used = true;
    b->group = group;
    b->id = id;
    b->xsize = 0;
    b->ysize = 0;
    b->offset = gfx_pool_used;
    b->len = len;
    gfx_pool_used += len;
    if (gfx_pool_used > gfx_pool_high)
        gfx_pool_high = gfx_pool_used;
}

void vidterm_buf_kill(uint8_t group, uint8_t id)
{
    struct gfx_buf *b = gfx_find(group, id);

    if (b != NULL)
        b->used = false;

    /* A face or a sprite is a resolved POINTER into the pool, taken once so
     * the blitter does not pay for a lookup per glyph. Killing the buffer it
     * came from is the one way that pointer could go stale, so the selection
     * is dropped here rather than left to dangle until the pool resets under
     * it. */
    vidterm_selection_dropped(group, id);

    for (unsigned i = 0; i < GFX_BUF_MAX; i++)
        if (gfx_bufs[i].used)
            return;
    gfx_pool_used = 0;		/* all gone: the pool starts over */
}

/* Hand out a buffer's bytes and shape, so the console can point the font or
 * the cursor sprite at one. Returns NULL for a buffer that does not exist,
 * which is how selecting a dead buffer becomes "keep what you had" rather
 * than a fault. */
const uint8_t *vidterm_buf_data(uint8_t group, uint8_t id, uint32_t *len,
                                uint16_t *xs, uint16_t *ys)
{
    const struct gfx_buf *b = gfx_find(group, id);
    const uint8_t *p = gfx_buf_bytes(b);

    if (p == NULL)
        return NULL;
    if (len != NULL) *len = b->len;
    if (xs != NULL)  *xs = b->xsize;
    if (ys != NULL)  *ys = b->ysize;
    return p;
}

void vidterm_buf_stats(uint32_t *used, uint32_t *high, unsigned *count)
{
    unsigned n = 0;

    for (unsigned i = 0; i < GFX_BUF_MAX; i++)
        if (gfx_bufs[i].used)
            n++;
    *used = gfx_pool_used;
    *high = gfx_pool_high;
    *count = n;
}

/* ───────────────────────── the graphics shadow ──────────────────────────── */

/* What the framebuffer would hold with no text on it, so transparent text can
 * ERASE by restoring what was underneath rather than by painting a background.
 *
 * Most of it is usually one colour, and writing half a megabyte to PSRAM costs
 * about 9 ms, so the flat part is kept IMPLICIT: a uniform colour plus a
 * per-row flag saying which rows have been made real. A clear sets the colour
 * and clears every flag -- no bytes move at all. A row becomes real the first
 * time a primitive draws into it, and only that row is filled.
 *
 * This is what stops transparency costing a frame per clear. The alternative,
 * maintaining the plane literally, would have put a clear back over its
 * budget the day after that budget was finally met. */
static uint8_t  shadow_uniform_colour;
static bool     shadow_row_real[FB_HEIGHT];
static bool     shadow_any_real;

static inline uint8_t *shadow_row(int y)
{
    uint8_t *base = gfx_shadow_base();

    if (base == NULL)
        return NULL;
    if (!shadow_row_real[y]) {
        memset(&base[(size_t)y * FB_WIDTH], shadow_uniform_colour, FB_WIDTH);
        shadow_row_real[y] = true;
        shadow_any_real = true;
    }
    return &base[(size_t)y * FB_WIDTH];
}

/* The row as it stands, without making it real. Only valid when some row IS
 * real; callers check shadow_any_real first. */
static inline const uint8_t *shadow_row_ro(int y)
{
    const uint8_t *base = gfx_shadow_base();

    return (base != NULL && shadow_row_real[y]) ?
        &base[(size_t)y * FB_WIDTH] : NULL;
}

/* Read one pixel of the shadow. The common answer needs no memory access at
 * all, which is the point: a screen of text over a plain background asks this
 * question half a million times and gets the uniform colour every time. */
static inline uint8_t shadow_peek(int x, int y)
{
    const uint8_t *base;

    if (!shadow_any_real || !shadow_row_real[y])
        return shadow_uniform_colour;
    base = gfx_shadow_base();
    return base == NULL ? shadow_uniform_colour : base[(size_t)y * FB_WIDTH + x];
}

/* Everything the guest draws goes to both planes: the framebuffer, which is
 * what the display reads, and the shadow, which is what erasing text restores
 * from. Text does NOT come here -- that is the whole distinction. */
static inline void shadow_poke(int x, int y, uint8_t colour)
{
    uint8_t *row = shadow_row(y);

    if (row != NULL)
        row[x] = colour;
}

static void shadow_fill_span(int x0, int x1, int y, uint8_t colour)
{
    uint8_t *row;

    if (x1 <= x0)
        return;
    /* A span covering the whole width makes the row uniform again in its own
     * right, but tracking that per row buys little -- fill it. */
    row = shadow_row(y);
    if (row != NULL)
        memset(&row[x0], colour, (size_t)(x1 - x0));
}

void vidterm_shadow_reset(uint8_t colour)
{
    shadow_uniform_colour = colour;
    memset(shadow_row_real, 0, sizeof(shadow_row_real));
    shadow_any_real = false;
}

/* ────────────────────── text cell rendering (blitting) ──────────────────── */

/* A glyph row is exactly CHAR_W bytes and starts CHAR_W-aligned, so it is two
 * aligned words. Written inline rather than through memset: at eight bytes a
 * call costs more than the stores it makes, and there are sixteen of them per
 * cell and 3840 cells in a full-screen refresh. */
static inline void
fill_cell_row(uint8_t *line, uint8_t colour)
{
    const uint32_t v = (uint32_t)colour * 0x01010101u;
    uint32_t w[2] = { v, v };

    /* The alignment has to be ASSERTED. Rows are 4-aligned and CHAR_W is 8,
     * so every cell starts on a word -- but the compiler cannot see that
     * through the pointer arithmetic, and without the hint it emits eight
     * byte stores, which is what it was doing before this was a "fast path"
     * at all. Checked in the disassembly, not assumed. */
    memcpy(__builtin_assume_aligned(line, 4), w, CHAR_W);
}

static void OPTIMIZE_O3
blit_char_cell(int col, int row)
{
    uint8_t ch   = text_cells[row][col];
    uint8_t attr = text_attrs[row][col];

    uint8_t fg = attr_to_fg(attr);
    uint8_t bg = attr_to_bg(attr);
    /* Transparency is the attr's bit0 flag, NOT a colour sentinel.
     *
     * It used to mean "leave the pixel alone", which is why the console could
     * not use it: nothing erased, so an overwritten glyph left its strokes
     * behind. It now means "put back what the graphics plane has there",
     * which erases correctly AND lets whatever was drawn underneath show
     * through the gaps in the glyph. That is the whole of VID-025.
     *
     * With no shadow storage there is nothing to put back, so the cell falls
     * back to painting its background -- text hides graphics exactly as it
     * always did, rather than ghosting. Degrading to the old behaviour beats
     * degrading to a broken one. */
    bool transparent = (attr & 0x01) != 0 && gfx_shadow_base() != NULL;

    int x = col * CHAR_W;
    int y = row * CHAR_H;

    for (int gy = 0; gy < CHAR_H; gy++) {
        uint8_t mask = font_row(ch, gy);
        uint8_t *line = &video_framebuffer[y + gy][x];

        /* A uniform row is the common one, not a curiosity. A space is
         * blank in all sixteen of its rows, and after any clear, scroll or
         * erase most of the grid is spaces -- so the whole-screen refresh
         * that follows a clear is almost entirely this case. Testing the
         * mask once and filling beats testing it once per pixel: eight
         * branches and eight byte stores become one run. */
        if (mask == 0x00) {
            if (!transparent)
                fill_cell_row(line, bg);
            else if (shadow_any_real)
                memcpy(line, &shadow_row_ro(y + gy)[x], CHAR_W);
            else
                fill_cell_row(line, shadow_uniform_colour);
            continue;
        }
        if (mask == 0xFF) {
            fill_cell_row(line, fg);
            continue;
        }

        for (int gx = 0; gx < CHAR_W; gx++) {
            bool is_fg = (mask & (0x80 >> gx)) != 0;

            if (is_fg)
                line[gx] = fg;
            else if (!transparent)
                line[gx] = bg;
            else
                line[gx] = shadow_peek(x + gx, y + gy);
        }
    }
}

/* Instrumentation, because this has now been diagnosed wrongly twice. The
 * render timer says a call was slow; it cannot say whether the time went into
 * the text blitter or a drawing primitive, and both can be provoked by a
 * single guest byte. Counting cells separates them: a full-screen refresh is
 * 3840 of them and nothing else comes close. */
static uint32_t blit_cells_total;
static uint32_t blit_cells_max;

void vidterm_blit_stats(uint32_t *total, uint32_t *worst)
{
    *total = blit_cells_total;
    *worst = blit_cells_max;
}

/* A maximum only ever grows, so comparing two conditions means clearing it
 * between them. Learned the expensive way: two runs of different length were
 * once compared by their maxima and overstated a result threefold. */
void vidterm_blit_reset(void)
{
    blit_cells_total = 0;
    blit_cells_max = 0;
}

static void render_dirty_cells(void)
{
    uint32_t blitted = 0;

    for (int row = 0; row < TEXT_ROWS; row++) {
        {
            uint32_t any = 0;
            for (int w = 0; w < DIRTY_WORDS; w++) any |= dirty_rows[row][w];
            if (!any) continue;
        }

        for (int col = 0; col < TEXT_COLS; col++) {
            int word = col >> 5;
            uint32_t bit = 1u << (col & 31);
            if (dirty_rows[row][word] & bit) {
                blit_char_cell(col, row);
                blitted++;
                dirty_rows[row][word] &= ~bit;
            }
        }
    }
    blit_cells_total += blitted;
    if (blitted > blit_cells_max)
        blit_cells_max = blitted;
}

/* ───────────────────────── public API (text layer) ──────────────────────── */

void vidterm_putch(int col, int row, uint8_t ch, uint8_t attr)
{
    if (col < 0 || col >= TEXT_COLS || row < 0 || row >= TEXT_ROWS) return;

    text_cells[row][col] = ch;
    text_attrs[row][col] = attr;
    dirty_rows[row][col >> 5] |= (1u << (col & 31));
}

void vidterm_puts(int col, int row, const char *s, uint8_t attr)
{
    while (*s && row < TEXT_ROWS) {
        vidterm_putch(col, row, (uint8_t)*s++, attr);
        if (++col >= TEXT_COLS) { col = 0; row++; }
    }
}

static void mark_dirty_span(int row, int col, int cols);

/* Mark cols [col, col+cols) of one row dirty. Rows are tracked as a bitmap a
 * word at a time, so a region has to set bits rather than whole words -- the
 * full-screen case could fill words wholesale because it covered every
 * column, which a window does not. */
static void mark_dirty_span(int row, int col, int cols)
{
    for (int c = col; c < col + cols; c++)
        dirty_rows[row][c >> 5] |= 1u << (c & 31);
}

/* Row transfer between the live grid and a parked screen. Characters and
 * attributes are stored in separate planes here but travel together, so the
 * parked form interleaves them -- one contiguous run per row, which is what
 * makes a window's backing a simple block rather than two. */
void vidterm_read_row(int col, int row, int cols, uint8_t *dst)
{
    for (int i = 0; i < cols; i++) {
        dst[i * 2]     = text_cells[row][col + i];
        dst[i * 2 + 1] = text_attrs[row][col + i];
    }
}

void vidterm_write_row(int col, int row, int cols, const uint8_t *src)
{
    for (int i = 0; i < cols; i++) {
        text_cells[row][col + i] = src[i * 2];
        text_attrs[row][col + i] = src[i * 2 + 1];
    }
    mark_dirty_span(row, col, cols);
}

void vidterm_clear_region(int col, int row, int cols, int rows,
                          uint8_t ch, uint8_t attr)
{
    if (cols <= 0 || rows <= 0)
        return;
    for (int r = row; r < row + rows; r++) {
        memset(&text_cells[r][col], ch,   (size_t)cols);
        memset(&text_attrs[r][col], attr, (size_t)cols);
        mark_dirty_span(r, col, cols);
    }
}

/* Does this glyph put no pixels down at all? A space does not, and neither do
 * several other cells, and the answer decides whether a screenful of them is
 * half a million glyph blits or one fill. */
static bool glyph_is_blank(uint8_t ch)
{
    for (int gy = 0; gy < CHAR_H; gy++)
        if (font_row(ch, gy) != 0)
            return false;
    return true;
}

bool vidterm_clear(uint8_t ch, uint8_t attr)
{
    memset(text_cells, ch,   sizeof(text_cells));
    memset(text_attrs, attr, sizeof(text_attrs));

    /* A blank opaque cell renders as a flat field of its background, and the
     * grid covers the whole panel -- so a clear to blank IS a fill, and
     * blitting it cell by cell paints the same pixels a second time.
     *
     * Which pass to keep is not obvious and was measured: a whole-screen
     * memset costs about 16 ms here while the glyph blit covering the same
     * half-megabyte costs 6.6. The fill is the SLOWER of the two per byte,
     * because it sweeps contiguously and collides with the scan-out DMA that
     * outranks the cores, where the blit touches eight bytes and skips a
     * row. Doing one is still far better than doing both, which is what a
     * clear used to do. */
    if (glyph_is_blank(ch) && (attr & 0x01) == 0) {
        memset(video_framebuffer, attr_to_bg(attr),
               sizeof(video_framebuffer));
        memset(dirty_rows, 0, sizeof(dirty_rows));
        return true;		/* the panel is already the right colour */
    }

    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int w = 0; w < DIRTY_WORDS; w++)
            dirty_rows[r][w] = 0xFFFFFFFF;
        /* Mask off unused bits in the last word */
        if (TEXT_COLS & 31)
            dirty_rows[r][DIRTY_WORDS - 1] = (1u << (TEXT_COLS & 31)) - 1;
    }
    return false;		/* the blit still has to paint it */
}

void vidterm_refresh(void)
{
    render_dirty_cells();
}

/* Scroll a rectangle up one text row. Unlike the full-screen case this cannot
 * be one memmove: the rows of a window are not contiguous in the grid, so each
 * row is moved separately from the one below it. */
void vidterm_scroll_region(int col, int row, int cols, int rows,
                           uint8_t fill_ch, uint8_t fill_attr)
{
    if (cols <= 0 || rows <= 0)
        return;
    for (int r = row; r < row + rows - 1; r++) {
        memmove(&text_cells[r][col], &text_cells[r + 1][col], (size_t)cols);
        memmove(&text_attrs[r][col], &text_attrs[r + 1][col], (size_t)cols);
        mark_dirty_span(r, col, cols);
    }
    vidterm_clear_region(col, row + rows - 1, cols, 1, fill_ch, fill_attr);
}

void vidterm_scroll_up(uint8_t fill_ch, uint8_t fill_attr)
{
    memmove(text_cells[0], text_cells[1], (TEXT_ROWS - 1) * TEXT_COLS);
    memmove(text_attrs[0], text_attrs[1], (TEXT_ROWS - 1) * TEXT_COLS);
    memset(text_cells[TEXT_ROWS - 1], fill_ch, TEXT_COLS);
    memset(text_attrs[TEXT_ROWS - 1], fill_attr, TEXT_COLS);
    /* Every cell moved: mark the whole grid dirty (same masking as
     * vidterm_clear). */
    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int w = 0; w < DIRTY_WORDS; w++)
            dirty_rows[r][w] = 0xFFFFFFFF;
        if (TEXT_COLS & 31)
            dirty_rows[r][DIRTY_WORDS - 1] = (1u << (TEXT_COLS & 31)) - 1;
    }
}

/* ───────────────────────── public API (graphics layer) ──────────────────── */

/* The clip rectangle bounds the GUEST's drawing, and nothing else. Text
 * blitting, the cursor and the mouse pointer are the firmware's own and stay
 * screen-wide: a pointer that vanished at a window edge would be a bug, not a
 * feature.
 *
 * There is one graphics plane and it belongs to no window, so clipping does
 * not give a window its own drawing surface -- it bounds WHERE a primitive
 * lands. The pixels stay global and outlive any window switch. */
static int clip_x0, clip_y0;
static int clip_x1 = FB_WIDTH, clip_y1 = FB_HEIGHT;
static int origin_x, origin_y;

/* Origin and bounds are set separately because they stop coinciding the
 * moment a window's protection is turned off: coordinates stay measured from
 * the window, while the pixels are allowed out of it. Keeping them as one
 * call would make "unprotected" mean "drawn somewhere else", which is not
 * what a guest asking to draw outside its window means. */
void vidterm_set_origin(int x, int y)
{
    origin_x = x;
    origin_y = y;
}

void vidterm_set_clip(int x, int y, int w, int h)
{
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = (x + w > FB_WIDTH)  ? FB_WIDTH  : x + w;
    clip_y1 = (y + h > FB_HEIGHT) ? FB_HEIGHT : y + h;
}

/* The raster mix mode, set by the guest and persisting until changed. Normal
 * replaces; the others combine what is being drawn with what is already
 * there.
 *
 * WHAT "ALREADY THERE" MEANS is the graphics plane, NOT the framebuffer. The
 * framebuffer may have text on it, and a guest XORing a line across a window
 * full of characters means to combine with the drawing underneath them, not
 * with the glyphs. Reading the shadow keeps graphics operations independent
 * of whatever text happens to be on screen -- which is the same separation
 * that lets text be transparent in the first place.
 *
 * Only normal can use run-based drawing. The others are read-modify-write per
 * pixel and are several times slower; that is inherent, it is recorded in the
 * protocol document, and it is why normal is zero -- a guest that never sets
 * a mode never pays for one. */
/* The fill pattern, selected by PSet. When one is set, a filled primitive
 * takes its colour from the pattern tiled across the plane rather than from
 * the foreground -- tiled in SCREEN coordinates, so abutting fills line up
 * instead of each restarting the pattern at its own corner.
 *
 * Like the mix modes, a pattern costs the run-based fast path: every pixel
 * needs its own lookup. Buffer zero means none, which is the default, so
 * again a guest that asks for nothing pays for nothing. */
static uint8_t pattern_group, pattern_id;
static bool    pattern_on;

void vidterm_set_pattern(uint8_t group, uint8_t id)
{
    pattern_group = group;
    pattern_id = id;
    pattern_on = (id != 0u);
}

enum { LOGIC_NORMAL = 0, LOGIC_AND = 1, LOGIC_OR = 2, LOGIC_XOR = 3 };
static uint8_t logic_mode;

void vidterm_set_logic(uint8_t mode)
{
    logic_mode = (mode <= LOGIC_XOR) ? mode : LOGIC_NORMAL;
}

/* One pixel, in ABSOLUTE coordinates, already known to be inside the clip. */
static inline uint8_t pattern_at(int px, int py, uint8_t fallback)
{
    const struct gfx_buf *b;
    const uint8_t *p;

    if (!pattern_on)
        return fallback;
    b = gfx_find(pattern_group, pattern_id);
    p = gfx_buf_bytes(b);
    if (p == NULL || b->xsize == 0 || b->ysize == 0)
        return fallback;
    return p[(size_t)(py % b->ysize) * b->xsize + (px % b->xsize)];
}

static inline void put_pixel_abs(int px, int py, uint8_t src)
{
    const uint8_t colour = pattern_at(px, py, src);
    uint8_t v = colour;

    if (logic_mode != LOGIC_NORMAL) {
        const uint8_t base = shadow_peek(px, py);

        switch (logic_mode) {
        case LOGIC_AND: v = base & colour; break;
        case LOGIC_OR:  v = base | colour; break;
        default:        v = base ^ colour; break;
        }
    }
    video_framebuffer[py][px] = v;
    shadow_poke(px, py, v);
}

static inline void plot_unlocked(int x, int y, uint8_t colour)
{
    const int px = origin_x + x, py = origin_y + y;
    if (px >= clip_x0 && px < clip_x1 && py >= clip_y0 && py < clip_y1)
        put_pixel_abs(px, py, colour);
}

void vidterm_plot(int x, int y, uint8_t colour)
{
    plot_unlocked(x, y, colour);
}


void vidterm_fill_rect(int x, int y, int w, int h, uint8_t colour)
{
    int px0 = origin_x + x; if (px0 < clip_x0) px0 = clip_x0;
    int py0 = origin_y + y; if (py0 < clip_y0) py0 = clip_y0;
    int px1 = origin_x + x + w; if (px1 > clip_x1) px1 = clip_x1;
    int py1 = origin_y + y + h; if (py1 > clip_y1) py1 = clip_y1;
    if (px1 <= px0 || py1 <= py0)
        return;

    const size_t span = (size_t)(px1 - px0);

    /* Full-width spans are contiguous in the framebuffer, so the whole
     * rectangle is one run of memory rather than one per row. A cleared
     * screen or a full-width bar is the common large fill, and it is the
     * case where the per-row call overhead would be most of what is left. */
    if (px0 == 0 && px1 == FB_WIDTH && logic_mode == LOGIC_NORMAL && !pattern_on) {
        memset(&video_framebuffer[py0][0], colour,
               span * (size_t)(py1 - py0));
        for (int row = py0; row < py1; row++)
            shadow_fill_span(0, FB_WIDTH, row, colour);
        return;
    }

    /* Otherwise one run per row. A byte-at-a-time loop was costing about
     * four times this: the fill is the largest thing the renderer does, a
     * whole-panel bar is half a million pixels, and at 33 ms it outlasted a
     * frame on the core that owes the overlay service its deadline. */
    for (int row = py0; row < py1; row++) {
        if (logic_mode == LOGIC_NORMAL && !pattern_on) {
            memset(&video_framebuffer[row][px0], colour, span);
            shadow_fill_span(px0, px1, row, colour);
        } else {
            for (int i = px0; i < px1; i++)
                put_pixel_abs(i, row, colour);
        }
    }
}

void vidterm_hline(int x, int y, int len, uint8_t colour)
{
    const int py = origin_y + y;
    if (py < clip_y0 || py >= clip_y1) return;
    int px0 = origin_x + x; if (px0 < clip_x0) px0 = clip_x0;
    int px1 = origin_x + x + len; if (px1 > clip_x1) px1 = clip_x1;
    if (px1 <= px0)
        return;
    if (logic_mode == LOGIC_NORMAL && !pattern_on) {
        memset(&video_framebuffer[py][px0], colour, (size_t)(px1 - px0));
        shadow_fill_span(px0, px1, py, colour);
    } else {
        for (int i = px0; i < px1; i++)
            put_pixel_abs(i, py, colour);
    }
}

void vidterm_vline(int x, int y, int len, uint8_t colour)
{
    const int px = origin_x + x;
    if (px < clip_x0 || px >= clip_x1) return;
    int py0 = origin_y + y; if (py0 < clip_y0) py0 = clip_y0;
    int py1 = origin_y + y + len; if (py1 > clip_y1) py1 = clip_y1;
    for (int i = py0; i < py1; i++)
        put_pixel_abs(px, i, colour);
}

void vidterm_line(int x0, int y0, int x1, int y1, uint8_t colour)
{
    /* Bresenham, per-pixel clipped: endpoints may lie off-screen. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        plot_unlocked(x0, y0, colour);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void vidterm_clear_graphics(uint8_t colour)
{
    memset(video_framebuffer, colour, sizeof(video_framebuffer));
    /* The shadow becomes that colour everywhere WITHOUT being written: the
     * uniform state is exactly this case, and it is the one that would
     * otherwise cost 9 ms of PSRAM traffic on every clear. */
    vidterm_shadow_reset(colour);
}

/* Integer square root, because there is no FPU here and a circle should not
 * pull in soft-float. Bit-by-bit restoring method: exact for every input, and
 * bounded at sixteen iterations for the range a screen coordinate can reach. */
static uint32_t isqrt32(uint32_t n)
{
    uint32_t rem = 0, root = 0;

    for (int i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 30);
        n <<= 2;
        if (root < rem) {
            rem -= root | 1u;
            root += 2u;
        }
    }
    return root >> 1;
}

/* Midpoint ellipse, plotted in four-way symmetry. The two regions are where
 * the curve is shallower than 45 degrees and where it is steeper; splitting
 * them is what keeps the step decision a single comparison per pixel. */
void vidterm_ellipse(int cx, int cy, int rx, int ry, uint8_t colour)
{
    if (rx < 0 || ry < 0)
        return;
    if (rx == 0 || ry == 0) {	/* degenerate: a line, not a curve */
        if (rx == 0)
            vidterm_vline(cx, cy - ry, 2 * ry + 1, colour);
        else
            vidterm_hline(cx - rx, cy, 2 * rx + 1, colour);
        return;
    }

    const long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    long x = 0, y = ry;
    long d = ry2 - rx2 * ry + rx2 / 4;
    long dx = 0, dy = 2 * rx2 * ry;

    while (dx < dy) {			/* region 1 */
        plot_unlocked(cx + (int)x, cy + (int)y, colour);
        plot_unlocked(cx - (int)x, cy + (int)y, colour);
        plot_unlocked(cx + (int)x, cy - (int)y, colour);
        plot_unlocked(cx - (int)x, cy - (int)y, colour);
        x++;
        dx += 2 * ry2;
        d += ry2 + dx;
        if (d >= 0) {
            y--;
            dy -= 2 * rx2;
            d -= dy;
        }
    }

    d = ry2 * (x * 2 + 1) * (x * 2 + 1) / 4 +
        rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {			/* region 2 */
        plot_unlocked(cx + (int)x, cy + (int)y, colour);
        plot_unlocked(cx - (int)x, cy + (int)y, colour);
        plot_unlocked(cx + (int)x, cy - (int)y, colour);
        plot_unlocked(cx - (int)x, cy - (int)y, colour);
        y--;
        dy -= 2 * rx2;
        d += rx2 - dy;
        if (d <= 0) {
            x++;
            dx += 2 * ry2;
            d += dx;
        }
    }
}

/* Filled ellipse as horizontal spans, one per scanline. Deliberately not the
 * outline algorithm with the gaps joined up: a span is a run, and runs are
 * what this renderer is fast at -- the same reason rectangles stopped being
 * filled a pixel at a time. */
void vidterm_fill_ellipse(int cx, int cy, int rx, int ry, uint8_t colour)
{
    if (rx < 0 || ry < 0)
        return;
    if (ry == 0) {
        vidterm_hline(cx - rx, cy, 2 * rx + 1, colour);
        return;
    }

    for (int dy = -ry; dy <= ry; dy++) {
        /* half-width = rx * sqrt(ry^2 - dy^2) / ry, kept in integers. The
         * square root is taken of a value no larger than ry^2, so the
         * multiply that follows cannot overflow for any on-screen radius. */
        const uint32_t root = isqrt32((uint32_t)(ry * ry - dy * dy));
        const int dx = (int)(((uint32_t)rx * root) / (uint32_t)ry);

        vidterm_hline(cx - dx, cy + dy, 2 * dx + 1, colour);
    }
}

/* Record a loaded bitmap's shape and take its bytes. The payload arrives a
 * byte at a time as the link drains it, so this appends rather than copies a
 * block -- there is nowhere to assemble one first. */
void vidterm_buf_shape(uint8_t group, uint8_t id, uint16_t xs, uint16_t ys)
{
    struct gfx_buf *b = gfx_find(group, id);

    if (b != NULL) {
        b->xsize = xs;
        b->ysize = ys;
    }
}

bool vidterm_buf_poke(uint8_t group, uint8_t id, uint32_t at, uint8_t v)
{
    struct gfx_buf *b = gfx_find(group, id);
    uint8_t *p = gfx_buf_bytes(b);

    if (p == NULL || at >= b->len)
        return false;
    p[at] = v;
    return true;
}

/* GetBlk: a rectangle of the GRAPHICS plane into a buffer. The graphics
 * plane rather than the framebuffer, for the same reason the mix modes read
 * it -- a guest saving a region means the drawing, not whatever text is
 * sitting on top of it at the time. */
void vidterm_buf_get(uint8_t group, uint8_t id, int x, int y, int w, int h)
{
    struct gfx_buf *b = gfx_find(group, id);
    uint8_t *p = gfx_buf_bytes(b);

    if (p == NULL || w <= 0 || h <= 0)
        return;
    if ((uint32_t)w * (uint32_t)h > b->len)
        return;			/* would not fit: unanswered */

    b->xsize = (uint16_t)w;
    b->ysize = (uint16_t)h;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            p[(size_t)row * w + col] =
                shadow_peek(origin_x + x + col, origin_y + y + row);
}

/* PutBlk: a buffer back onto the screen, honouring the clip and the mix mode
 * exactly as a primitive does -- it IS drawing, and a guest that has set XOR
 * expects a stamped block to obey it. */
void vidterm_buf_put(uint8_t group, uint8_t id, int x, int y)
{
    const struct gfx_buf *b = gfx_find(group, id);
    const uint8_t *p = gfx_buf_bytes(b);

    if (p == NULL || b->xsize == 0 || b->ysize == 0)
        return;
    for (int row = 0; row < b->ysize; row++)
        for (int col = 0; col < b->xsize; col++)
            plot_unlocked(x + col, y + row,
                          p[(size_t)row * b->xsize + col]);
}

/* ───────────────────────────── flood fill ───────────────────────────────── */

/* Seeds waiting to be grown. A SCANLINE fill, so what is stacked is one seed
 * per run of unfilled pixels on the row above or below a completed span --
 * not one entry per pixel, which is what makes a bound of this size workable
 * where a per-pixel stack would need thousands.
 *
 * Bounded because there is about 7 KB of SRAM spare and a flood fill over a
 * 1024x480 plane cannot be allowed to decide for itself how much of it to
 * take. Recursion was never an option for the same reason: the C stack here
 * is 4 KB a core.
 *
 * WHEN THE BOUND IS REACHED the seed is dropped and counted. The region then
 * fills incompletely, which is visibly wrong -- but it is wrong in a way that
 * stops, rather than one that corrupts memory, and the counter says it
 * happened rather than leaving someone to wonder why a shape has a notch in
 * it. */
#define FILL_STACK_MAX	96u

static struct { int16_t x, y; } fill_stack[FILL_STACK_MAX];
static uint32_t fill_overflows;

void vidterm_fill_stats(uint32_t *overflows)
{
    *overflows = fill_overflows;
}

static inline void fill_span_abs(int px0, int px1, int py, uint8_t colour)
{
    memset(&video_framebuffer[py][px0], colour, (size_t)(px1 - px0));
    shadow_fill_span(px0, px1, py, colour);
}

void vidterm_flood_fill(int x, int y, uint8_t colour)
{
    const int sx = origin_x + x, sy = origin_y + y;

    if (sx < clip_x0 || sx >= clip_x1 || sy < clip_y0 || sy >= clip_y1)
        return;

    const uint8_t target = video_framebuffer[sy][sx];

    /* Filling a region with the colour it already is has nothing to do, and
     * attempting it would never terminate: every pixel examined would match
     * the target after being written, so every neighbour would be seeded
     * again forever. */
    if (target == colour)
        return;

    unsigned sp = 0;
    fill_stack[sp].x = (int16_t)sx;
    fill_stack[sp].y = (int16_t)sy;
    sp++;

    while (sp > 0u) {
        sp--;
        const int py = fill_stack[sp].y;
        int px = fill_stack[sp].x;

        /* A seed can be stale: another span may have reached it between
         * being pushed and being popped. */
        if (video_framebuffer[py][px] != target)
            continue;

        int x1 = px, x2 = px;
        while (x1 > clip_x0 && video_framebuffer[py][x1 - 1] == target)
            x1--;
        while (x2 < clip_x1 - 1 && video_framebuffer[py][x2 + 1] == target)
            x2++;

        fill_span_abs(x1, x2 + 1, py, colour);

        /* One seed per RUN on each neighbouring row, not per pixel: after
         * seeding a run, skip to its end. That is the whole economy of the
         * scanline method. */
        for (int ny = py - 1; ny <= py + 1; ny += 2) {
            if (ny < clip_y0 || ny >= clip_y1)
                continue;
            int i = x1;
            while (i <= x2) {
                if (video_framebuffer[ny][i] != target) {
                    i++;
                    continue;
                }
                if (sp < FILL_STACK_MAX) {
                    fill_stack[sp].x = (int16_t)i;
                    fill_stack[sp].y = (int16_t)ny;
                    sp++;
                } else {
                    fill_overflows++;
                }
                while (i <= x2 && video_framebuffer[ny][i] == target)
                    i++;
            }
        }
    }
}

void vidterm_circle(int cx, int cy, int radius, uint8_t colour)
{
    int x = 0, y = radius;
    int d = 3 - 2 * radius;

    while (x <= y) {
        plot_unlocked(cx + x, cy + y, colour);
        plot_unlocked(cx - x, cy + y, colour);
        plot_unlocked(cx + x, cy - y, colour);
        plot_unlocked(cx - x, cy - y, colour);
        plot_unlocked(cx + y, cy + x, colour);
        plot_unlocked(cx - y, cy + x, colour);
        plot_unlocked(cx + y, cy - x, colour);
        plot_unlocked(cx - y, cy - x, colour);

        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* ─────────────────────────────── bring-up ───────────────────────────────── */

/* Cursor state (see video_render.h for why none of this runs in an ISR). */
int               video_cursor_col;
int               video_cursor_row;
bool              video_cursor_enabled;
volatile uint32_t video_frames;

static uint8_t cursor_save[CHAR_H][CHAR_W];   /* pixels the cursor covers */
static bool    cursor_drawn;
static int     cursor_drawn_col;
static int     cursor_drawn_row;
static uint32_t cursor_shown_at;              /* video_frames when last shown */

/* ── Mouse pointer ────────────────────────────────────────────────────────
 * Same save-under discipline as the text cursor, with a shape and a mask
 * instead of a cell inversion. Both overlays are lifted and restored together
 * (see vidterm_cursor_hide / vidterm_cursor_service), so their ordering is
 * decided here rather than by whichever call site happens to run first. */
#define PTR_W 8
#define PTR_H 12

/* A plain arrow. 1 = draw, 0 = show what is underneath. The outline is drawn
 * in the background colour so the pointer stays visible over any content. */
static const uint8_t ptr_shape[PTR_H] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xFE, 0xFF, 0xF8, 0xD8, 0x8C, 0x0C
};
static const uint8_t ptr_edge[PTR_H] = {   /* 1 = outline pixel */
    0x80, 0x40, 0x20, 0x10, 0x08, 0x04,
    0x02, 0x01, 0x08, 0x48, 0x84, 0x0C
};

static int     pointer_x, pointer_y;
static bool    pointer_enabled;
/* A guest sprite replaces the built-in arrow when one is set. Capped at
 * 16x16 because the pointer is a save-under overlay drawn into the
 * framebuffer and its cost is paid on every movement; anything larger uses
 * its top-left corner. The hotspot is 0,0 BY DEFINITION -- there is no field
 * to carry one, so a guest draws its point at the corner, as the built-in
 * arrow does.
 *
 * $E3 is transparent: bright magenta, the traditional key precisely because
 * nobody wants it deliberately. A sprite with no transparency would be a
 * rectangle. */
#define PTR_MAX		16
#define PTR_KEY		0xE3u

static const uint8_t *ptr_sprite;	/* NULL: use the built-in arrow */
static int            ptr_sprite_w, ptr_sprite_h;

void vidterm_set_cursor_sprite(const uint8_t *data, int w, int h)
{
    if (data == NULL || w <= 0 || h <= 0) {
        ptr_sprite = NULL;
        return;
    }
    ptr_sprite = data;
    ptr_sprite_w = w > PTR_MAX ? PTR_MAX : w;
    ptr_sprite_h = h > PTR_MAX ? PTR_MAX : h;
}

static inline int ptr_width(void)
{
    return ptr_sprite != NULL ? ptr_sprite_w : PTR_W;
}

static inline int ptr_height(void)
{
    return ptr_sprite != NULL ? ptr_sprite_h : PTR_H;
}

static uint8_t pointer_save[PTR_MAX][PTR_MAX];
static bool    pointer_drawn;
static int     pointer_drawn_x, pointer_drawn_y;

void vidterm_pointer_set(int x, int y)
{
    if (x < 0) x = 0;
    if (x > FB_WIDTH  - 1) x = FB_WIDTH  - 1;
    if (y < 0) y = 0;
    if (y > FB_HEIGHT - 1) y = FB_HEIGHT - 1;
    pointer_x = x;
    pointer_y = y;
    pointer_enabled = true;      /* first movement brings it on screen */
}

void vidterm_pointer_enable(bool on)
{
    pointer_enabled = on;
}

static void pointer_hide(void)
{
    if (!pointer_drawn)
        return;
    for (int gy = 0; gy < ptr_height(); gy++) {
        const int y = pointer_drawn_y + gy;
        if (y >= FB_HEIGHT)
            break;
        int w = ptr_width();
        if (pointer_drawn_x + w > FB_WIDTH)
            w = FB_WIDTH - pointer_drawn_x;
        memcpy(&video_framebuffer[y][pointer_drawn_x], pointer_save[gy], (size_t)w);
    }
    pointer_drawn = false;
}

static void pointer_show(void)
{
    if (pointer_drawn || !pointer_enabled)
        return;
    for (int gy = 0; gy < ptr_height(); gy++) {
        const int y = pointer_y + gy;
        if (y >= FB_HEIGHT)
            break;
        int w = ptr_width();
        if (pointer_x + w > FB_WIDTH)
            w = FB_WIDTH - pointer_x;
        uint8_t *line = &video_framebuffer[y][pointer_x];
        memcpy(pointer_save[gy], line, (size_t)w);
        for (int gx = 0; gx < w; gx++) {
            if (ptr_sprite != NULL) {
                const uint8_t px =
                    ptr_sprite[(size_t)gy * ptr_sprite_w + gx];

                if (px != PTR_KEY)
                    line[gx] = px;
            } else {
                const uint8_t bit = (uint8_t)(0x80u >> gx);

                if (ptr_shape[gy] & bit)
                    line[gx] = (ptr_edge[gy] & bit) ? 0x00u : 0xFFu;
            }
        }
    }
    pointer_drawn = true;
    pointer_drawn_x = pointer_x;
    pointer_drawn_y = pointer_y;
}

void vidterm_cursor_hide(void)
{
    /* Both overlays: the pointer sits on top, so it comes off first. */
    pointer_hide();

    if (!cursor_drawn)
        return;
    const int x = cursor_drawn_col * CHAR_W;
    const int y = cursor_drawn_row * CHAR_H;
    for (int gy = 0; gy < CHAR_H; gy++)
        memcpy(&video_framebuffer[y + gy][x], cursor_save[gy], CHAR_W);
    cursor_drawn = false;
}

static void cursor_service(void)
{
    /* Blink phase. A move restarts the cycle (cursor_shown_at), so the cursor
     * is always visible the instant it arrives somewhere — blinking off on
     * arrival reads as a dropped keystroke. */
    const bool phase =
        (((video_frames - cursor_shown_at) / VIDEO_CURSOR_BLINK_FRAMES) & 1u) == 0u;
    const bool want = video_cursor_enabled && phase;

    if (cursor_drawn && (!want ||
        cursor_drawn_col != video_cursor_col ||
        cursor_drawn_row != video_cursor_row))
        vidterm_cursor_hide();

    if (!want || cursor_drawn)
        return;

    /* Invert the cell: over white-on-black text this is the classic block
     * cursor, and the glyph beneath stays legible in reverse video. */
    const int x = video_cursor_col * CHAR_W;
    const int y = video_cursor_row * CHAR_H;
    for (int gy = 0; gy < CHAR_H; gy++) {
        uint8_t *line = &video_framebuffer[y + gy][x];
        memcpy(cursor_save[gy], line, CHAR_W);
        for (int gx = 0; gx < CHAR_W; gx++)
            line[gx] = (uint8_t)~line[gx];
    }
    cursor_drawn = true;
    cursor_drawn_col = video_cursor_col;
    cursor_drawn_row = video_cursor_row;
}

static void pointer_service(void)
{
    if (pointer_drawn &&
        (!pointer_enabled || pointer_drawn_x != pointer_x ||
         pointer_drawn_y != pointer_y))
        pointer_hide();
    pointer_show();
}

/* The one public entry. Order matters: the cursor is laid down first and the
 * pointer over it, so a pointer resting on the cursor cell hides it rather
 * than the reverse, and each save-under holds what was genuinely beneath. */
void vidterm_cursor_service(void)
{
    cursor_service();
    pointer_service();
}

void vidterm_cursor_set(int col, int row)
{
    if (col < 0) col = 0;
    if (col >= TEXT_COLS) col = TEXT_COLS - 1;
    if (row < 0) row = 0;
    if (row >= TEXT_ROWS) row = TEXT_ROWS - 1;
    if (col != video_cursor_col || row != video_cursor_row)
        cursor_shown_at = video_frames;   /* restart the blink cycle */
    video_cursor_col = col;
    video_cursor_row = row;
}

void vidterm_cursor_enable(bool on)
{
    if (on && !video_cursor_enabled)
        cursor_shown_at = video_frames;
    video_cursor_enabled = on;
    if (!on)
        vidterm_cursor_hide();
}

void video_render_init(void)
{
    video_cursor_col = 0;
    video_cursor_row = 0;
    video_cursor_enabled = false;   /* nothing owns the screen yet */
    cursor_drawn = false;
    pointer_enabled = false;        /* appears on first movement */
    pointer_drawn = false;
    pointer_x = FB_WIDTH / 2;
    pointer_y = FB_HEIGHT / 2;

    memset(video_framebuffer, 0, sizeof(video_framebuffer));
    memset(text_cells, 0, sizeof(text_cells));
    memset(text_attrs, 0, sizeof(text_attrs));
    memset(dirty_rows, 0, sizeof(dirty_rows));
}

/* ────────────────────────────── test screen ─────────────────────────────── */

#ifdef VIDEO_DEMO
void video_demo(void)
{
    // Blue background with a 1-pixel white border.
    vidterm_clear_graphics(rgb332(0, 0, 3));
    uint8_t white = rgb332(7, 7, 3);
    vidterm_hline(0, 0, FB_WIDTH, white);
    vidterm_hline(0, FB_HEIGHT - 1, FB_WIDTH, white);
    vidterm_vline(0, 0, FB_HEIGHT, white);
    vidterm_vline(FB_WIDTH - 1, 0, FB_HEIGHT, white);

    // Text: row numbers, a column ruler, and the full character set.
    vidterm_clear(' ', 0xE1);  /* white on transparent */

    char buf[4];
    for (int r = 0; r < TEXT_ROWS; r++) {
        snprintf(buf, sizeof(buf), "%2d", r);
        vidterm_puts(1, r, buf, 0xE1);
    }
    for (int c = 0; c < TEXT_COLS; c++)
        vidterm_putch(c, 1, (c % 10 == 0) ? '|' : '-', 0xC1);  /* yellow */
    for (int c = 0; c < TEXT_COLS; c += 10) {
        snprintf(buf, sizeof(buf), "%d", c);
        vidterm_puts(c, 0, buf, 0xC1);
    }
    int col = 4, row = 3;
    for (int ch = 0x20; ch < 0x100; ch++) {
        vidterm_putch(col, row, (uint8_t)ch, 0xE1);
        col += 2;  /* double-spaced */
        if (col >= TEXT_COLS - 1) {
            col = 4;
            row += 2;
            if (row >= TEXT_ROWS) break;
        }
    }
    vidterm_refresh();

    // Diagonal corner-to-corner lines over the graphics layer.
    uint8_t red = rgb332(7, 0, 0), green = rgb332(0, 7, 0);
    for (int i = 0; i < FB_HEIGHT; i++) {
        int x1 = (i * (FB_WIDTH - 1)) / (FB_HEIGHT - 1);
        int x2 = (FB_WIDTH - 1) - x1;
        vidterm_plot(x1, i, red);
        vidterm_plot(x2, i, green);
    }
}
#endif  /* VIDEO_DEMO */

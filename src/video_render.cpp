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
#include "fill_log.h"
#include "gfx_bufs.h"
#include "font_spleen_8x16.h"

/* ───────────────────────── compile-time parameters ──────────────────────── */

/* Screen geometry (FB_WIDTH/FB_HEIGHT/CHAR_W/CHAR_H/TEXT_COLS/TEXT_ROWS) is the
 * single source of truth in video.h, so consumers cannot drift from it. */

#define TRANSPARENT_BG  0x00

/* This file is compiled at -O3 in every optimised build, by CMakeLists.txt,
 * not by a function attribute. The attribute that used to do it was GCC-only
 * -- clang rejects it -- so it was an empty macro in the one build that
 * actually gets flashed (RelWithDebInfo with ATfE), and the hot blitter ran at
 * -O2 there for its whole life while this comment said otherwise. A build
 * rule cannot quietly become a no-op when the compiler changes. */

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
static uint8_t pattern_group, pattern_id;
static bool    pattern_on;
void vidterm_set_pattern(uint8_t group, uint8_t id);

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
    /* The pattern is a kept pointer too -- into the cache when it fitted,
     * into the pool when it did not. Either way the selection has to go when
     * its buffer does. */
    if (pattern_on && pattern_group == group && pattern_id == id)
        vidterm_set_pattern(group, 0u);
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

/* What the LAST block command decoded to, as opposed to what the guest sent.
 * Those are the same thing only while the stream keeps its framing, and when
 * they differ the difference is the whole diagnosis: a stamp landing at a
 * coordinate the guest never asked for says a byte was lost upstream, and a
 * stamp landing where it was asked says the loss is somewhere else. Reading
 * the parameters back is the only way to tell those apart from this end --
 * the picture cannot, because a plausible wrong coordinate draws a perfectly
 * ordinary-looking block. */
static uint32_t blk_get_count, blk_put_count;
static int16_t blk_get_x, blk_get_y, blk_get_w, blk_get_h;
static int16_t blk_put_x, blk_put_y;
static uint8_t blk_get_id, blk_put_id;

void vidterm_blk_stats(struct vidterm_blk_info *o)
{
    o->gets = blk_get_count;
    o->puts = blk_put_count;
    o->get_x = blk_get_x; o->get_y = blk_get_y;
    o->get_w = blk_get_w; o->get_h = blk_get_h;
    o->put_x = blk_put_x; o->put_y = blk_put_y;
    o->get_id = blk_get_id; o->put_id = blk_put_id;
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

/* Copy to and from PSRAM with 32-BIT accesses, because memcpy will not.
 *
 * Measured on hardware (the "psram" console command): a single-byte read
 * access costs 45 ns and a 32-bit read 93, which fits 29 ns fixed + 16 ns a
 * byte -- so four bytes in one transaction is nearly twice as cheap per byte
 * as one. memcpy does not exploit that at all: it managed 51 ns/byte reading
 * and 36 writing, against 23 and 20 for the explicit loops below. It is
 * behaving as though it were byte-at-a-time, and it cost this renderer half
 * the available gain until it was measured.
 *
 * LONGER IS NOT BETTER, which kills the obvious next idea. memcpy measured
 * FLAT at 32, 120 and 256 bytes (51/51/51 reading), so there is no burst mode
 * to reach for by staging a whole block instead of a row -- and therefore no
 * reason to want the kilobyte of SRAM that would have needed. Four bytes an
 * access is the whole of the win; the asymptotic floor is 16 ns/byte and a
 * word already reaches 23.
 *
 * The PSRAM SIDE is the one kept aligned, because it is the side that costs.
 * The SRAM side takes whatever alignment falls out and is copied through
 * __builtin_memcpy, which may become four byte stores -- to SRAM that is
 * free, and it keeps the code honest about alignment instead of casting a
 * pointer it has not proved.
 *
 * VERIFY THE DISASSEMBLY after touching these. A "word store fast path" in
 * this file once emitted eight byte stores because the compiler could not
 * prove alignment, gained 2 %, and looked correct in the source the whole
 * time. */
static inline void
copy_from_psram(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n != 0u && ((uintptr_t)src & 3u) != 0u) {
        *dst++ = *src++;
        n--;
    }
    const uint32_t *s32 = (const uint32_t *)(const void *)src;
    while (n >= 4u) {
        const uint32_t w = *s32++;

        __builtin_memcpy(dst, &w, 4);
        dst += 4;
        n -= 4u;
    }
    src = (const uint8_t *)(const void *)s32;
    while (n-- != 0u)
        *dst++ = *src++;
}

static inline void
copy_to_psram(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n != 0u && ((uintptr_t)dst & 3u) != 0u) {
        *dst++ = *src++;
        n--;
    }
    uint32_t *d32 = (uint32_t *)(void *)dst;
    while (n >= 4u) {
        uint32_t w;

        __builtin_memcpy(&w, src, 4);
        *d32++ = w;
        src += 4;
        n -= 4u;
    }
    dst = (uint8_t *)(void *)d32;
    while (n-- != 0u)
        *dst++ = *src++;
}

/* A shadow row about to be written from x0 to x1. When the span is the whole
 * row there is nothing to preserve, so an undrawn row is marked real WITHOUT
 * first being filled with its uniform colour -- a 1 KB PSRAM write that would
 * be overwritten at once, and on a full-panel patterned fill that was 480 KB
 * of wasted traffic. */
static inline uint8_t *shadow_row_for_write(int y, int x0, int x1)
{
    uint8_t *base = gfx_shadow_base();

    if (base == NULL)
        return NULL;
    if (!shadow_row_real[y] && x0 == 0 && x1 == FB_WIDTH) {
        shadow_row_real[y] = true;
        shadow_any_real = true;
        return &base[(size_t)y * FB_WIDTH];
    }
    return shadow_row(y);
}

static void shadow_fill_span(int x0, int x1, int y, uint8_t colour)
{
    uint8_t *row;

    if (x1 <= x0)
        return;
    /* A full-width span overwrites the whole row, so an undrawn row is not
     * first filled with its uniform colour only to be overwritten -- the
     * span writer already skipped that, and this path did not: a plain
     * full-panel FFill over undrawn rows wrote every row twice, ~20 ms of
     * its measured 45. (Making the row UNIFORM again, rather than real,
     * would save the second write too, but needs a per-row colour -- one
     * byte a row, and SRAM is tight; see VID-029.) */
    row = shadow_row_for_write(y, x0, x1);
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

/* A glyph row is eight 1-bit pixels; the framebuffer row it lands on is eight
 * BYTES. This table widens four mask bits to four byte-lane masks at once, so
 * a row is composed as two words -- (fg AND mask) OR (background AND NOT mask)
 * -- instead of eight tested, branched, byte-sized stores. Pixel 0 is the most
 * significant bit of the mask and the LOWEST address, hence byte 0 of the word
 * on a little-endian part (asserted below). const, so it lives in flash and
 * costs none of the SRAM this board has not got. */
static const uint32_t glyph_expand[16] = {
    0x00000000u, 0xFF000000u, 0x00FF0000u, 0xFFFF0000u,
    0x0000FF00u, 0xFF00FF00u, 0x00FFFF00u, 0xFFFFFF00u,
    0x000000FFu, 0xFF0000FFu, 0x00FF00FFu, 0xFFFF00FFu,
    0x0000FFFFu, 0xFF00FFFFu, 0x00FFFFFFu, 0xFFFFFFFFu,
};
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "glyph_expand puts pixel 0 in byte 0 of each word");

/* Eight framebuffer or shadow bytes as two words. Both a cell row and a shadow
 * row are 4-aligned (rows are 1024 bytes, cells 8), but the compiler cannot see
 * that through the arithmetic and would otherwise emit byte accesses -- the
 * mistake fill_cell_row once made while calling itself a fast path -- so the
 * alignment is ASSERTED, and memcpy keeps it free of aliasing casts. */
static inline void
cell_row_put(uint8_t *line, uint32_t w0, uint32_t w1)
{
    const uint32_t w[2] = { w0, w1 };

    memcpy(__builtin_assume_aligned(line, 4), w, CHAR_W);
}

static inline void
cell_row_get(const uint8_t *src, uint32_t *w0, uint32_t *w1)
{
    uint32_t w[2];

    memcpy(w, __builtin_assume_aligned(src, 4), CHAR_W);
    *w0 = w[0];
    *w1 = w[1];
}

/* sbase is the graphics shadow, fetched ONCE per refresh by the caller rather
 * than once per cell -- or, as it was, once per background PIXEL through
 * shadow_peek(), each a call into another translation unit that itself called
 * win_store_slots().
 *
 * FORCED INLINE, on measurement. With one call site clang inlined it; the run
 * path gave it a second, clang then kept it out of line, and a full opaque
 * refresh -- 3840 calls -- went from 6.19 to 6.68 ms on hardware. */
static inline __attribute__((always_inline)) void
blit_char_cell(int col, int row, const uint8_t *sbase)
{
    uint8_t ch   = text_cells[row][col];
    uint8_t attr = text_attrs[row][col];

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
    const bool transparent = (attr & 0x01) != 0 && sbase != NULL;
    const uint32_t fgw = (uint32_t)attr_to_fg(attr) * 0x01010101u;
    const uint32_t bgw = (uint32_t)attr_to_bg(attr) * 0x01010101u;
    const uint32_t uniw = (uint32_t)shadow_uniform_colour * 0x01010101u;
    const int x = col * CHAR_W;
    const int y = row * CHAR_H;

    /* WHAT THIS REPLACED, measured on hardware over a full 3840-cell refresh:
     * 22.1 ms opaque, 26.9 ms transparent, 62.5 ms transparent over drawn
     * graphics -- 3.8 frames. Mixed rows went pixel by pixel, and every
     * transparent background pixel called shadow_peek(): flag checks, a call
     * into another file, and a single-byte uncached PSRAM read, which came
     * out at 73 ns a byte -- slower even than a plain byte loop. Now each row
     * is at most two 32-bit shadow reads, one table lookup per half and two
     * word stores. */
    for (int gy = 0; gy < CHAR_H; gy++) {
        const uint8_t mask = font_row(ch, gy);
        uint8_t *line = &video_framebuffer[y + gy][x];
        uint32_t b0, b1;

        /* A solid row needs no background, so it must not pay a PSRAM
         * read to find one. */
        if (mask == 0xFF) {
            cell_row_put(line, fgw, fgw);
            continue;
        }

        /* The background: the cell's own colour, or the graphics plane.
         * An unmaterialised shadow row IS the uniform colour -- most of the
         * plane never is, that being the point -- and must never be read.
         * This once tested shadow_any_real and dereferenced the row pointer
         * regardless, so a cell in an undrawn row, while some OTHER row was
         * real, read from address x: 0 to 1016, the bootrom, rendered as
         * pixels. It needs a screen part drawn and part not to show, which
         * is every real screen and no contrived one. */
        if (!transparent) {
            b0 = b1 = bgw;
        } else if (shadow_row_real[y + gy]) {
            cell_row_get(&sbase[(size_t)(y + gy) * FB_WIDTH + (size_t)x],
                         &b0, &b1);
        } else {
            b0 = b1 = uniw;
        }

        /* A blank row -- every space, and most of any cleared screen -- is
         * just the background. */
        if (mask == 0x00) {
            cell_row_put(line, b0, b1);
            continue;
        }

        const uint32_t m0 = glyph_expand[mask >> 4];
        const uint32_t m1 = glyph_expand[mask & 0x0Fu];

        cell_row_put(line, (fgw & m0) | (b0 & ~m0), (fgw & m1) | (b1 & ~m1));
    }
}

/* Up to RUN_CELLS neighbouring dirty cells of one text row, with any
 * transparent ones reading the shadow a PIXEL ROW AT A TIME across the run.
 *
 * blit_char_cell() walks one cell top to bottom, so its shadow reads are
 * eight bytes and then a 1 KB jump to the next pixel row. Measured ("psram",
 * 2026-09-26) that is 345 ns per 8 bytes, where 8 bytes following on from
 * the last cost 188 -- the QMI continues a transfer when the next address is
 * consecutive, and re-sends command and address when it is not. A refresh
 * over drawn graphics paid the strided price on every row of every cell.
 * Here each pixel row of the run is read as one consecutive slice, into a
 * buffer, back to back, and only then composed: a new transaction per slice
 * rather than per cell.
 *
 * Eight cells, so the slice is 64 bytes on a 2 KB stack -- the same budget
 * paint_span() keeps. Wider would amortise the one strided open per slice a
 * little further; it is a small fraction of what remains.
 *
 * The caller sends only text rows with a DRAWN shadow row among their
 * sixteen, and a run with no transparent cell goes straight back to the
 * per-cell path: in either case there is nothing to read, and taking the
 * run path anyway cost opaque and undrawn-plane refreshes 6-7 % on
 * hardware for no gain. */
#define RUN_CELLS 8

static void
blit_cell_run(int row, int c0, uint32_t bits, const uint8_t *sbase)
{
    uint32_t fgw[RUN_CELLS], bgw[RUN_CELLS];
    uint8_t chs[RUN_CELLS];
    uint32_t transp = 0;

    /* Transparency first, alone: a run without any must not pay for
     * colours the per-cell path is about to work out again. */
    for (uint32_t b = bits; b != 0u; b &= b - 1u) {
        const int i = __builtin_ctz(b);

        if ((text_attrs[row][c0 + i] & 0x01) != 0)
            transp |= 1u << i;
    }
    if (transp == 0u) {
        for (uint32_t b = bits; b != 0u; b &= b - 1u)
            blit_char_cell(c0 + __builtin_ctz(b), row, sbase);
        return;
    }
    for (uint32_t b = bits; b != 0u; b &= b - 1u) {
        const int i = __builtin_ctz(b);
        const uint8_t attr = text_attrs[row][c0 + i];

        chs[i] = text_cells[row][c0 + i];
        fgw[i] = (uint32_t)attr_to_fg(attr) * 0x01010101u;
        bgw[i] = (uint32_t)attr_to_bg(attr) * 0x01010101u;
    }

    /* The slice spans the first to the last TRANSPARENT cell; opaque cells
     * between them are read and ignored, which is cheaper than breaking the
     * run in two. */
    const int lo = __builtin_ctz(transp);
    const int words = (32 - __builtin_clz(transp) - lo) * 2;
    const uint32_t uniw = (uint32_t)shadow_uniform_colour * 0x01010101u;
    const int x = c0 * CHAR_W;
    const int y = row * CHAR_H;
    uint32_t slice[RUN_CELLS * 2];

    for (int gy = 0; gy < CHAR_H; gy++) {
        const bool real = shadow_row_real[y + gy];
        uint8_t *line = &video_framebuffer[y + gy][x];

        /* All of the row's PSRAM reads, consecutive and uninterrupted. */
        if (real) {
            const uint32_t *src = (const uint32_t *)__builtin_assume_aligned(
                &sbase[(size_t)(y + gy) * FB_WIDTH +
                       (size_t)(x + lo * CHAR_W)], 4);

            for (int w = 0; w < words; w++)
                slice[lo * 2 + w] = src[w];
        }

        for (uint32_t b = bits; b != 0u; b &= b - 1u) {
            const int i = __builtin_ctz(b);
            const uint8_t mask = font_row(chs[i], gy);
            uint8_t *cell = &line[i * CHAR_W];
            uint32_t b0, b1;

            if (mask == 0xFF) {
                cell_row_put(cell, fgw[i], fgw[i]);
                continue;
            }
            if ((transp & (1u << i)) == 0u) {
                b0 = b1 = bgw[i];
            } else if (real) {
                b0 = slice[i * 2];
                b1 = slice[i * 2 + 1];
            } else {
                b0 = b1 = uniw;
            }
            if (mask == 0x00) {
                cell_row_put(cell, b0, b1);
                continue;
            }

            const uint32_t m0 = glyph_expand[mask >> 4];
            const uint32_t m1 = glyph_expand[mask & 0x0Fu];

            cell_row_put(cell, (fgw[i] & m0) | (b0 & ~m0),
                         (fgw[i] & m1) | (b1 & ~m1));
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

/* Blit dirty cells, at most max_rows text rows of them, and say whether any
 * remain. Rows are taken top to bottom from wherever the dirty bits are, so a
 * bounded caller simply comes back and the next call carries on. */
static bool render_dirty_rows(int max_rows)
{
    uint32_t blitted = 0;
    int rows_done = 0;
    bool left = false;
    /* Once per refresh: the storage cannot change underneath a refresh, and
     * asking per cell was a cross-file call chain paid 3840 times. */
    const uint8_t *sbase = gfx_shadow_base();

    for (int row = 0; row < TEXT_ROWS; row++) {
        {
            uint32_t any = 0;
            for (int w = 0; w < DIRTY_WORDS; w++) any |= dirty_rows[row][w];
            if (!any) continue;
        }
        if (rows_done == max_rows) {
            left = true;
            break;
        }
        rows_done++;

        /* Runs pay off only where there is shadow to read: some pixel row
         * of this text row drawn. Otherwise every cell's background is
         * its own colour or the plane's uniform one, and the per-cell path
         * finds either without reading anything. */
        bool drawn = false;
        if (sbase != NULL && shadow_any_real)
            for (int gy = 0; gy < CHAR_H && !drawn; gy++)
                drawn = shadow_row_real[row * CHAR_H + gy];
        if (!drawn) {
            for (int w = 0; w < DIRTY_WORDS; w++)
                for (uint32_t b = dirty_rows[row][w]; b != 0u; b &= b - 1u) {
                    blit_char_cell(w * 32 + __builtin_ctz(b), row, sbase);
                    blitted++;
                }
            for (int w = 0; w < DIRTY_WORDS; w++)
                dirty_rows[row][w] = 0;
            continue;
        }

        static_assert(32 % RUN_CELLS == 0 && TEXT_COLS % RUN_CELLS == 0,
                      "a run is a whole slice of one dirty word");
        for (int c0 = 0; c0 < TEXT_COLS; c0 += RUN_CELLS) {
            const uint32_t bits = (dirty_rows[row][c0 >> 5] >> (c0 & 31)) &
                                  ((1u << RUN_CELLS) - 1u);

            if (bits != 0u) {
                blit_cell_run(row, c0, bits, sbase);
                blitted += (uint32_t)__builtin_popcount(bits);
            }
        }
        for (int w = 0; w < DIRTY_WORDS; w++)
            dirty_rows[row][w] = 0;
    }
    blit_cells_total += blitted;
    if (blitted > blit_cells_max)
        blit_cells_max = blitted;
    return left;
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
    (void)render_dirty_rows(TEXT_ROWS);
}

/* A refresh the guest's stream can ask for without costing the render core its
 * frame. A full-screen transparent refresh over drawn graphics measured 28.7 ms
 * -- 1.7 frames -- and cannot fit one at any speed: reading a fully drawn plane
 * is ~11 ms even sequentially, plus the composing. So the work is spread: at
 * most VIDTERM_REFRESH_ROWS_MAX text rows per call, the caller returning until
 * it is done. The screen then finishes over two frames instead of one frame
 * arriving late (agreed with Mark 2026-09-26). vidterm_refresh() stays
 * complete, for callers that have to finish before anything else draws. */
bool vidterm_refresh_bounded(void)
{
    return render_dirty_rows(VIDTERM_REFRESH_ROWS_MAX);
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
/* The pattern is RESOLVED ONCE, and copied into SRAM when it is small enough
 * to fit. Looking it up per pixel was the first implementation and it was
 * badly wrong twice over: a thirty-two entry table scan AND an uncached PSRAM
 * read for every pixel of a fill. A full-screen patterned Bar is half a
 * million pixels, so that is sixteen million comparisons and half a million
 * PSRAM transactions -- tens of milliseconds, for a lookup into 256 bytes
 * that could sit in SRAM.
 *
 * The same mistake was already avoided for the font, which is resolved once
 * at select time for exactly this reason. The pattern should have followed it
 * and did not. */
#define PATTERN_CACHE_BYTES	256u	/* a 16x16 weave, the usual size */

/* THE PATTERN RULE, taken from CoCo3's GrfDrv (level2/cmds/grfdrv.asm) rather
 * than from memory -- a first version here got it wrong twice:
 *
 *  SCOPE: every primitive that plots pixels goes through the pattern -- Point,
 *    Line, Box, Bar, the curves outlined and filled, FFill, AND PutBlk. The
 *    only exemption on CoCo3 is the graphics cursor; here the cursor and the
 *    mouse pointer are overlays that never pass through this writer at all.
 *  COMBINE: the pixel written is colour AND pattern-pixel, then the mix mode.
 *    CoCo3 ANDs palette indices; this plane is direct RGB332, so the bytes
 *    are ANDed -- with a white foreground the pattern's own colours show
 *    verbatim, and where the pattern is 0 the pixel becomes 0 (CoCo3 writes
 *    colour 0 there too; it is not transparent). For PutBlk "colour" is the
 *    SOURCE pixel, so a stamp is the image ANDed with the pattern.
 *  ANCHOR: the tile is indexed from the WINDOW origin and follows each pixel
 *    as it is plotted, so abutting fills in a window line up.
 *
 * Two deliberate divergences, both Pico extensions: the tile is whatever size
 * its buffer is (CoCo3 always reads 32x8), and PutBlk tiles per pixel --
 * GrfDrv's slow PUT never advances its pattern index, freezing one pattern
 * byte across the whole block, which looks like an oversight (agreed with
 * Mark 2026-09-26).
 *
 * The history, because it will look tempting again. First the pattern
 * REPLACED the colour in the writer everything shares, so a stamp showed the
 * pattern instead of its image; then it was confined to fills (1943bae),
 * which hid that bug by removing the pattern from everything CoCo3 patterns.
 * Both were wrong. VID-033. */

static uint8_t       pattern_cache[PATTERN_CACHE_BYTES];
static const uint8_t *pattern_data;	/* cache, or straight into PSRAM */
static int           pattern_w, pattern_h;

void vidterm_set_pattern(uint8_t group, uint8_t id)
{
    uint16_t xs = 0, ys = 0;
    const uint8_t *p = (id == 0u) ? NULL :
        vidterm_buf_data(group, id, NULL, &xs, &ys);

    pattern_group = group;
    pattern_id = id;

    if (p == NULL || xs == 0 || ys == 0) {
        pattern_on = false;
        pattern_data = NULL;
        return;
    }
    pattern_w = xs;
    pattern_h = ys;
    if ((uint32_t)xs * ys <= PATTERN_CACHE_BYTES) {
        memcpy(pattern_cache, p, (size_t)xs * ys);
        pattern_data = pattern_cache;
    } else {
        /* Too big to cache. The table scan is still gone, which is the
         * larger of the two costs; the PSRAM read per pixel remains and a
         * guest using a pattern this large has chosen that. */
        pattern_data = p;
    }
    pattern_on = true;
}

enum { LOGIC_NORMAL = 0, LOGIC_AND = 1, LOGIC_OR = 2, LOGIC_XOR = 3 };
static uint8_t logic_mode;

void vidterm_set_logic(uint8_t mode)
{
    logic_mode = (mode <= LOGIC_XOR) ? mode : LOGIC_NORMAL;
}

/* One pixel, in ABSOLUTE coordinates, already known to be inside the clip. */
static inline uint8_t pattern_apply(int px, int py, uint8_t colour)
{
    if (!pattern_on)
        return colour;
    /* Window-relative, and wrapped properly: an unprotected window may draw
     * left of or above its own origin, where a plain % goes negative. */
    int tx = (px - origin_x) % pattern_w;
    int ty = (py - origin_y) % pattern_h;

    if (tx < 0)
        tx += pattern_w;
    if (ty < 0)
        ty += pattern_h;
    return (uint8_t)(colour & pattern_data[(size_t)ty * pattern_w + tx]);
}

static inline void put_pixel_abs(int px, int py, uint8_t src)
{
    const uint8_t colour = pattern_apply(px, py, src);
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

/* ONE SPAN of patterned and/or mixed pixels, x0..x1 absolute and already
 * clipped: exactly what calling put_pixel_abs() for each pixel does, without
 * paying for it per pixel (VID-035).
 *
 * The per-pixel path measured ~400 ns a pixel on hardware -- a full-panel
 * patterned fill 195 ms, 11.7 frames in one render call, and wgfx's patterned
 * Bar ~70 ms every run. Each pixel made two modulo operations, a call chain
 * into another file for the shadow's base, and a single-byte uncached PSRAM
 * write. The same shape the text blitter had before it was made word-wise.
 *
 * Here the span is composed IN THE FRAMEBUFFER ROW, which must be written
 * anyway, so no staging array is needed beyond a 64-byte chunk for a PutBlk
 * source (core 0's stack is 2 KB; a row-sized buffer would take half of it):
 *   - the mix base comes from the shadow ONCE per chunk, as a word copy, or is
 *     the uniform colour of an undrawn row -- read, never materialised;
 *   - the pattern row is resolved once and stepped without division;
 *   - the shadow gets ONE sequential word write for the whole span, which is
 *     the cheap PSRAM access pattern.
 * `src` is NULL for a constant colour, or a PutBlk's source row (which may be
 * in PSRAM). The rule itself -- colour AND pattern, then the mix mode against
 * the graphics plane, tiled from the window origin -- is unchanged; see the
 * pattern notes above. */
#define PAINT_CHUNK 64

/* Combine n bytes of the mix base into dst in place. The mode is decided
 * once, outside the loop, and each loop is plain enough to run a byte a
 * cycle or two -- the per-pixel three-way choice was a large part of the
 * ~60 ns a pixel the generic loop cost. */
static void mix_into(uint8_t *dst, const uint8_t *base, int n)
{
    switch (logic_mode) {
    case LOGIC_AND: for (int i = 0; i < n; i++) dst[i] &= base[i]; break;
    case LOGIC_OR:  for (int i = 0; i < n; i++) dst[i] |= base[i]; break;
    default:        for (int i = 0; i < n; i++) dst[i] ^= base[i]; break;
    }
}

static void mix_uniform(uint8_t *dst, uint8_t b, int n)
{
    switch (logic_mode) {
    case LOGIC_AND: for (int i = 0; i < n; i++) dst[i] &= b; break;
    case LOGIC_OR:  for (int i = 0; i < n; i++) dst[i] |= b; break;
    default:        for (int i = 0; i < n; i++) dst[i] ^= b; break;
    }
}

/* The patterned colour for n pixels starting at absolute x0 on row py:
 * colour AND the pattern, tiled from the window origin.
 *
 * With no source image the row is PERIODIC -- it repeats every pattern
 * width -- so only one period is composed and the rest is copied, doubling
 * each time. The period is first grown to a multiple of four bytes, so every
 * copy after it has source and destination aligned alike and moves words;
 * a pattern three pixels wide would otherwise copy a byte at a time for the
 * width of the panel. The pattern row comes through copy_from_psram() when
 * it lives in PSRAM, so a wide pattern is read once a row, a word at a
 * time, instead of a byte per pixel. */
static void pattern_row(uint8_t *dst, int n, int py, int x0, uint8_t colour)
{
    const int pw = pattern_w;
    int ty = (py - origin_y) % pattern_h;
    int tx = (x0 - origin_x) % pw;

    if (ty < 0)
        ty += pattern_h;
    if (tx < 0)
        tx += pw;

    const uint8_t *const prow = &pattern_data[(size_t)ty * (size_t)pw];
    const bool cached = pattern_data == pattern_cache;
    const int first = n < pw ? n : pw;
    const int k1 = first < pw - tx ? first : pw - tx;

    /* One period, rotated to the span's phase: the tail of the pattern row
     * from tx, then its head. */
    if (cached) {
        memcpy(dst, &prow[tx], (size_t)k1);
        memcpy(&dst[k1], prow, (size_t)(first - k1));
    } else {
        copy_from_psram(dst, &prow[tx], (size_t)k1);
        copy_from_psram(&dst[k1], prow, (size_t)(first - k1));
    }
    if (colour != 0xFFu)
        for (int i = 0; i < first; i++)
            dst[i] &= colour;

    /* Grow to a word-multiple period, then double. Each copy is from the
     * start of the span to a whole number of periods along it, so the
     * phase is right, and source and destination never overlap. */
    const int p4 = (pw & 1) ? pw * 4 : (pw & 2) ? pw * 2 : pw;
    int done = first;

    while (done < n && done < p4) {
        const int c = (n < p4 ? n : p4) - done < done ?
            (n < p4 ? n : p4) - done : done;

        memcpy(&dst[done], dst, (size_t)c);
        done += c;
    }
    while (done < n) {
        const int c = n - done < done ? n - done : done;

        memcpy(&dst[done], dst, (size_t)c);
        done += c;
    }
}

static void paint_span(int py, int x0, int x1, uint8_t colour, const uint8_t *src)
{
    /* A constant colour: the common case -- every fill, Bar, hline and
     * FFill -- and the one where the row is periodic. Compose the patterned
     * colour straight into the framebuffer row, apply the mix mode against
     * the graphics plane a chunk at a time, and write the shadow once. */
    if (src == NULL) {
        uint8_t *const line = &video_framebuffer[py][x0];
        const int n = x1 - x0;

        if (pattern_on)
            pattern_row(line, n, py, x0, colour);
        else
            memset(line, colour, (size_t)n);
        if (logic_mode != LOGIC_NORMAL) {
            const uint8_t *const sbase = gfx_shadow_base();

            if (sbase != NULL && shadow_row_real[py]) {
                const uint8_t *const srow = &sbase[(size_t)py * FB_WIDTH];
                uint8_t base[PAINT_CHUNK];

                for (int a = 0; a < n; a += PAINT_CHUNK) {
                    const int k = n - a < PAINT_CHUNK ? n - a : PAINT_CHUNK;

                    copy_from_psram(base, &srow[x0 + a], (size_t)k);
                    mix_into(&line[a], base, k);
                }
            } else {
                mix_uniform(line, shadow_uniform_colour, n);
            }
        }

        uint8_t *const srow = shadow_row_for_write(py, x0, x1);

        if (srow != NULL)
            copy_to_psram(&srow[x0], line, (size_t)n);
        return;
    }

    /* A PutBlk source row: not periodic, so composed as it comes. */
    uint8_t *const line = &video_framebuffer[py][0];
    const uint8_t *const sbase = gfx_shadow_base();
    const uint8_t *const srow_ro = (sbase != NULL && shadow_row_real[py]) ?
        &sbase[(size_t)py * FB_WIDTH] : NULL;
    const bool mixing = logic_mode != LOGIC_NORMAL;
    const uint8_t *prow = NULL;
    uint8_t prow_local[PAINT_CHUNK];
    int tx = 0;

    if (pattern_on) {
        int ty = (py - origin_y) % pattern_h;

        if (ty < 0)
            ty += pattern_h;
        prow = &pattern_data[(size_t)ty * (size_t)pattern_w];
        /* A pattern too big for the SRAM cache is read from PSRAM; take
         * its row into SRAM once rather than a byte per pixel. */
        if (prow != &pattern_cache[(size_t)ty * (size_t)pattern_w] &&
            pattern_w <= PAINT_CHUNK) {
            copy_from_psram(prow_local, prow, (size_t)pattern_w);
            prow = prow_local;
        }
        tx = (x0 - origin_x) % pattern_w;
        if (tx < 0)
            tx += pattern_w;
    }

    uint8_t tmp[PAINT_CHUNK];

    for (int a = x0; a < x1; a += PAINT_CHUNK) {
        const int n = (x1 - a < PAINT_CHUNK) ? x1 - a : PAINT_CHUNK;

        if (src != NULL)
            copy_from_psram(tmp, &src[a - x0], (size_t)n);
        if (mixing) {
            if (srow_ro != NULL)
                copy_from_psram(&line[a], &srow_ro[a], (size_t)n);
            else
                memset(&line[a], shadow_uniform_colour, (size_t)n);
        }
        for (int i = 0; i < n; i++) {
            uint8_t c = (src != NULL) ? tmp[i] : colour;

            if (prow != NULL) {
                c &= prow[tx];
                if (++tx == pattern_w)
                    tx = 0;
            }
            if (mixing) {
                const uint8_t b = line[a + i];

                c = (logic_mode == LOGIC_AND) ? (uint8_t)(b & c) :
                    (logic_mode == LOGIC_OR) ? (uint8_t)(b | c) :
                    (uint8_t)(b ^ c);
            }
            line[a + i] = c;
        }
    }

    uint8_t *const srow = shadow_row_for_write(py, x0, x1);

    if (srow != NULL)
        copy_to_psram(&srow[x0], &line[x0], (size_t)(x1 - x0));
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

    /* THE WHOLE PANEL makes the shadow uniform by definition, so it needs no
     * shadow traffic at all -- just a new uniform colour and every row marked
     * not-real. That is the entire point of keeping the plane implicit, and
     * this path was missing it: shadow_fill_span() per row materialised each
     * row with a 1024-byte PSRAM memset and then filled it with another, so a
     * full-screen fill moved nearly a MEGABYTE across the QMI to reach a
     * state describable by one byte and a flag.
     *
     * Measured on hardware before this: 24,148 us for a full-screen fill,
     * which is one and a half frames, on the core that owes the overlay
     * service its deadline. The self-test found it -- the budget it passed
     * against was 25,000 us, which is how narrowly a figure that large
     * escaped notice.
     *
     * It must be the whole panel and not merely full width: a full-width bar
     * of partial height leaves the rows above and below it holding something
     * else, and there is no per-row uniform colour to express that. (There
     * could be, at one byte a row -- see the fill below, which is the case
     * that would benefit.) */
    if (px0 == 0 && px1 == FB_WIDTH && py0 == 0 && py1 == FB_HEIGHT &&
        logic_mode == LOGIC_NORMAL && !pattern_on) {
        memset(&video_framebuffer[0][0], colour,
               (size_t)FB_WIDTH * (size_t)FB_HEIGHT);
        vidterm_shadow_reset(colour);
        return;
    }

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
            paint_span(row, px0, px1, colour, NULL);
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
        paint_span(py, px0, px1, colour, NULL);
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

    /* Recorded before the early returns: a command that was decoded and then
     * refused looks identical from outside to one that never arrived. */
    blk_get_count = blk_get_count + 1u;
    blk_get_id = id;
    blk_get_x = (int16_t)x; blk_get_y = (int16_t)y;
    blk_get_w = (int16_t)w; blk_get_h = (int16_t)h;

    if (p == NULL || w <= 0 || h <= 0)
        return;
    if ((uint32_t)w * (uint32_t)h > b->len)
        return;			/* would not fit: unanswered */

    b->xsize = (uint16_t)w;
    b->ysize = (uint16_t)h;

    /* A ROW AT A TIME, not a pixel at a time. Both operands live in PSRAM
     * behind the QMI, reached through the uncached alias -- it has to be
     * uncached, because the XIP cache is not write-through for PSRAM and the
     * per-core caches are not coherent. So every BUS ACCESS here is its own
     * QSPI transaction: command, address, dummy cycles, then data. The
     * command and address dominate, which is why a byte cost about 150 ns
     * against the 19 ns a 53 MB/s bandwidth figure suggests, and why the fix
     * is fewer, wider accesses rather than faster ones.
     *
     * The off-panel bound from the per-pixel version is kept, and is now
     * three explicit spans instead of a test per pixel: the guest chooses
     * these coordinates and nothing else clips them, so an off-panel
     * rectangle used to index shadow_row_real[] past its end and read past a
     * shadow row -- benign-looking on a host, a potential bus fault on
     * hardware, where PSRAM beyond the declared region faults. Off-panel
     * pixels still read as 0. */
    const int px0 = origin_x + x;
    int lo = px0 < 0 ? 0 : px0;			/* first on-panel column */
    int hi = px0 + w > FB_WIDTH ? FB_WIDTH : px0 + w;	/* one past last */

    if (hi < lo)
        hi = lo;
    for (int row = 0; row < h; row++) {
        const int py = origin_y + y + row;
        uint8_t *dst = &p[(size_t)row * (size_t)w];
        const uint8_t *srow;

        if (py < 0 || py >= FB_HEIGHT) {
            memset(dst, 0, (size_t)w);		/* wholly off-panel */
            continue;
        }
        /* The margins, where the rectangle hangs off the side. */
        if (lo > px0)
            memset(dst, 0, (size_t)(lo - px0));
        if (px0 + w > hi)
            memset(&dst[hi - px0], 0, (size_t)(px0 + w - hi));
        if (hi <= lo)
            continue;
        /* The middle. A row nothing has drawn on is the uniform colour by
         * definition, so it is a memset of SRAM and costs no QSPI traffic at
         * all -- which is the common case for a mostly-clear screen and the
         * reason the plane is kept implicit. */
        srow = shadow_row_ro(py);
        if (srow == NULL)
            memset(&dst[lo - px0], shadow_uniform_colour,
                   (size_t)(hi - lo));
        else
            copy_from_psram(&dst[lo - px0], &srow[lo], (size_t)(hi - lo));
    }
}

/* PutBlk: a buffer back onto the screen, honouring the clip and the mix mode
 * exactly as a primitive does -- it IS drawing, and a guest that has set XOR
 * expects a stamped block to obey it. */
void vidterm_buf_put(uint8_t group, uint8_t id, int x, int y)
{
    const struct gfx_buf *b = gfx_find(group, id);
    const uint8_t *p = gfx_buf_bytes(b);

    blk_put_count = blk_put_count + 1u;
    blk_put_id = id;
    blk_put_x = (int16_t)x; blk_put_y = (int16_t)y;

    if (p == NULL || b->xsize == 0 || b->ysize == 0)
        return;

    const int w = b->xsize, h = b->ysize;

    /* A mix mode or a fill pattern makes every pixel depend on what is
     * already there, or on where it lands, so there is nothing to copy in
     * bulk -- take the per-pixel path, which is also the one that defines
     * what those modes MEAN. Correctness first: this is the fallback, and it
     * is the original routine unchanged. */
    const int bx0 = origin_x + x;
    const int blo = bx0 < clip_x0 ? clip_x0 : bx0;
    const int bhi = bx0 + w > clip_x1 ? clip_x1 : bx0 + w;

    /* A mix mode or a pattern makes each pixel depend on the background or
     * on its position, so the source cannot simply be copied: each clipped
     * row goes through the span writer, which applies both at span cost
     * rather than per-pixel cost (VID-035). */
    if (logic_mode != LOGIC_NORMAL || pattern_on) {
        if (bhi <= blo)
            return;
        for (int row = 0; row < h; row++) {
            const int py = origin_y + y + row;

            if (py < clip_y0 || py >= clip_y1)
                continue;
            paint_span(py, blo, bhi, 0,
                       &p[(size_t)row * (size_t)w + (size_t)(blo - bx0)]);
        }
        return;
    }

    /* The ordinary stamp, a row at a time. Three things made the per-pixel
     * version cost what it did, and the first two are QSPI transactions:
     * reading the source byte from the buffer in PSRAM, and writing the
     * shadow byte to PSRAM. At roughly 150 ns each (command, address, dummy
     * cycles, one byte -- the overhead IS the cost) a 32x32 stamp paid about
     * 300 us before touching the framebuffer.
     *
     * NO STAGING BUFFER, deliberately. The framebuffer row has to be written
     * anyway, and once written it holds exactly the bytes the shadow wants,
     * so it IS the staging area: one bulk read PSRAM->framebuffer, then one
     * bulk write framebuffer->shadow. A separate staging array would have
     * cost SRAM this design has not got: the device-role build has 648 bytes
     * left before the link fails (measured -- a 1 KB static in the PSRAM
     * benchmark proved it by failing), which is also why the flood fill's
     * seed stack is bounded at 96. */
    const int px0 = origin_x + x;
    int lo = px0 < clip_x0 ? clip_x0 : px0;
    int hi = px0 + w > clip_x1 ? clip_x1 : px0 + w;

    if (hi <= lo)
        return;				/* wholly outside the clip */
    for (int row = 0; row < h; row++) {
        const int py = origin_y + y + row;
        const size_t n = (size_t)(hi - lo);
        uint8_t *srow;

        if (py < clip_y0 || py >= clip_y1)
            continue;
        copy_from_psram(&video_framebuffer[py][lo],
                        &p[(size_t)row * (size_t)w + (size_t)(lo - px0)], n);
        srow = shadow_row(py);
        if (srow != NULL)
            copy_to_psram(&srow[lo], &video_framebuffer[py][lo], n);
    }
}

/* ───────────────────────────── flood fill ───────────────────────────────── */

/* Work waiting to be done, as SEGMENTS: "the run [x1, x2] on row y is filled;
 * look at row y + dy beneath it". This is the parent-span scanline fill
 * (Heckbert, Graphics Gems I), and the parent is the point of it. The fill it
 * replaced stacked bare seed points and, filling a run, scanned BOTH
 * neighbouring rows across its whole width -- including the row it had just
 * come from, which was known to be filled over exactly the parent's extent.
 * That second scan was a third of the region search, all of it finding
 * nothing. Here a row is scanned only in the direction the work is going,
 * and back towards the parent only where a run LEAKS past the parent's ends.
 *
 * The stack lives in PSRAM (fill_log.h), one packed word a segment. It was a
 * 96-entry static in SRAM, sized by what SRAM could spare rather than by what
 * a picture needs, and an ordinary busy region overflowed it by hundreds --
 * dropping work and leaving holes (VID-037). Recursion was never an option:
 * the C stack here is 4 KB a core.
 *
 * With no PSRAM there is nowhere large to put it, so a small stack on the
 * call's own frame stands in, and the fill degrades to what it always was:
 * bounded, and counted when the bound is reached. That costs SRAM only for
 * the length of the call, where the static cost it permanently.
 *
 * WHEN THE BOUND IS REACHED the segment is dropped and counted. The region
 * then fills incompletely, which is visibly wrong -- but it is wrong in a way
 * that stops, rather than one that corrupts memory, and the counter says it
 * happened rather than leaving someone to wonder why a shape has a notch in
 * it. */
#define FILL_STACK_SRAM	64u

/* x1 and x2 in ten bits each, y in nine, the direction in one. */
static inline uint32_t seg_pack(int y, int x1, int x2, int dy)
{
    return ((dy < 0) ? (1u << 29) : 0u) | ((uint32_t)y << 20) |
           ((uint32_t)x1 << 10) | (uint32_t)x2;
}

static uint32_t fill_overflows;		/* seeds dropped */
static uint32_t fill_log_overflows;	/* spans filled solid, not patterned */

void vidterm_fill_stats(uint32_t *overflows, uint32_t *unpatterned)
{
    *overflows = fill_overflows;
    *unpatterned = fill_log_overflows;
}

/* A span in one word, for the PSRAM log: row, first and last column. */
static inline uint32_t span_pack(int y, int x1, int x2)
{
    return ((uint32_t)y << 20) | ((uint32_t)x1 << 10) | (uint32_t)x2;
}

/* THE REGION SEARCH, A WORD AT A TIME. Finding the region is most of a flood
 * fill that is not patterned: every pixel is compared about three times (its
 * own row's extent, then the row above and the row below looking for seeds),
 * and done a byte at a time that measured ~24 ms of a 35 ms full-panel fill.
 * These answer the same three questions from four bytes at once. XOR with
 * the target repeated in each byte turns "equals the target" into "is a zero
 * byte"; pixel 0 is the lowest address and so the least significant byte on
 * this part, which is why the forward searches count TRAILING zeros. The
 * answers are exactly those of the byte loops they replace -- the usim oracle
 * runs the old fill beside this one and demands the same pixels. */
static inline uint32_t fb_word(const uint8_t *p)
{
    uint32_t w;

    memcpy(&w, __builtin_assume_aligned(p, 4), 4);
    return w;
}

/* First j in [i, end) whose pixel is NOT t, else end. */
static int run_end(const uint8_t *r, int i, int end, uint8_t t)
{
    const uint32_t tw = (uint32_t)t * 0x01010101u;

    for (; i < end && (i & 3) != 0; i++)
        if (r[i] != t)
            return i;
    for (; i + 4 <= end; i += 4) {
        const uint32_t d = fb_word(&r[i]) ^ tw;

        if (d != 0u)
            return i + (__builtin_ctz(d) >> 3);
    }
    for (; i < end; i++)
        if (r[i] != t)
            return i;
    return end;
}

/* First j in [i, end) whose pixel IS t, else end. A zero byte of d is a
 * match; the borrow trick below can flag a byte ABOVE a true zero falsely,
 * never one below it, so the lowest flag is exact. */
static int run_find(const uint8_t *r, int i, int end, uint8_t t)
{
    const uint32_t tw = (uint32_t)t * 0x01010101u;

    for (; i < end && (i & 3) != 0; i++)
        if (r[i] == t)
            return i;
    for (; i + 4 <= end; i += 4) {
        const uint32_t d = fb_word(&r[i]) ^ tw;
        const uint32_t z = (d - 0x01010101u) & ~d & 0x80808080u;

        if (z != 0u)
            return i + (__builtin_ctz(z) >> 3);
    }
    for (; i < end; i++)
        if (r[i] == t)
            return i;
    return end;
}

/* Smallest j >= lo with every pixel in [j, i) equal to t: the run's start,
 * searching LEFT from i. The first mismatch going left is the highest one in
 * a word, hence leading zeros. */
static int run_start(const uint8_t *r, int i, int lo, uint8_t t)
{
    const uint32_t tw = (uint32_t)t * 0x01010101u;

    for (; i > lo && (i & 3) != 0; i--)
        if (r[i - 1] != t)
            return i;
    for (; i - 4 >= lo; i -= 4) {
        const uint32_t d = fb_word(&r[i - 4]) ^ tw;

        if (d != 0u)
            return i - (__builtin_clz(d) >> 3);
    }
    for (; i > lo; i--)
        if (r[i - 1] != t)
            return i;
    return lo;
}

static inline void fill_span_abs(int px0, int px1, int py, uint8_t colour)
{
    /* The plain case only: vidterm_flood_fill() routes patterned and
     * mix-mode fills through its two-pass path instead. */
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

    /* TWO PASSES when the pixels written are not simply `colour` (VID-034).
     * This fill finds its way BY COLOUR -- a pixel equal to the target is
     * one it has not reached -- and a pattern or mix mode breaks that: a
     * pixel just filled can still equal the target (white AND a black
     * pattern pixel, on black), so it re-seeds its own work, overflows the
     * seed stack and quits with most of the region untouched. Measured on
     * the host before this: 6,144 of 491,520 pixels.
     *
     * So pass 1 floods SOLID -- guaranteed to terminate, since every filled
     * pixel then differs from the target -- into the framebuffer ONLY,
     * logging each span. Pass 2 repaints exactly those spans through the
     * ordinary pixel path, pattern and mix mode applied, framebuffer and
     * shadow. Pass 1 leaves the shadow alone, so a mix mode still combines
     * with the true background. The plain fill keeps its single pass. */
    const bool plain = !pattern_on && logic_mode == LOGIC_NORMAL;
    uint32_t *const log = plain ? NULL : fill_log_base();
    uint32_t logged = 0;

    uint32_t sram_stack[FILL_STACK_SRAM];
    uint32_t *stack = fill_seeds_base();
    const uint32_t stack_max = stack != NULL ? FILL_SEEDS : FILL_STACK_SRAM;

    if (stack == NULL)
        stack = sram_stack;

    uint32_t sp = 0;

    /* Stack the segment [a, b] of row y for a look at row y + dy -- if that
     * row is inside the clip at all. */
    auto push = [&](int y, int a, int b, int dy) {
        if (y + dy < clip_y0 || y + dy >= clip_y1)
            return;
        if (sp < stack_max)
            stack[sp++] = seg_pack(y, a, b, dy);
        else
            fill_overflows++;
    };

    /* Claim the run [a, b] of row y: fill it, and for a two-pass fill, log
     * it for painting. */
    auto claim = [&](int y, int a, int b) {
        if (plain) {
            fill_span_abs(a, b + 1, y, colour);
        } else {
            memset(&video_framebuffer[y][a], colour, (size_t)(b + 1 - a));
            if (log != NULL && logged < FILL_LOG_SPANS) {
                log[logged++] = span_pack(y, a, b);
            } else {
                /* Past the log's end, or no PSRAM: this span stays solid.
                 * Visibly unpatterned, but consistent -- the shadow gets
                 * the same bytes -- and COUNTED, like a dropped segment. */
                shadow_fill_span(a, b + 1, y, colour);
                fill_log_overflows++;
            }
        }
    };

    {
        const uint8_t *const row = video_framebuffer[sy];
        const int a = run_start(row, sx, clip_x0, target);
        const int b = run_end(row, sx, clip_x1, target) - 1;

        claim(sy, a, b);
        push(sy, a, b, 1);
        push(sy, a, b, -1);
    }

    while (sp > 0u) {
        const uint32_t seg = stack[--sp];
        const int dy = (seg & (1u << 29)) ? -1 : 1;
        const int y = (int)((seg >> 20) & 0x1FFu) + dy;
        const int x1 = (int)((seg >> 10) & 0x3FFu);
        const int x2 = (int)(seg & 0x3FFu);
        const uint8_t *const row = video_framebuffer[y];

        /* Every unfilled run of row y that touches the parent's extent.
         * Work stacked earlier may already have filled some or all of it;
         * a run that is gone is simply not found. */
        for (int x = x1;;) {
            x = run_find(row, x, x2 + 1, target);
            if (x > x2)
                break;

            /* The run may reach past the parent at either end. */
            const int a = run_start(row, x, clip_x0, target);
            const int b = run_end(row, x, clip_x1, target) - 1;

            claim(y, a, b);
            push(y, a, b, dy);
            /* Where it leaks past the parent, the row the parent was on is
             * unexplored beneath the leak -- and only there. */
            if (a < x1)
                push(y, a, x1 - 1, -dy);
            if (b > x2)
                push(y, x2 + 1, b, -dy);
            x = b + 1;
        }
    }

    /* Pass 2: exactly the spans pass 1 claimed, through the pixel path. */
    for (uint32_t i = 0; i < logged; i++) {
        const uint32_t w = log[i];
        const int py = (int)(w >> 20);
        const int x1 = (int)((w >> 10) & 0x3FFu);
        const int x2 = (int)(w & 0x3FFu);

        paint_span(py, x1, x2 + 1, colour, NULL);
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

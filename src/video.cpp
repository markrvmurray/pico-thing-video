/*
 * video.c — HSTX->DVI text+graphics engine for pico-thing-video.
 *
 * Spliced from ../pico-vidterm (based on raspberrypi pico-examples
 * hstx/dvi_out_hstx_encoder). Changes for this project:
 *   - Output is brought up by video_init() from Core 1 (not main()).
 *   - The per-scanline DMA IRQ moved from DMA_IRQ_0 to DMA_IRQ_2, because the
 *     6809 bus engine owns DMA_IRQ_0 (read) and DMA_IRQ_1 (write).
 *   - The demo is now video_demo(), gated by VIDEO_DEMO (default on).
 *   - System clock is 192 MHz (set in main()); HSTX CLKDIV=5 -> 38.4 MHz pixel.
 *
 * Hardware: RP2350 + HDMI/DVI breakout on GP12-19. RGB332 framebuffer in SRAM.
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include "pico_thing_video.h"   // shared pin map (GPIO_HSTX_BASE / GPIO_HSTX_COUNT)
#include "video.h"
#include "font_spleen_8x16.h"

/* ───────────────────────── compile-time parameters ──────────────────────── */

// 1024 active. 1056 overflows SRAM by ~6 KB — the framebuffer (1 byte/pixel)
// plus the 80x30-ish text layer leaves no room for the extra 32 columns.
// The 32 dropped columns are added to the H back porch below so the 60 Hz
// horizontal total (1216) is unchanged.
#define FB_WIDTH       1024
#define FB_HEIGHT      480

#define CHAR_W           8
#define CHAR_H          16
#define TEXT_COLS       (FB_WIDTH / CHAR_W)   /* 128 */
#define TEXT_ROWS       (FB_HEIGHT / CHAR_H)  /*  30 */

#define TRANSPARENT_BG  0x00

/* Per-scanline video IRQ. Bus engine owns DMA_IRQ_0/1, so video uses IRQ 2. */
#define VIDEO_DMA_IRQ_IDX 2
#define VIDEO_DMA_IRQ     DMA_IRQ_2

/* GPIO_HSTX_BASE / GPIO_HSTX_COUNT come from the shared pin map. */

/* ──────────────────────────── framebuffer (SRAM) ──────────────────────── */

// RGB332 framebuffer in SRAM. DMA reads directly from here.
static uint8_t framebuffer[FB_HEIGHT][FB_WIDTH] __attribute__((aligned(4)));

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

static inline uint8_t font_row(uint8_t ch, int row)
{
    return font_spleen_8x16[ch][row & (CHAR_H - 1)];
}

/* ────────────────────── text cell rendering (blitting) ──────────────────── */

static void __attribute__((optimize("O3")))
blit_char_cell(int col, int row)
{
    uint8_t ch   = text_cells[row][col];
    uint8_t attr = text_attrs[row][col];

    uint8_t fg = attr_to_fg(attr);
    uint8_t bg = attr_to_bg(attr);
    bool transparent = (bg == TRANSPARENT_BG);

    int x = col * CHAR_W;
    int y = row * CHAR_H;

    for (int gy = 0; gy < CHAR_H; gy++) {
        uint8_t mask = font_row(ch, gy);
        uint8_t *line = &framebuffer[y + gy][x];

        for (int gx = 0; gx < CHAR_W; gx++) {
            bool is_fg = (mask & (0x80 >> gx)) != 0;

            if (is_fg) {
                line[gx] = fg;
            } else if (!transparent) {
                line[gx] = bg;
            }
        }
    }
}

static void render_dirty_cells(void)
{
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
                dirty_rows[row][word] &= ~bit;
            }
        }
    }
}

/* ─────────────────────────── HSTX DVI constants ─────────────────────────── */

// TMDS control symbols (active-low hsync/vsync for 640×480)
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// 640×480 @ 60 Hz timing family (see note in main() re: pixel clock)
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    80   /* 48 + the 32 columns dropped from active */
#define MODE_H_ACTIVE_PIXELS FB_WIDTH

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH     2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_V_TOTAL_LINES ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

// HSTX command words
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

/* ────────────────────── HSTX command lists (per scanline) ───────────────── */

#define MODE_H_ACTIVE_HALF1 (MODE_H_ACTIVE_PIXELS / 2)
#define MODE_H_ACTIVE_HALF2 (MODE_H_ACTIVE_PIXELS - MODE_H_ACTIVE_HALF1)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_ACTIVE_HALF1,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_ACTIVE_HALF2,
    SYNC_V1_H1,
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_ACTIVE_HALF1,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_ACTIVE_HALF2,
    SYNC_V0_H1,
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | (MODE_H_SYNC_WIDTH / 2),
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_SYNC_WIDTH - MODE_H_SYNC_WIDTH / 2),
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

/* ───────────────────── ping-pong DMA (per-scanline) ─────────────────────── */

static int DMACH_PING;
static int DMACH_PONG;

static bool dma_pong = false;
static uint v_scanline = 2;  /* two transfers pre-cued (reference pattern) */
static bool vactive_cmdlist_posted = false;
static volatile bool vsync_flag;

#define VBLANK_END (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH)

/* RAM-resident (not __scratch_x — that bank holds Core 1's stack here). */
void __not_in_flash_func(dma_irq_handler)(void)
{
    /* Auto-chaining: reconfigure the channel that just FINISHED.
     * The other channel is already running. (Reference pattern.) */
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH &&
        v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < VBLANK_END) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        /* Pixel data: DMA reads directly from SRAM framebuffer */
        uint line = v_scanline - VBLANK_END;
        ch->read_addr = (uintptr_t)framebuffer[line];
        ch->transfer_count = FB_WIDTH / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        if (v_scanline == 0)
            vsync_flag = true;
    }
}

/* ─────────────────────────── HSTX + DMA init ────────────────────────────── */

static void hstx_dvi_init(void)
{
    // TMDS encoder: RGB332 pixel format.
    // Solder Party carrier lane mapping {6,4,0} routes:
    //   L0 → Green → extract G: rot 29, 2 bits
    //   L1 → Red   → extract R: rot 0,  2 bits
    //   L2 → Blue  → extract B: rot 26, 1 bit
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB;

    // Command expander: RGB332 = 4 pixels per 32-bit word (8-bit shifts)
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serialiser: 5-cycle clock, 5 shifts of 2 bits = 10-bit TMDS symbol.
    // Pixel clock = clk_sys / CLKDIV = 192 MHz / 5 = 38.4 MHz.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // ── HSTX output-pin mapping (per board) ───────────────────────────────────
    // GP12-19 are HSTX bit[0..7]; each adjacent pair drives one differential
    // lane. HSTX_CLK_BIT is the pair that outputs the pixel clock; the loop maps
    // each TMDS data lane (0/1/2) to its output-bit pair via lane_to_output_bit.
    // Clock and data can land on any of the four pairs, so per-board DVI routing
    // differences live entirely here.
    //   Signal ↔ TMDS lane: D1 = lane 0 (SEL 0/1), D2 = lane 1 (SEL 10/11),
    //                       D0 = lane 2 (SEL 20/21).
#if defined(SOLDERPARTY_RP2350_STAMP_XL_CARRIER)
    // Validated mapping (Pico DVI Sock style):
    //   GP12/13 = D0   GP14/15 = CK   GP16/17 = D2   GP18/19 = D1
    static const int HSTX_CLK_BIT = 2;
    static const int lane_to_output_bit[3] = {6, 4, 0};
#elif defined(PIMORONI_PGA2350)
    // Swap D0<->CK and D1<->D2 relative to the SolderParty routing:
    //   GP12/13 = CK   GP14/15 = D0   GP16/17 = D1   GP18/19 = D2
    static const int HSTX_CLK_BIT = 0;
    static const int lane_to_output_bit[3] = {4, 6, 2};
#else
    #error "No HSTX pin mapping for this PICO_BOARD (expected solderparty_rp2350_stamp_xl_carrier or pimoroni_pga2350)"
#endif

    hstx_ctrl_hw->bit[HSTX_CLK_BIT    ] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[HSTX_CLK_BIT + 1] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = lane_to_output_bit[lane];
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 0; i < GPIO_HSTX_COUNT; ++i)
        gpio_set_function(GPIO_HSTX_BASE + i, GPIO_FUNC_HSTX);

    // Claim DMA channels dynamically to avoid conflicts with the bus channels.
    DMACH_PING = dma_claim_unused_channel(true);
    DMACH_PONG = dma_claim_unused_channel(true);

    // Auto-chaining ping-pong DMA — each channel chains to the other.
    dma_channel_config c;

    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    dma_irqn_acknowledge_channel(VIDEO_DMA_IRQ_IDX, DMACH_PING);
    dma_irqn_acknowledge_channel(VIDEO_DMA_IRQ_IDX, DMACH_PONG);
    dma_irqn_set_channel_enabled(VIDEO_DMA_IRQ_IDX, DMACH_PING, true);
    dma_irqn_set_channel_enabled(VIDEO_DMA_IRQ_IDX, DMACH_PONG, true);
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_irq_handler);
    // Highest priority (0): the per-scanline DMA re-arm has a hard deadline, so
    // this must preempt the bus DMA IRQs (set to 0x40 in pico_thing_video.cpp).
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}

/* ───────────────────────── vsync presentation ───────────────────────────── */

void vidterm_present(void)
{
    while (!vsync_flag) tight_loop_contents();
    vsync_flag = false;
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

void vidterm_clear(uint8_t ch, uint8_t attr)
{
    memset(text_cells, ch,   sizeof(text_cells));
    memset(text_attrs, attr, sizeof(text_attrs));
    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int w = 0; w < DIRTY_WORDS; w++)
            dirty_rows[r][w] = 0xFFFFFFFF;
        /* Mask off unused bits in the last word */
        if (TEXT_COLS & 31)
            dirty_rows[r][DIRTY_WORDS - 1] = (1u << (TEXT_COLS & 31)) - 1;
    }
}

void vidterm_refresh(void)
{
    render_dirty_cells();
}

/* ───────────────────────── public API (graphics layer) ──────────────────── */

static inline void plot_unlocked(int x, int y, uint8_t colour)
{
    if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT)
        framebuffer[y][x] = colour;
}

void vidterm_plot(int x, int y, uint8_t colour)
{
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    framebuffer[y][x] = colour;
}

void vidterm_fill_rect(int x, int y, int w, int h, uint8_t colour)
{
    for (int row = y; row < y + h && row < FB_HEIGHT; row++) {
        if (row < 0) continue;
        int x0 = (x < 0) ? 0 : x;
        int x1 = (x + w > FB_WIDTH) ? FB_WIDTH : x + w;
        for (int i = x0; i < x1; i++) {
            framebuffer[row][i] = colour;
        }
    }
}

void vidterm_hline(int x, int y, int len, uint8_t colour)
{
    if (y < 0 || y >= FB_HEIGHT) return;
    int x0 = (x < 0) ? 0 : x;
    int x1 = (x + len > FB_WIDTH) ? FB_WIDTH : x + len;
    for (int i = x0; i < x1; i++) {
        framebuffer[y][i] = colour;
    }
}

void vidterm_vline(int x, int y, int len, uint8_t colour)
{
    if (x < 0 || x >= FB_WIDTH) return;
    int y0 = (y < 0) ? 0 : y;
    int y1 = (y + len > FB_HEIGHT) ? FB_HEIGHT : y + len;
    for (int i = y0; i < y1; i++) {
        framebuffer[i][x] = colour;
    }
}

void vidterm_clear_graphics(uint8_t colour)
{
    memset(framebuffer, colour, sizeof(framebuffer));
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

void video_init(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    hstx_dvi_init();
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

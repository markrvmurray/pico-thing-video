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
#include "video_render.h"        // the portable renderer owns the framebuffer

/* ───────────────────────── compile-time parameters ──────────────────────── */

/* Per-scanline video IRQ. Bus engine owns DMA_IRQ_0/1, so video uses IRQ 2. */
#define VIDEO_DMA_IRQ_IDX 2
#define VIDEO_DMA_IRQ     DMA_IRQ_2

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
        /* Pixel data: DMA reads directly from SRAM framebuffer.
         *
         * Nothing may be composited here. This handler has only the
         * duration of the in-flight transfer to re-arm the finished
         * channel, and copying even one scanline (let alone through a
         * flash-resident memcpy) overruns that budget and breaks the HSTX
         * stream outright — the display loses sync and blanks. Overlays
         * such as the cursor are drawn into the framebuffer from the
         * service loop instead. */
        uint line = v_scanline - VBLANK_END;
        ch->read_addr = (uintptr_t)video_framebuffer[line];
        ch->transfer_count = FB_WIDTH / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
        if (v_scanline == 0) {
            vsync_flag = true;
            video_frames = video_frames + 1;   /* blink timebase (volatile: not ++) */
        }
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

    video_set_dma_priority(true);

#if VIDEO_SCANOUT_OFF
    /* Measurement build: everything above is configured but never started, so
     * not one framebuffer word is fetched. The display stays dark -- which is
     * why this says so on the console rather than leaving a silent black
     * screen to be diagnosed as a fault. */
    printf("VIDEO: scan-out DMA NOT started (VIDEO_SCANOUT=OFF)."
           " Display is dark by construction; this is a measurement build.\n");
#else
    dma_channel_start(DMACH_PING);
#endif
}

/* ─────────────────────── SRAM crossbar priority ─────────────────────────── */

/* Who wins when a core and a DMA want the same SRAM bank in the same cycle.
 * Scan-out has had absolute priority since bring-up, on the reasoning that it
 * owes a hard per-scanline deadline -- but it consumes about 38 MB/s of a bus
 * good for well over a gigabyte, and the cost of that choice turns out to be
 * large: a core writing the framebuffer measures 10.5 cycles per BYTE, which
 * is arbitration, not instructions.
 *
 * Worth knowing before changing it: the deadline belongs to the scanline
 * INTERRUPT re-arming the finished channel, not to the DREQ-paced transfer,
 * which has a whole line of slack. That is the argument that this may be
 * safe. It is an argument, not a measurement, which is why this is a runtime
 * toggle: if the display suffers it can be put back from the console without
 * a reflash, and the console is on UART so it stays reachable either way. */
void video_set_dma_priority(bool high)
{
    bus_ctrl_hw->priority = high ?
        (BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS) : 0u;
}

bool video_get_dma_priority(void)
{
    return (bus_ctrl_hw->priority &
            (BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS)) != 0u;
}

/* ───────────────────────── vsync presentation ───────────────────────────── */

void vidterm_present(void)
{
    while (!vsync_flag) tight_loop_contents();
    vsync_flag = false;
}

/* ─────────────────────────────── bring-up ───────────────────────────────── */

void video_init(void)
{
    video_render_init();
    hstx_dvi_init();
}


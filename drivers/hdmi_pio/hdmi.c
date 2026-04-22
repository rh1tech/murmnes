/*
 * PIO-based HDMI driver for frank-nes
 * Adapted from murmsnes HDMI driver by Mikhail Matveev
 * SPDX-License-Identifier: MIT
 *
 * This driver uses PIO0 for HDMI signal generation and DMA for
 * palette-indexed framebuffer to TMDS conversion. It runs entirely
 * from DMA interrupts - no Core 1 needed for video.
 */

#include "hdmi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdalign.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"

/* Globals expected by the driver */
int graphics_buffer_width = 256;
int graphics_buffer_height = 240;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

/* Graphics buffer pointer */
static uint8_t *graphics_buffer = NULL;

/* CRT scanline effect: when active, every other display line is black */
static bool crt_active = false;

void graphics_set_scanlines(bool active) {
    crt_active = active;
}

void graphics_set_buffer(uint8_t *buffer) {
    graphics_buffer = buffer;
}

uint8_t* graphics_get_buffer(void) {
    return graphics_buffer;
}

uint32_t graphics_get_width(void) {
    return graphics_buffer_width;
}

uint32_t graphics_get_height(void) {
    return graphics_buffer_height;
}

void graphics_set_res(int w, int h) {
    graphics_buffer_width = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

uint8_t* get_line_buffer(int line) {
    if (!graphics_buffer) return NULL;
    if (line < 0 || line >= graphics_buffer_height) return NULL;
    return graphics_buffer + line * graphics_buffer_width;
}

static struct video_mode_t video_mode[] = {
    { /* 640x480 60Hz */
        .h_total = 524,
        .h_width = 480,
        .freq = 60,
        .vgaPxClk = 25175000
    }
};

struct video_mode_t graphics_get_video_mode(int mode) {
    (void)mode;
    return video_mode[0];
}

static int get_video_mode(void) {
    return 0;
}

void vsync_handler(void) {
    /* Optional: Add vsync callback if needed */
}

/* --- HDMI Driver Code --- */

/* PIO parameters */
static uint offs_prg0 = 0;
static uint offs_prg1 = 0;

/* SM */
static int SM_video = -1;
static int SM_conv = -1;

/* Palette buffer: 256 colors in R8G8B8 format */
static uint32_t palette[256];

/* Substitute map for HDMI reserved sync-control indices (BASE_HDMI_CTRL_INX..BASE_HDMI_CTRL_INX+3) */
static uint8_t hdmi_color_substitute[4] = {0, 0, 0, 0};

/* Assembly-optimized functions */
extern void hdmi_copy_scanline_asm(uint8_t* dst, const uint8_t* src, uint32_t count, const uint8_t* subst);
extern void hdmi_memset_fast(uint8_t* dst, uint8_t val, uint32_t count);
extern uint64_t get_ser_diff_data_asm(uint16_t dataR, uint16_t dataG, uint16_t dataB);

/* TMDS encoding lookup table - 256 entries (precomputed) */
static const uint16_t tmds_table[256] = {
    0x100, 0x1ff, 0x1fe, 0x101, 0x1fc, 0x103, 0x102, 0x1fd,
    0x1f8, 0x107, 0x106, 0x1f9, 0x104, 0x1fb, 0x1fa, 0x105,
    0x1f0, 0x10f, 0x10e, 0x1f1, 0x10c, 0x1f3, 0x1f2, 0x10d,
    0x108, 0x1f7, 0x1f6, 0x109, 0x1f4, 0x10b, 0x2a0, 0x25f,
    0x1e0, 0x11f, 0x11e, 0x1e1, 0x11c, 0x1e3, 0x1e2, 0x11d,
    0x118, 0x1e7, 0x1e6, 0x119, 0x1e4, 0x11b, 0x2b0, 0x24f,
    0x110, 0x1ef, 0x1ee, 0x111, 0x1ec, 0x113, 0x2b8, 0x247,
    0x1e8, 0x117, 0x2bc, 0x243, 0x2be, 0x241, 0x240, 0x2bf,
    0x1c0, 0x13f, 0x13e, 0x1c1, 0x13c, 0x1c3, 0x1c2, 0x13d,
    0x138, 0x1c7, 0x1c6, 0x139, 0x1c4, 0x13b, 0x290, 0x26f,
    0x130, 0x1cf, 0x1ce, 0x131, 0x1cc, 0x133, 0x298, 0x267,
    0x1c8, 0x137, 0x29c, 0x263, 0x29e, 0x261, 0x260, 0x29f,
    0x120, 0x1df, 0x1de, 0x121, 0x1dc, 0x123, 0x288, 0x277,
    0x1d8, 0x127, 0x28c, 0x273, 0x28e, 0x271, 0x270, 0x28f,
    0x1d0, 0x12f, 0x284, 0x27b, 0x286, 0x279, 0x278, 0x287,
    0x282, 0x27d, 0x27c, 0x283, 0x27e, 0x281, 0x280, 0x27f,
    0x180, 0x17f, 0x17e, 0x181, 0x17c, 0x183, 0x182, 0x17d,
    0x178, 0x187, 0x186, 0x179, 0x184, 0x17b, 0x2d0, 0x22f,
    0x170, 0x18f, 0x18e, 0x171, 0x18c, 0x173, 0x2d8, 0x227,
    0x188, 0x177, 0x2dc, 0x223, 0x2de, 0x221, 0x220, 0x2df,
    0x160, 0x19f, 0x19e, 0x161, 0x19c, 0x163, 0x2c8, 0x237,
    0x198, 0x167, 0x2cc, 0x233, 0x2ce, 0x231, 0x230, 0x2cf,
    0x190, 0x16f, 0x2c4, 0x23b, 0x2c6, 0x239, 0x238, 0x2c7,
    0x2c2, 0x23d, 0x23c, 0x2c3, 0x23e, 0x2c1, 0x2c0, 0x23f,
    0x140, 0x1bf, 0x1be, 0x141, 0x1bc, 0x143, 0x2e8, 0x217,
    0x1b8, 0x147, 0x2ec, 0x213, 0x2ee, 0x211, 0x210, 0x2ef,
    0x1b0, 0x14f, 0x2e4, 0x21b, 0x2e6, 0x219, 0x218, 0x2e7,
    0x2e2, 0x21d, 0x21c, 0x2e3, 0x21e, 0x2e1, 0x2e0, 0x21f,
    0x1a0, 0x15f, 0x2f4, 0x20b, 0x2f6, 0x209, 0x208, 0x2f7,
    0x2f2, 0x20d, 0x20c, 0x2f3, 0x20e, 0x2f1, 0x2f0, 0x20f,
    0x2fa, 0x205, 0x204, 0x2fb, 0x206, 0x2f9, 0x2f8, 0x207,
    0x202, 0x2fd, 0x2fc, 0x203, 0x2fe, 0x201, 0x200, 0x2ff,
};

/* Inline TMDS encoder using lookup table */
#define tmds_encode(d8) tmds_table[(uint8_t)(d8)]

/* Palette update flag - set by emulator, checked during vblank */
static volatile bool palette_dirty = false;
static volatile bool full_palette_update_pending = false;
static void apply_pending_palette(void);
void graphics_convert_all_palette(void);

/* HDMI sync control indices start at 251 */
#define BASE_HDMI_CTRL_INX (251)

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

/* DMA channels */
static int dma_chan_ctrl;
static int dma_chan;
static int dma_chan_pal_conv_ctrl;
static int dma_chan_pal_conv;

/* DMA buffers */
static uint32_t *dma_lines[2] = { NULL, NULL };
static uint32_t *DMA_BUF_ADDR[2];

/* DMA palette for conversion - dma_data allocated at the tail */
alignas(4096) uint32_t conv_color[1224];

/* Index for hang detection */
static uint32_t irq_inx = 0;

/* PIO programs */

static uint64_t get_ser_diff_data(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
        uint8_t bR = (dataR >> (9 - i)) & 1;
        uint8_t bG = (dataG >> (9 - i)) & 1;
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (HDMI_PIN_invert_diffpairs) {
            bR ^= 0b11;
            bG ^= 0b11;
            bB ^= 0b11;
        }
        uint8_t d6;
        if (HDMI_PIN_RGB_notBGR) {
            d6 = (bR << 4) | (bG << 2) | (bB << 0);
        }
        else {
            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        }

        out64 |= d6;
    }
    return out64;
}

static uint tmds_encoder(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }

    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;

    return d_out;
}

static void pio_set_x(PIO pio, const int sm, uint32_t v) {
    uint instr_shift = pio_encode_in(pio_x, 4);
    uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = (v >> (i * 4)) & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}

static inline uint32_t rgb_dist2(uint32_t a, uint32_t b) {
    int dr = (int)((a >> 16) & 0xff) - (int)((b >> 16) & 0xff);
    int dg = (int)((a >> 8) & 0xff) - (int)((b >> 8) & 0xff);
    int db = (int)(a & 0xff) - (int)(b & 0xff);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

static void hdmi_recompute_color_substitute(void) {
    const int base = BASE_HDMI_CTRL_INX;
    for (int i = 0; i < 4; i++) {
        const uint8_t reserved = (uint8_t)(base + i);
        const uint32_t target = palette[reserved] & 0x00ffffff;

        uint8_t best = 0;
        uint32_t best_d = 0xffffffffu;
        for (int j = 0; j < 256; j++) {
            if (j >= base && j <= base + 3) continue;
            const uint32_t cand = palette[j] & 0x00ffffff;
            const uint32_t d = rgb_dist2(target, cand);
            if (d < best_d) {
                best_d = d;
                best = (uint8_t)j;
                if (d == 0) break;
            }
        }

        hdmi_color_substitute[i] = best;
    }
}

uint16_t pio_program_instructions_conv_HDMI[] = {
    0x80a0, /*  0: pull   block */
    0x40e8, /*  1: in     osr, 8 */
    0x4034, /*  2: in     x, 20 */
    0x8020, /*  3: push   block */
};

const struct pio_program pio_program_conv_addr_HDMI = {
    .instructions = pio_program_instructions_conv_HDMI,
    .length = 4,
    .origin = -1,
};

static const uint16_t instructions_PIO_HDMI[] = {
    0x7006, /*  0: out    pins, 6         side 2 */
    0x7006, /*  1: out    pins, 6         side 2 */
    0x7006, /*  2: out    pins, 6         side 2 */
    0x7006, /*  3: out    pins, 6         side 2 */
    0x7006, /*  4: out    pins, 6         side 2 */
    0x6806, /*  5: out    pins, 6         side 1 */
    0x6806, /*  6: out    pins, 6         side 1 */
    0x6806, /*  7: out    pins, 6         side 1 */
    0x6806, /*  8: out    pins, 6         side 1 */
    0x6806, /*  9: out    pins, 6         side 1 */
};

static const struct pio_program program_PIO_HDMI = {
    .instructions = instructions_PIO_HDMI,
    .length = 10,
    .origin = -1,
};

/*
 * DMA ISR: generates HDMI scanlines from the 8-bit indexed framebuffer.
 * NES: 256x240, centered in 320x480 (doubled vertically).
 */
static void dma_handler_HDMI(void) {
    static uint32_t inx_buf_dma;
    static uint line = 0;
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[inx_buf_dma & 1], false);

    line = line >= 524 ? 0 : line + 1;

    /* CRT scanline effect */
    #define VMARGIN_SCANLINES 0
    #define CONTENT_SCANLINES (240*2)
    if ((line & 1) == 0) {
        if (crt_active) {
            int sl = (int)line - VMARGIN_SCANLINES;
            if (sl >= 0 && sl < CONTENT_SCANLINES) {
                inx_buf_dma++;
                uint8_t* activ_buf = (uint8_t *)dma_lines[inx_buf_dma & 1];
                hdmi_memset_fast(activ_buf + 72, 0, 320);
                hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX, 24);
                hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 1, 48);
                hdmi_memset_fast(activ_buf + 392, BASE_HDMI_CTRL_INX, 8);
            }
        }
        return;
    }
    inx_buf_dma++;

    uint8_t* activ_buf = (uint8_t *)dma_lines[inx_buf_dma & 1];

    /* NES: 240 lines doubled = 480 active lines, no vertical margin */
    if (line < (VMARGIN_SCANLINES + CONTENT_SCANLINES + VMARGIN_SCANLINES) ) {
        uint8_t* output_buffer = activ_buf + 72;

        /* Left margin: center 256 in 320 => 32 black pixels each side */
        hdmi_memset_fast(output_buffer, 0, 32);
        output_buffer += 32;

        int nes_scanline = (int)line - VMARGIN_SCANLINES;
        if (nes_scanline >= 0 && nes_scanline < CONTENT_SCANLINES && graphics_buffer) {
            /* Read from framebuffer - each NES line shown twice */
            const uint8_t* input = graphics_buffer + (nes_scanline / 2) * graphics_buffer_width;
            hdmi_copy_scanline_asm(output_buffer, input, graphics_buffer_width, hdmi_color_substitute);
        } else {
            hdmi_memset_fast(output_buffer, 0, graphics_buffer_width);
        }
        output_buffer += graphics_buffer_width;

        /* Right margin */
        hdmi_memset_fast(output_buffer, 0, 32);

        /* Sync pulses */
        hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX, 24);
        hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 1, 48);
        hdmi_memset_fast(activ_buf + 392, BASE_HDMI_CTRL_INX, 8);
    }
    else {
        /* VBlank area */
        if (line == (VMARGIN_SCANLINES + CONTENT_SCANLINES + VMARGIN_SCANLINES + 1)) {
            apply_pending_palette();
        }

        if ((line >= 490) && (line < 492)) {
            /* Vertical sync pulse */
            hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX + 2, 352);
            hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 3, 48);
        }
        else {
            /* VBlank without image */
            hdmi_memset_fast(activ_buf + 48, BASE_HDMI_CTRL_INX, 352);
            hdmi_memset_fast(activ_buf, BASE_HDMI_CTRL_INX + 1, 48);
        }
    }
}


static inline void irq_remove_handler_DMA_core1(void) {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void irq_set_exclusive_handler_DMA_core1(void) {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);  /* Highest priority for HDMI */
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

/* Deinitialization - reinitialization of resources */
static inline bool hdmi_init(void) {
    /* Disable DMA interrupt */
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl, false);
    }
    else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl, false);
    }

    irq_remove_handler_DMA_core1();

    /* Stop all DMA channels */
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | (1 << dma_chan_pal_conv) | (
                        1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();

#if HDMI_BASE_PIN >= 16
    pio_set_gpio_base(PIO_VIDEO, 16);
    pio_set_gpio_base(PIO_VIDEO_ADDR, 16);
#endif

    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);

    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI, offs_prg1);
    pio_remove_program(PIO_VIDEO, &program_PIO_HDMI, offs_prg0);

    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_PIO_HDMI);
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, ((uint32_t)conv_color >> 12));

    /* Initialize palette conversion (skip HDMI sync-control indices) */
    for (int ci = 0; ci < BASE_HDMI_CTRL_INX; ci++) graphics_set_palette_hdmi(ci, palette[ci]);
    for (int ci = BASE_HDMI_CTRL_INX + 4; ci < 256; ci++) {
        if (palette[ci] == 0) palette[ci] = 0x000000;
        graphics_set_palette_hdmi(ci, palette[ci]);
    }

    /* Sync control data */
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);

    /* Configure PIO SM for palette conversion */
    pio_sm_config c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_HDMI.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    /* Configure PIO SM for video output */
    c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_PIO_HDMI.length - 1));

    /* Side-set configuration */
    sm_config_set_sideset_pins(&c_c, beginHDMI_PIN_clk);
    sm_config_set_sideset(&c_c, 2, false, false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }

#if HDMI_BASE_PIN >= 16
    {
        uint64_t mask64 = (uint64_t)3u << beginHDMI_PIN_clk;
        pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
        pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    }
#else
    pio_sm_set_pins_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    pio_sm_set_pindirs_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
#endif

    /* Data pins */
    for (int i = 0; i < 6; i++) {
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, beginHDMI_PIN_data, 6, true);
    sm_config_set_out_pins(&c_c, beginHDMI_PIN_data, 6);

    sm_config_set_out_shift(&c_c, true, true, 30);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    int hdmi_hz = graphics_get_video_mode(get_video_mode()).freq;
    sm_config_set_clkdiv(&c_c, (clock_get_hz(clk_sys) / 252000000.0f) * (60 / hdmi_hz));
    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    /* DMA setup */
    dma_lines[0] = &conv_color[1024];
    dma_lines[1] = &conv_color[1124];

    /* Main working channel */
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl);

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv],
        &dma_lines[0][0],
        400,
        false
    );

    /* Control channel for main */
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan);

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];

    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr,
        &DMA_BUF_ADDR[0],
        1,
        false
    );

    /* Palette conversion channel */
    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl);

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video],
        &conv_color[0],
        4,
        false
    );

    /* Palette conversion control channel */
    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv);

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr,
        &PIO_VIDEO_ADDR->rxf[SM_conv],
        1,
        true /* start */
    );

    /* Start interrupt and channel */
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
        dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    }
    else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
        dma_channel_set_irq1_enabled(dma_chan_ctrl, true);
    }

    irq_set_exclusive_handler_DMA_core1();

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
}

void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) {
    color888 &= 0x00ffffff;
    palette[i] = color888;

    /* Don't write to hardware palette for HDMI control indices (251-254), but allow 255 (bgcolor) */
    if ((i >= BASE_HDMI_CTRL_INX) && (i != 255)) return;

    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    conv_color64[i * 2] = get_ser_diff_data(tmds_encode(R), tmds_encode(G), tmds_encode(B));
    conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
}

void graphics_convert_all_palette(void) {
    uint64_t* conv_color64 = (uint64_t *)conv_color;

    for (int i = 0; i < BASE_HDMI_CTRL_INX; i++) {
        uint32_t color888 = palette[i];
        const uint8_t R = (color888 >> 16) & 0xff;
        const uint8_t G = (color888 >> 8) & 0xff;
        const uint8_t B = (color888 >> 0) & 0xff;
        conv_color64[i * 2] = get_ser_diff_data(tmds_encode(R), tmds_encode(G), tmds_encode(B));
        conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
    }

    /* Also update color 255 (bgcolor) */
    uint32_t c255 = palette[255];
    const uint8_t R = (c255 >> 16) & 0xff;
    const uint8_t G = (c255 >> 8) & 0xff;
    const uint8_t B = (c255 >> 0) & 0xff;
    conv_color64[255 * 2] = get_ser_diff_data(tmds_encode(R), tmds_encode(G), tmds_encode(B));
    conv_color64[255 * 2 + 1] = conv_color64[255 * 2] ^ 0x0003ffffffffffffl;

    graphics_restore_sync_colors();
    hdmi_recompute_color_substitute();
}

static void apply_pending_palette(void) {
    if (!full_palette_update_pending) return;
    graphics_convert_all_palette();
    full_palette_update_pending = false;
}

void graphics_init_hdmi(void) {
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);

    hdmi_init();

    /* Initialize palette to all black */
    for (int i = 0; i < 256; i++) {
        palette[i] = 0;
    }
    graphics_convert_all_palette();
}

void graphics_set_bgcolor_hdmi(uint32_t color888) {
    graphics_set_palette_hdmi(255, color888);
}

void graphics_restore_sync_colors(void) {
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);
}

void graphics_set_mode(enum graphics_mode_t mode) {
    hdmi_graphics_mode = mode;
}

uint32_t graphics_get_palette(uint8_t i) {
    return palette[i];
}

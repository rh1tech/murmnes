/*
 * VGA output driver for frank-nes
 * Adapted from quakegeneric VGA driver (Mikhail Matveev)
 *
 * When SELECT_VGA is true, VGA PIO output is used.
 * When SELECT_VGA is false, all calls dispatch to the HDMI driver.
 * Only one can be active at a time (shared PIO0 and DMA_IRQ_0).
 *
 * SPDX-License-Identifier: MIT
 */

#include "hdmi.h"
#include "hardware/clocks.h"
#include "stdbool.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"

/* conv_color[] is defined in hdmi.c — VGA text mode reuses it for fast palette */
extern uint32_t conv_color[1224];

/* SELECT_VGA: set by testPins() in main before graphics_init() */
bool SELECT_VGA = false;

/* These are defined in hdmi.c — VGA uses them via extern */
extern int graphics_buffer_width;
extern int graphics_buffer_height;
extern int graphics_buffer_shift_x;
extern int graphics_buffer_shift_y;

/* get_line_buffer() and vsync_handler() are defined in hdmi.c */
extern uint8_t* get_line_buffer(int line);
extern void vsync_handler(void);

/* Forward declaration — VGA needs the video mode from hdmi.c */
extern struct video_mode_t graphics_get_video_mode(int mode);
static int get_video_mode(void) { return 0; }

/* VGA PIO program: simple 8-bit output */
uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};

/* VGA line pattern buffers and palette */
static uint32_t *lines_pattern[4];
static uint16_t pallette[256];
static uint32_t *lines_pattern_data = NULL;
static int _SM_VGA = -1;

static int line_VS_begin = 490;
static int line_VS_end = 491;
static int shift_picture = 0;

static int visible_line_size = 320;

static int dma_chan_ctrl_vga;
static int dma_chan_vga;

static bool is_flash_line = false;
static bool is_flash_frame = false;

static uint32_t bg_color[2];
static uint16_t palette16_mask = 0;

static uint text_buffer_width = 0;
static uint text_buffer_height = 0;

static uint16_t txt_palette[16];

/* Text palette fast lookup (reuses conv_color buffer) */
static uint16_t *txt_palette_fast = NULL;

static enum graphics_mode_t vga_graphics_mode = GRAPHICSMODE_DEFAULT;

static uint32_t frame_number = 0;
static uint32_t screen_line = 0;

void dma_handler_VGA(void) {
    dma_hw->ints0 = 1u << dma_chan_ctrl_vga;
    screen_line++;

    struct video_mode_t mode = graphics_get_video_mode(get_video_mode());

    if (screen_line == (uint32_t)mode.h_total) {
        vsync_handler();
        screen_line = 0;
        frame_number++;
    }

    if (screen_line >= (uint32_t)mode.h_width) {
        /* Fill background color */
        if (screen_line == (uint32_t)mode.h_width || screen_line == (uint32_t)mode.h_width + 3) {
            uint32_t *output_buffer_32bit = lines_pattern[2 + (screen_line & 1)];
            output_buffer_32bit += shift_picture / 4;
            uint32_t p_i = ((screen_line & is_flash_line) + (frame_number & is_flash_frame)) & 1;
            uint32_t color32 = bg_color[p_i];
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }

        /* Sync signals */
        if (screen_line >= (uint32_t)line_VS_begin && screen_line <= (uint32_t)line_VS_end)
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[1], false); //VS SYNC
        else
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
        return;
    }

    int y, line_number;

    uint32_t **output_buffer = &lines_pattern[2 + (screen_line & 1)];
    switch (vga_graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            line_number = screen_line / 2;
            if (screen_line % 2) return;
            y = screen_line / 2 - graphics_buffer_shift_y;
            break;
        default: {
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
            return;
        }
    }

    if (y < 0) {
        dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
        return;
    }
    if (y >= graphics_buffer_height) {
        /* Fill line with background color */
        if (y == graphics_buffer_height || y == graphics_buffer_height + 1 ||
            y == graphics_buffer_height + 2) {
            uint32_t *output_buffer_32bit = *output_buffer;
            uint32_t p_i = ((line_number & is_flash_line) + (frame_number & is_flash_frame)) & 1;
            uint32_t color32 = bg_color[p_i];

            output_buffer_32bit += shift_picture / 4;
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }
        dma_channel_set_read_addr(dma_chan_ctrl_vga, output_buffer, false);
        return;
    };

    /* Active image area */
    uint8_t *input_buffer_8bit = get_line_buffer(y);

    uint16_t *output_buffer_16bit = (uint16_t *)(*output_buffer);
    output_buffer_16bit += shift_picture / 2;

    graphics_buffer_shift_x &= 0xfffffff2; //2bit buf

    uint max_width = graphics_buffer_width;
    if (graphics_buffer_shift_x < 0) {
        input_buffer_8bit -= graphics_buffer_shift_x / 4; //2bit buf
        max_width += graphics_buffer_shift_x;
    }
    else {
#define div_factor (2)
        output_buffer_16bit += graphics_buffer_shift_x * 2 / div_factor;
    }

    int width = MIN((visible_line_size - ((graphics_buffer_shift_x > 0) ? (graphics_buffer_shift_x) : 0)), (int)max_width);
    if (width < 0) return;

    /* Palette index lookup */
    uint16_t *current_palette = pallette;

    switch (vga_graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            for (register int x = 0; x < width; ++x) {
                register uint8_t cx = input_buffer_8bit[x];
                *output_buffer_16bit++ = current_palette[cx];
            }
            break;
        default:
            break;
    }
    dma_channel_set_read_addr(dma_chan_ctrl_vga, output_buffer, false);
}

/* --- Dispatching functions: VGA or HDMI based on SELECT_VGA --- */

void graphics_set_mode_vga(enum graphics_mode_t mode) {
    if (!SELECT_VGA) {
        vga_graphics_mode = mode;
        return;
    }
    text_buffer_width = 80;
    text_buffer_height = 30;
    if (_SM_VGA < 0) return;

    vga_graphics_mode = mode;

    /* If already initialized, return */
    if (txt_palette_fast && lines_pattern_data) {
        return;
    };
    uint8_t TMPL_VHS8 = 0;
    uint8_t TMPL_VS8 = 0;
    uint8_t TMPL_HS8 = 0;
    uint8_t TMPL_LINE8 = 0;

    int line_size;
    double fdiv = 100;
    int HS_SIZE = 4;
    int HS_SHIFT = 100;

    switch (vga_graphics_mode) {
        case TEXTMODE_DEFAULT:
            /* Text palette */
            for (int i = 0; i < 16; i++) {
                txt_palette[i] = txt_palette[i] & 0x3f | palette16_mask >> 8;
            }

            if (!txt_palette_fast) {
                txt_palette_fast = (uint16_t *)conv_color;
                for (int i = 0; i < 256; i++) {
                    const uint8_t c1 = txt_palette[i & 0xf];
                    const uint8_t c0 = txt_palette[i >> 4];

                    txt_palette_fast[i * 4 + 0] = c0 | c0 << 8;
                    txt_palette_fast[i * 4 + 1] = c1 | c0 << 8;
                    txt_palette_fast[i * 4 + 2] = c0 | c1 << 8;
                    txt_palette_fast[i * 4 + 3] = c1 | c1 << 8;
                }
            }
            /* fall through */
        case GRAPHICSMODE_DEFAULT:
            TMPL_LINE8 = 0b11000000;
            HS_SHIFT = 328 * 2;
            HS_SIZE = 48 * 2;
            line_size = 400 * 2;
            shift_picture = line_size - HS_SHIFT;
            palette16_mask = 0xc0c0;
            visible_line_size = 320;
            line_VS_begin = 490;
            line_VS_end = 491;
            {
                struct video_mode_t vMode = graphics_get_video_mode(get_video_mode());
                fdiv = clock_get_hz(clk_sys) / (double)vMode.vgaPxClk;
            }
            break;
        default:
            return;
    }

    /* Adjust palette by sync bit mask */
    bg_color[0] = bg_color[0] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
    bg_color[1] = bg_color[1] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;

    /* Initialize line pattern templates and sync signal */
    if (!lines_pattern_data) {
        const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
        PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000;
        dma_channel_set_trans_count(dma_chan_vga, line_size / 4, false);

        lines_pattern_data = conv_color;

        for (int i = 0; i < 4; i++) {
            lines_pattern[i] = &lines_pattern_data[i * (line_size / 4)];
        }
        TMPL_VHS8 = TMPL_LINE8 ^ 0b11000000;
        TMPL_VS8 = TMPL_LINE8 ^ 0b10000000;
        TMPL_HS8 = TMPL_LINE8 ^ 0b01000000;

        uint8_t *base_ptr = (uint8_t *)lines_pattern[0];
        /* Empty line */
        memset(base_ptr, TMPL_LINE8, line_size);
        /* Hsync aligned at start */
        memset(base_ptr, TMPL_HS8, HS_SIZE);

        /* Vsync line */
        base_ptr = (uint8_t *)lines_pattern[1];
        memset(base_ptr, TMPL_VS8, line_size);
        memset(base_ptr, TMPL_VHS8, HS_SIZE);

        /* Image line templates */
        base_ptr = (uint8_t *)lines_pattern[2];
        memcpy(base_ptr, lines_pattern[0], line_size);
        base_ptr = (uint8_t *)lines_pattern[3];
        memcpy(base_ptr, lines_pattern[0], line_size);
    }
}

void graphics_set_offset(const int x, const int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    is_flash_frame = flash_frame;
    is_flash_line = flash_line;
}

void graphics_set_bgcolor(const uint32_t color888) {
    if (!SELECT_VGA) {
        graphics_set_bgcolor_hdmi(color888);
        return;
    }
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;
    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];
    bg_color[0] = ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask) << 16 |
                  ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask);
    bg_color[1] = ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask) << 16 |
                  ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask);
}

void graphics_set_palette(const uint8_t i, const uint32_t color888) {
    if (!SELECT_VGA) {
        graphics_set_palette_hdmi(i, color888);
        return;
    }
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;
    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];

    pallette[i] = (c_hi << 8 | c_lo) & 0x3f3f | palette16_mask;
}

void graphics_init(void) {
    if (!SELECT_VGA) {
        graphics_init_hdmi();
        return;
    }
    /* Initialize default palette */
    /* Text palette */
    for (int i = 0; i < 16; i++) {
        const uint8_t b = i & 1 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t r = i & 4 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t g = i & 2 ? (i >> 3 ? 3 : 2) : 0;

        const uint8_t c = r << 4 | g << 2 | b;

        txt_palette[i] = c & 0x3f | 0xc0;
    }
#if VGA_BASE_PIN >= 16
    pio_set_gpio_base(PIO_VGA, 16);
#endif
    /* Initialize PIO */
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    _SM_VGA = pio_claim_unused_sm(PIO_VGA, true);
    const uint sm = _SM_VGA;

    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    };

    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, 8, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + (pio_program_VGA.length - 1));

    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    pio_sm_init(PIO_VGA, sm, offset, &c);

    pio_sm_set_enabled(PIO_VGA, sm, true);

    /* Initialize DMA */
    dma_chan_ctrl_vga = dma_claim_unused_channel(true);
    dma_chan_vga = dma_claim_unused_channel(true);
    /* Main DMA channel */
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan_vga);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);

    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);

    uint dreq = DREQ_PIO1_TX0 + sm;
    if (PIO_VGA == pio0) dreq = DREQ_PIO0_TX0 + sm;

    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_chan_ctrl_vga);

    dma_channel_configure(
        dma_chan_vga,
        &c0,
        &PIO_VGA->txf[sm],     // Write address
        lines_pattern[0],       // Read address
        600 / 4,
        false                   // Don't start yet
    );
    /* DMA control channel */
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_ctrl_vga);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);

    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_chan_vga);

    dma_channel_configure(
        dma_chan_ctrl_vga,
        &c1,
        &dma_hw->ch[dma_chan_vga].read_addr, // Write address
        &lines_pattern[0],                    // Read address
        1,
        false                                 // Don't start yet
    );

    graphics_set_mode_vga(GRAPHICSMODE_DEFAULT);
    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_VGA);
    dma_channel_set_irq0_enabled(dma_chan_ctrl_vga, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan_vga);
}

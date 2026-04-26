/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#if defined(HDMI_PIO)
#include "hdmi.h"
#elif defined(VIDEO_COMPOSITE)
#include "graphics.h"
#elif defined(VGA_HSTX)
#include "pico_vga_hstx/video_output.h"
#else
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"
#endif

#include "pico/multicore.h"
#include "pico/stdlib.h"
#if !USB_HID_ENABLED
#include "pico/stdio_usb.h"
#endif

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "quicknes.h"
#include "psram_init.h"
#include "nespad.h"
#include "ps2kbd_wrapper.h"
#include "settings.h"
#include "rom_selector.h"
#include "i2s_audio.h"
#include "pwm_audio.h"
#include "uart_logging.h"
#include "ff.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/resets.h"
#include "hardware/xip_cache.h"

#if USB_HID_ENABLED
#include "usbhid.h"
#endif

/* 16KB stack in main SRAM — scratch_y (4KB) is too small for QuickNES */
static uint8_t big_stack[16384] __attribute__((aligned(8)));
static void real_main(void);

#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO)
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#endif

#define NES_WIDTH 256
#define NES_HEIGHT 240
#define SAMPLE_RATE 44100

#ifdef VIDEO_COMPOSITE
/* Framebuffer for composite TV: 8-bit indexed, 256x240 */
static uint8_t tv_framebuf[NES_WIDTH * NES_HEIGHT];
#endif

#ifdef HDMI_PIO
/* Framebuffer for PIO HDMI: 8-bit indexed, 256x240 */
static uint8_t soft_framebuf[NES_WIDTH * NES_HEIGHT];
#endif


/* ROM embedded in flash by CMake (objcopy) */
#ifdef HAS_NES_ROM
extern const uint8_t nes_rom_data[];
extern const uint8_t nes_rom_end[];
#endif

/* Palette lookup: NES indexed pixel -> doubled RGB565 (two pixels per word).
 * Pre-doubled eliminates shift+OR in the scanline callback hot loop,
 * reducing SRAM accesses and keeping the DMA ISR within timing budget. */
uint32_t rgb565_palette_32[2][256];
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
/* Darkened copy of the palette for the scanline effect — every second
 * output line reads from this instead of rgb565_palette_32, mimicking
 * the dark gap between CRT scanlines. Single-buffered: a torn read on
 * a dim line is far less visible than on a bright line, and the write
 * is a few hundred cycles inside vblank. Populated by update_palette.
 * Only compiled on HDMI HSTX — VGA_HSTX and the PIO/composite paths
 * render through different pipelines. */
uint32_t rgb565_palette_dim_32[256];
#endif
int pal_write_idx = 0;
static volatile int pal_read_idx = 0;
volatile int pending_pal_idx = -1;

/* Pointer to current frame pixels — only updated during vblank by vsync_cb */
static const uint8_t *frame_pixels;
static long frame_pitch;

/* Pending frame update: Core 0 writes here after emulation, Core 1 applies
   it during the next vsync callback (vblank). Ensures the pointer never
   changes while active scanlines are being displayed. */
volatile const uint8_t *pending_pixels;
volatile long pending_pitch;

/* Vsync flag — set by Core 1 DMA ISR, cleared by Core 0 after emulating */
volatile uint32_t vsync_flag;

#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO)
static void __not_in_flash("vsync") vsync_cb(void)
{
    /* Apply pending frame pointer during vblank — safe from tearing */
    const uint8_t *pp = (const uint8_t *)pending_pixels;
    if (pp) {
        frame_pixels = pp;
        frame_pitch = pending_pitch;
        pending_pixels = NULL;
        if (pending_pal_idx != -1) {
            pal_read_idx = pending_pal_idx;
            pending_pal_idx = -1;
        }
    }
    vsync_flag = 1;
    __sev(); /* wake Core 0 from WFE */
}
#endif

#ifdef VIDEO_COMPOSITE
/* Copy QuickNES frame (pitch=272) into contiguous TV framebuffer (width=256) */
static void tv_copy_frame(const uint8_t *src, long pitch) {
    if (pitch == NES_WIDTH)
        memcpy(tv_framebuf, src, NES_WIDTH * NES_HEIGHT);
    else
        for (int y = 0; y < NES_HEIGHT; y++)
            memcpy(&tv_framebuf[y * NES_WIDTH], src + y * pitch, NES_WIDTH);
}

/* Core 1 entry point for composite TV */
static void render_core_tv(void) {
    graphics_init();
    graphics_set_buffer(tv_framebuf, NES_WIDTH, NES_HEIGHT);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    graphics_set_offset(32, 0);
    graphics_set_palette(200, RGB888(0, 0, 0));
    while (1) tight_loop_contents();
}
#endif

#ifdef HDMI_PIO
/* Copy QuickNES frame (pitch=272) into contiguous PIO HDMI framebuffer (width=256) */
static void soft_copy_frame(const uint8_t *src, long pitch) {
    if (pitch == NES_WIDTH)
        memcpy(soft_framebuf, src, NES_WIDTH * NES_HEIGHT);
    else
        for (int y = 0; y < NES_HEIGHT; y++)
            memcpy(&soft_framebuf[y * NES_WIDTH], src + y * pitch, NES_WIDTH);
}
#endif

/*
 * Audio pipeline — same architecture as pico-infonesPlus:
 *   Core 0: encode NES audio → push pre-encoded packets to DI queue
 *   Core 1 ISR: pop from DI queue → HDMI output
 *   One shared queue. No intermediate buffers. No background task.
 *
 * All encoding functions are __not_in_flash (SRAM) so Core 0 can
 * encode without flash contention after running QuickNES from flash.
 * DI queue (512 entries = ~43ms) survives the ~10ms emulation gap.
 */
static int audio_frame_counter = 0;

#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
/* ─── HDMI audio: encode mono NES samples into Data Island packets ─── */
static int16_t audio_carry[3];
static int audio_carry_count = 0;

static void __not_in_flash("audio") hdmi_push_samples(const int16_t *buf, int count)
{
    int16_t merged[4];
    int pos = 0;

    if (audio_carry_count > 0) {
        for (int i = 0; i < audio_carry_count; i++)
            merged[i] = audio_carry[i];
        int need = 4 - audio_carry_count;
        if (need > count) need = count;
        for (int i = 0; i < need; i++)
            merged[audio_carry_count + i] = buf[i];
        pos = need;
        if (audio_carry_count + need == 4) {
            audio_sample_t samples[4];
            for (int i = 0; i < 4; i++) {
                samples[i].left = merged[i];
                samples[i].right = merged[i];
            }
            hstx_packet_t packet;
            int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
            hstx_data_island_t island;
            hstx_encode_data_island(&island, &packet, false, true);
            if (!hstx_di_queue_push(&island)) {
                return;
            }
            audio_frame_counter = new_fc;
        }
        audio_carry_count = 0;
    }

    while (pos + 4 <= count) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            samples[i].left = buf[pos + i];
            samples[i].right = buf[pos + i];
        }
        hstx_packet_t packet;
        int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island)) {
            break;
        }
        audio_frame_counter = new_fc;
        pos += 4;
    }

    audio_carry_count = count - pos;
    if (audio_carry_count > 3) {
        audio_carry_count = count % 4;
        pos = count - audio_carry_count;
    }
    for (int i = 0; i < audio_carry_count; i++)
        audio_carry[i] = buf[pos + i];
    hstx_di_queue_update_silence(audio_frame_counter);
}

static void hdmi_fill_silence(int count)
{
    for (int i = 0; i < count / 4; i++) {
        audio_sample_t samples[4] = {0};
        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}
#endif /* !VIDEO_COMPOSITE && !HDMI_PIO */

/* ─── Audio routing ───────────────────────────────────────────────── */
static bool i2s_initialized = false;
static bool pwm_audio_initialized = false;

static void ensure_i2s_initialized(void) {
#ifdef HAS_I2S
    if (!i2s_initialized) {
        i2s_audio_init(I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, SAMPLE_RATE);
        i2s_initialized = true;
    }
#endif
}

#include "hardware/pwm.h"

static void ensure_pwm_audio_initialized(void) {
    if (pwm_audio_initialized) return;
    pwm_audio_init(PWM_PIN0, PWM_PIN1, SAMPLE_RATE);
    pwm_audio_initialized = true;
}

/* Apply volume: scale 16-bit samples by g_settings.volume (0-100) */
static int16_t volume_buf[1024];

static const int16_t *apply_volume(const int16_t *buf, int count) {
    if (g_settings.volume >= 100) return buf;
    if (count > 1024) count = 1024;
    int vol = g_settings.volume;
    for (int i = 0; i < count; i++)
        volume_buf[i] = (int16_t)((buf[i] * vol) / 100);
    return volume_buf;
}

static void __not_in_flash("audio") audio_push_samples(const int16_t *buf, int count)
{
    if (g_settings.audio_mode == AUDIO_MODE_DISABLED) {
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
        hdmi_fill_silence(count);
#endif
        return;
    }
    const int16_t *out = (g_settings.volume < 100) ? apply_volume(buf, count) : buf;
    if (g_settings.audio_mode == AUDIO_MODE_PWM) {
        ensure_pwm_audio_initialized();
        pwm_audio_push_samples(out, count);
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
        hdmi_fill_silence(count);
#endif
        return;
    }
#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO) || defined(VGA_HSTX)
    ensure_i2s_initialized();
    i2s_audio_push_samples(out, count);
#else
    if (g_settings.audio_mode == AUDIO_MODE_I2S) {
        ensure_i2s_initialized();
        i2s_audio_push_samples(out, count);
        hdmi_fill_silence(count);
    } else {
        hdmi_push_samples(out, count);
    }
#endif
}

void audio_fill_silence(int count)
{
    if (g_settings.audio_mode == AUDIO_MODE_PWM) {
        ensure_pwm_audio_initialized();
        pwm_audio_fill_silence(count);
        return;
    }
#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO) || defined(VGA_HSTX)
    ensure_i2s_initialized();
    i2s_audio_fill_silence(count);
#else
    if (g_settings.audio_mode == AUDIO_MODE_I2S) {
        ensure_i2s_initialized();
        i2s_audio_fill_silence(count);
    }
    hdmi_fill_silence(count);
#endif
}

/* Post a frame for display and wait for it to be consumed */
void video_post_frame(const uint8_t *pixels, long pitch) {
#ifdef VIDEO_COMPOSITE
    if (pitch == NES_WIDTH)
        memcpy(tv_framebuf, pixels, NES_WIDTH * NES_HEIGHT);
    else
        for (int y = 0; y < NES_HEIGHT; y++)
            memcpy(&tv_framebuf[y * NES_WIDTH], pixels + y * pitch, NES_WIDTH);
#elif defined(HDMI_PIO)
    soft_copy_frame(pixels, pitch);
#elif defined(VGA_HSTX)
    vga_hstx_post_frame(pixels, pitch);
#else
    pending_pitch = pitch;
    pending_pixels = pixels;
#endif
}

void video_wait_vsync(void) {
#if USB_HID_ENABLED
    usbhid_task();
#endif
#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO)
    sleep_ms(1);
#elif defined(VGA_HSTX)
    /* Blit pending NES frame into DispHSTX framebuffer now. */
    vga_hstx_service();
#else
    while (pending_pixels != NULL) {
        __wfe();
    }
    vsync_flag = 0;
#endif
}

#include "palettes.h"

/* Resolve the NES palette index to an RGB tuple, honoring the currently
 * selected palette option. Base colors (0..63) come from the selected
 * palette; emphasis rows (64..511) fall back to the QuickNES table. */
static inline qnes_rgb_t resolve_nes_color(int idx, const qnes_rgb_t *qnes_colors) {
    if (idx < 0 || idx >= 512) idx = 0x0F;
    if (idx < 64) {
        const palette_rgb_t *p;
        switch (g_settings.palette) {
            case PALETTE_FIREBRANDX: p = palette_firebrandx; break;
            case PALETTE_WAVEBEAM:   p = palette_wavebeam;   break;
            case PALETTE_COMPOSITE:  p = palette_composite;  break;
            case PALETTE_NES:
            case PALETTE_CUSTOM:
            default:                 p = palette_nes_default; break;
        }
        qnes_rgb_t c;
        c.r = p[idx].r; c.g = p[idx].g; c.b = p[idx].b;
        return c;
    }
    return qnes_colors[idx];
}

/* Overscan crop in NES pixels (0/8/16). Read by the HSTX scanline
 * callback every line; updated from apply_runtime_settings(). Declared
 * volatile so Core 0 writes are visible to Core 1 without a lock. */
volatile uint8_t __not_in_flash("scanline_state") scanline_overscan_px = 8;

/* Translate the settings_t overscan enum to a pixel count. */
static inline uint8_t overscan_enum_to_px(uint8_t v) {
    switch (v) {
        case OVERSCAN_OFF: return 0;
        case OVERSCAN_16:  return 16;
        case OVERSCAN_8:
        default:           return 8;
    }
}

/* Translate the scanline enum to a palette multiplier in /256 units. */
static inline uint16_t scanlines_enum_to_mul(uint8_t v) {
    switch (v) {
        case SCANLINES_25: return 192; /* 75% brightness */
        case SCANLINES_50: return 128; /* 50% */
        case SCANLINES_75: return 64;  /* 25% */
        case SCANLINES_OFF:
        default:           return 256; /* no dim */
    }
}

/* Forward declare — defined inside the HDMI HSTX video-mode branch below.
 * VGA_HSTX uses DispHSTX's own rendering pipeline so the scanline dim
 * trick does not apply there. */
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
extern uint16_t scanline_dim_mul;
#endif

/* Scanlines-enabled flag read by the HSTX callback. OFF = skip dim
 * lookup and use only the normal palette for both output lines. */
volatile uint8_t __not_in_flash("scanline_state") scanline_effect_on = 0;

/* PAR enum read by the HSTX callback. */
volatile uint8_t __not_in_flash("scanline_state") scanline_par_mode = PAR_1_1;

/* Apply any Game Genie codes found in /nes/.cheats/{rom_name}.txt to
 * the currently loaded ROM. Called once after each successful ROM load.
 * One code per line; blank lines and lines starting with '#' or ';' are
 * ignored. Invalid codes log a message but do not abort. */
static void apply_cheat_file(void) {
    if (g_rom_name[0] == '\0') return;

    FATFS fs;
    if (f_mount(&fs, "", 1) != FR_OK) return;

    char path[128];
    snprintf(path, sizeof(path), "/nes/.cheats/%s.txt", g_rom_name);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        f_unmount("");
        return;
    }

    char line[32];
    int applied = 0;
    int failed = 0;
    while (f_gets(line, sizeof(line), &f)) {
        /* Strip trailing newline/whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '#' || *p == ';')
            continue;
        char *q = p;
        while (*q && *q != '\r' && *q != '\n' && *q != ' ' && *q != '\t') q++;
        *q = '\0';
        if (qnes_apply_game_genie(p) == 0) {
            applied++;
        } else {
            failed++;
            printf("cheat: rejected '%s'\n", p);
        }
    }

    f_close(&f);
    f_unmount("");
    if (applied || failed)
        printf("cheats: applied=%d rejected=%d from %s\n", applied, failed, path);
}

/* Push settings into the runtime state they affect. Called at startup
 * (after settings_load) and after every settings_menu_show() return. */
static void apply_runtime_settings(void) {
    scanline_overscan_px = overscan_enum_to_px(g_settings.overscan);
    scanline_effect_on = (g_settings.scanlines != SCANLINES_OFF);
    scanline_par_mode = g_settings.par;
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
    scanline_dim_mul = scanlines_enum_to_mul(g_settings.scanlines);
#endif
    /* Palette change: update_palette() reads g_settings.palette every
     * time it runs, so the next update_palette call (from ROM load or
     * menu return) picks up the selection. No per-frame work here. */
}

#ifdef VIDEO_COMPOSITE
/* Sync NES palette to composite TV driver */
void video_sync_palette(void) {
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();
    if (!pal || !colors) return;
    for (int i = 0; i < pal_size && i < 256; i++) {
        qnes_rgb_t c = resolve_nes_color(pal[i], colors);
        graphics_set_palette(i, ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
    }
    graphics_set_palette(200, 0x000000);
}

/* Sync rgb565_palette_32 entries to composite TV driver (for menu screens) */
void video_sync_palette_from_rgb565(int buf_idx) {
    for (int i = 0; i < 256; i++) {
        uint16_t c16 = (uint16_t)(rgb565_palette_32[buf_idx][i] & 0xFFFF);
        uint8_t r = ((c16 >> 11) & 0x1F) << 3;
        uint8_t g = ((c16 >> 5) & 0x3F) << 2;
        uint8_t b = (c16 & 0x1F) << 3;
        graphics_set_palette(i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
    graphics_set_palette(200, 0x000000);
}

static void update_palette(int buf_idx) {
    (void)buf_idx;
    video_sync_palette();
}
#elif defined(HDMI_PIO)
/* Sync NES palette to PIO HDMI or VGA driver (dispatches via SELECT_VGA) */
void video_sync_palette(void) {
    extern bool SELECT_VGA;
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();
    if (!pal || !colors) return;
    int limit = SELECT_VGA ? 256 : 251;
    for (int i = 0; i < pal_size && i < limit; i++) {
        qnes_rgb_t c = resolve_nes_color(pal[i], colors);
        graphics_set_palette(i, ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
    }
    if (!SELECT_VGA) graphics_restore_sync_colors();
}

/* Sync rgb565_palette_32 entries to PIO HDMI/VGA driver (for menu screens) */
void video_sync_palette_from_rgb565(int buf_idx) {
    extern bool SELECT_VGA;
    int limit = SELECT_VGA ? 256 : 251;
    for (int i = 0; i < limit; i++) {
        uint16_t c16 = (uint16_t)(rgb565_palette_32[buf_idx][i] & 0xFFFF);
        uint8_t r = ((c16 >> 11) & 0x1F) << 3;
        uint8_t g = ((c16 >> 5) & 0x3F) << 2;
        uint8_t b = (c16 & 0x1F) << 3;
        graphics_set_palette(i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
    if (!SELECT_VGA) graphics_restore_sync_colors();
}

static void update_palette(int buf_idx) {
    (void)buf_idx;
    video_sync_palette();
}
#else
#if !defined(VGA_HSTX)
/* Scanline dim factor in /256 units. 256 = no dim (OFF). Updated by
 * apply_runtime_settings from g_settings.scanlines. Not static so the
 * shared apply_runtime_settings() (above all video branches) can write
 * to it via the `extern` declaration. */
uint16_t scanline_dim_mul = 256;

/* Dim an RGB565 value by `mul/256`. Returns the packed 32-bit doubled form. */
static inline uint32_t dim_rgb565_doubled(uint16_t c, uint16_t mul) {
    unsigned r = (c >> 11) & 0x1F;
    unsigned g = (c >> 5)  & 0x3F;
    unsigned b =  c        & 0x1F;
    r = (r * mul) >> 8;
    g = (g * mul) >> 8;
    b = (b * mul) >> 8;
    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return ((uint32_t)out) | ((uint32_t)out << 16);
}
#endif /* !VGA_HSTX */

/* Build RGB565 palette from QuickNES frame palette + selected color table.
 * On HDMI HSTX also populates the companion dim palette for the
 * scanline effect so the scanline callback never does RGB math. */
static void update_palette(int buf_idx)
{
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();

    if (!pal || !colors)
        return;

#if !defined(VGA_HSTX)
    const uint16_t dim = scanline_dim_mul;
#endif

    for (int i = 0; i < pal_size && i < 256; i++) {
        qnes_rgb_t c = resolve_nes_color(pal[i], colors);
        uint16_t c16 = ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3);
        rgb565_palette_32[buf_idx][i] = c16 | ((uint32_t)c16 << 16);
#if !defined(VGA_HSTX)
        rgb565_palette_dim_32[i] = dim_rgb565_doubled(c16, dim);
#endif
    }
#if defined(VGA_HSTX)
    /* Also rebuild RGB332 palette for the DispHSTX framebuffer. */
    vga_hstx_update_palette(buf_idx, pal, pal_size, colors);
#endif
}

#if !defined(VGA_HSTX)
/* Output doublets (= 2 pixels) reserved for the NES image at 8:7 pixel
 * aspect. 256 * 8/7 ≈ 292.57 → round to 292 so ratio lands at ~8.045/7.
 * LUT below maps each output doublet to the corresponding source byte. */
#define PAR_87_DST_DOUBLETS 292

/* Nearest-neighbor LUT: par87_lut[i] = source column for output doublet i. */
static uint8_t par87_lut[PAR_87_DST_DOUBLETS];

static void build_par_luts(void) {
    for (int i = 0; i < PAR_87_DST_DOUBLETS; i++) {
        int src = (i * NES_WIDTH + PAR_87_DST_DOUBLETS / 2) / PAR_87_DST_DOUBLETS;
        if (src >= NES_WIDTH) src = NES_WIDTH - 1;
        par87_lut[i] = (uint8_t)src;
    }
}
#endif /* !VGA_HSTX */

#if !defined(VGA_HSTX)
/* Scanline callback: convert indexed pixels to RGB565, doubled to 640x480
 * Runs on Core 1 DMA ISR — must be in RAM, no flash access.
 * Not compiled on VGA_HSTX: DispHSTX uses its own framebuffer path. */
void __not_in_flash("scanline") scanline_callback(
    uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;
    int crop = (int)scanline_overscan_px;

    if ((int)nes_line < crop || (int)nes_line >= NES_HEIGHT - crop) {
        for (int i = 0; i < 320; i++)
            dst[i] = 0;
        return;
    }

    /* On every other output line, read from the dim palette so the
     * NES line shows as bright-dim-bright-dim. Gated by scanline_effect_on. */
    const uint32_t *pal;
    if (scanline_effect_on && (active_line & 1u)) {
        pal = rgb565_palette_dim_32;
    } else {
        pal = rgb565_palette_32[pal_read_idx];
    }

    const uint8_t *src = frame_pixels + nes_line * frame_pitch;

    if (scanline_par_mode == PAR_8_7) {
        /* 8:7 stretch: fill PAR_87_DST_DOUBLETS centered doublets,
         * pillarbox the rest. Overscan cols are applied against the
         * NES source, so the visible span is (NES_WIDTH - 2*crop).
         * Clip the destination range proportionally so the stretch
         * rate stays constant regardless of overscan. */
        int total_dst = PAR_87_DST_DOUBLETS;
        int dst_x0 = (320 - total_dst) / 2;
        int dst_x1 = dst_x0 + total_dst;

        /* Clear side pillars */
        for (int i = 0; i < dst_x0; i++) dst[i] = 0;
        for (int i = dst_x1; i < 320; i++) dst[i] = 0;

        for (int i = 0; i < total_dst; i++) {
            int sx = par87_lut[i];
            if (sx < crop || sx >= NES_WIDTH - crop) {
                dst[dst_x0 + i] = 0;
            } else {
                dst[dst_x0 + i] = pal[src[sx]];
            }
        }
    } else {
        /* 1:1 — straight 2x pixel replication, centered in the 320-wide output. */
        for (int i = 0; i < 32; i++) dst[i] = 0;
        for (int i = 288; i < 320; i++) dst[i] = 0;

        uint32_t *out = dst + 32;
        for (int i = 0; i < crop; i++) out[i] = 0;
        out += crop;
        for (int x = crop; x < NES_WIDTH - crop; x += 4) {
            uint32_t p = *(const uint32_t *)(src + x);
            out[0] = pal[p & 0xFF];
            out[1] = pal[(p >> 8) & 0xFF];
            out[2] = pal[(p >> 16) & 0xFF];
            out[3] = pal[(p >> 24)];
            out += 4;
        }
        for (int i = 0; i < crop; i++) out[i] = 0;
    }
}
#endif /* !VGA_HSTX */
#endif

/* Pixel buffer — used by settings menu for rendering */
uint8_t test_pixels[NES_WIDTH * NES_HEIGHT];

/* Try to initialize PSRAM, returns true on success */
static bool psram_available = false;
#define PSRAM_BASE ((volatile uint8_t *)0x11000000)

static bool try_init_psram(void)
{
    /* Try RP2350B pin first, then RP2350A */
    static const uint cs_pins[] = { PSRAM_CS_PIN_RP2350B, PSRAM_CS_PIN_RP2350A };
    for (int i = 0; i < 2; i++) {
        psram_init(cs_pins[i]);
        /* Write/read test — flush and invalidate cache so the read
         * actually goes to PSRAM instead of returning the cached write. */
        PSRAM_BASE[0] = 0xA5;
        PSRAM_BASE[1] = 0x5A;
        __compiler_memory_barrier();
        xip_cache_clean_all();
        xip_cache_invalidate_all();
        __compiler_memory_barrier();
        if (PSRAM_BASE[0] == 0xA5 && PSRAM_BASE[1] == 0x5A) {
            printf("PSRAM detected on CS pin %u\n", cs_pins[i]);
            psram_available = true;
            return true;
        }
    }
    printf("No PSRAM detected\n");
    return false;
}

/* Load first .nes ROM from SD card "nes" directory.
 * Returns ROM data pointer and sets *out_size. NULL on failure.
 * If PSRAM is available, ROM is loaded there; otherwise uses malloc.
 * Caller may free the buffer after qnes_load_rom() since QuickNES copies it. */
static uint8_t *sd_rom_buf = NULL;

/* Current ROM filename (without path/extension) for save state naming */
char g_rom_name[64] = {0};

static uint8_t *try_load_rom_from_sd(long *out_size)
{
    *out_size = 0;

    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD mount failed (%d)\n", fr);
        return NULL;
    }
    printf("SD card mounted\n");

    DIR dir;
    fr = f_opendir(&dir, "/nes");
    if (fr != FR_OK) {
        printf("SD: /nes directory not found (%d)\n", fr);
        f_unmount("");
        return NULL;
    }

    /* Find first .nes file */
    char filepath[280];
    FILINFO fno;
    bool found = false;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR)
            continue;
        size_t len = strlen(fno.fname);
        if (len >= 4 &&
            (fno.fname[len-4] == '.') &&
            (fno.fname[len-3] == 'n' || fno.fname[len-3] == 'N') &&
            (fno.fname[len-2] == 'e' || fno.fname[len-2] == 'E') &&
            (fno.fname[len-1] == 's' || fno.fname[len-1] == 'S')) {
            snprintf(filepath, sizeof(filepath), "/nes/%s", fno.fname);
            /* Store ROM name (without extension) for save state naming */
            {
                size_t nlen = len >= 4 ? len - 4 : len;
                if (nlen >= sizeof(g_rom_name)) nlen = sizeof(g_rom_name) - 1;
                memcpy(g_rom_name, fno.fname, nlen);
                g_rom_name[nlen] = '\0';
            }
            found = true;
            printf("SD: found ROM: %s (%lu bytes)\n", filepath, (unsigned long)fno.fsize);
            break;
        }
    }
    f_closedir(&dir);

    if (!found) {
        printf("SD: no .nes files in /nes\n");
        f_unmount("");
        return NULL;
    }

    FIL fil;
    fr = f_open(&fil, filepath, FA_READ);
    if (fr != FR_OK) {
        printf("SD: failed to open %s (%d)\n", filepath, fr);
        f_unmount("");
        return NULL;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize < 16 || fsize > 2 * 1024 * 1024) {
        printf("SD: invalid ROM size %lu\n", (unsigned long)fsize);
        f_close(&fil);
        f_unmount("");
        return NULL;
    }

    /* Allocate buffer: prefer PSRAM, fall back to SRAM malloc */
    if (psram_available) {
        sd_rom_buf = (uint8_t *)PSRAM_BASE;
        printf("SD: loading ROM into PSRAM\n");
    } else {
        sd_rom_buf = (uint8_t *)malloc(fsize);
        if (!sd_rom_buf) {
            printf("SD: malloc failed for %lu bytes\n", (unsigned long)fsize);
            f_close(&fil);
            f_unmount("");
            return NULL;
        }
        printf("SD: loading ROM into SRAM\n");
    }

    UINT bytes_read;
    fr = f_read(&fil, sd_rom_buf, (UINT)fsize, &bytes_read);
    f_close(&fil);
    f_unmount("");

    if (fr != FR_OK || bytes_read != (UINT)fsize) {
        printf("SD: read error (%d, got %u/%lu)\n", fr, bytes_read, (unsigned long)fsize);
        if (!psram_available)
            free(sd_rom_buf);
        sd_rom_buf = NULL;
        return NULL;
    }

    printf("SD: loaded %lu bytes\n", (unsigned long)fsize);
    *out_size = (long)fsize;
    return sd_rom_buf;
}

/* Stack watermark: paint stack with 0xDEADBEEF, later check how much was used */
static void paint_stack(void)
{
    volatile uint32_t sp;
    __asm volatile ("MOV %0, SP" : "=r" (sp));
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t *end = (uint32_t *)(sp - 256);
    while (p < end)
        *p++ = 0xDEADBEEF;
}

static uint32_t check_stack_free(void)
{
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t count = 0;
    while (*p == 0xDEADBEEF) {
        p++;
        count += 4;
    }
    return count;
}

/* HardFault handler — store fault info, pump USB to flush, blink LED */
static volatile uint32_t fault_pc, fault_lr, fault_cfsr, fault_mmfar, fault_bfar;
static volatile bool fault_occurred = false;

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile (
        "MRS r0, MSP\n"
        "B hardfault_handler_c\n"
    );
}

/* Stash fault info in watchdog scratch regs (survive warm reset) so
 * the next boot can report it over USB-CDC, which is far more reliable
 * than trying to print from inside the fault handler itself. */
#define FAULT_MAGIC 0xFA0171ED  /* "FAULTED" */

void __attribute__((used)) hardfault_handler_c(uint32_t *stack)
{
    fault_pc    = stack[6];
    fault_lr    = stack[5];
    fault_cfsr  = *(volatile uint32_t *)0xE000ED28;
    fault_mmfar = *(volatile uint32_t *)0xE000ED34;
    fault_bfar  = *(volatile uint32_t *)0xE000ED38;
    fault_occurred = true;

    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_hw->scratch[1] = fault_pc;
    watchdog_hw->scratch[2] = fault_lr;
    watchdog_hw->scratch[3] = fault_cfsr;
    watchdog_hw->scratch[4] = fault_mmfar;
    watchdog_hw->scratch[5] = fault_bfar;
    watchdog_hw->scratch[6] = (uint32_t)stack;  /* faulting SP */
    watchdog_hw->scratch[7] = stack[7];         /* xPSR */

    /* Arm a short watchdog and spin — warm-reset, scratch regs survive. */
    watchdog_reboot(0, 0, 100);
    while (1) tight_loop_contents();
}

/* Called from real_main() right after stdio is up. If a fault occurred in
 * the previous run, dump the captured info so we can see PC/LR over CDC. */
static void report_previous_fault(void)
{
    if (watchdog_hw->scratch[0] != FAULT_MAGIC) return;

    uint32_t pc    = watchdog_hw->scratch[1];
    uint32_t lr    = watchdog_hw->scratch[2];
    uint32_t cfsr  = watchdog_hw->scratch[3];
    uint32_t mmfar = watchdog_hw->scratch[4];
    uint32_t bfar  = watchdog_hw->scratch[5];
    uint32_t sp    = watchdog_hw->scratch[6];
    uint32_t xpsr  = watchdog_hw->scratch[7];

    /* Clear so we only report once per real fault. */
    watchdog_hw->scratch[0] = 0;

    for (int i = 0; i < 30; i++) {
        printf("!PREV_FAULT! PC=%08lx LR=%08lx CFSR=%08lx MMFAR=%08lx BFAR=%08lx SP=%08lx xPSR=%08lx\n",
               (unsigned long)pc, (unsigned long)lr, (unsigned long)cfsr,
               (unsigned long)mmfar, (unsigned long)bfar,
               (unsigned long)sp, (unsigned long)xpsr);
        sleep_ms(200);
    }
}

int main(void)
{
    /* Switch to large stack before doing anything else */
    __asm volatile ("MSR MSP, %0" :: "r" (big_stack + sizeof(big_stack)));
    real_main();
    __builtin_unreachable();
}

/* Configure flash timing for overclocked CPU.
 * Must run from SRAM — flash is being reconfigured. */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz)
{
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000)
        divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000)
        rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000 |
                           rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                           divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

/* Convert nespad button state to QuickNES joypad bitmask.
 * QuickNES: A=0, B=1, Select=2, Start=3, Up=4, Down=5, Left=6, Right=7 */
static int nespad_to_qnes(uint32_t pad)
{
    int joy = 0;
    if (pad & DPAD_A)      joy |= 0x01;
    if (pad & DPAD_B)      joy |= 0x02;
    if (pad & DPAD_SELECT) joy |= 0x04;
    if (pad & DPAD_START)  joy |= 0x08;
    if (pad & DPAD_UP)     joy |= 0x10;
    if (pad & DPAD_DOWN)   joy |= 0x20;
    if (pad & DPAD_LEFT)   joy |= 0x40;
    if (pad & DPAD_RIGHT)  joy |= 0x80;
    return joy;
}

/* Convert PS/2 keyboard state to QuickNES joypad bitmask */
static int ps2kbd_to_qnes(uint16_t kbd)
{
    int joy = 0;
    if (kbd & KBD_STATE_A)      joy |= 0x01;
    if (kbd & KBD_STATE_B)      joy |= 0x02;
    if (kbd & KBD_STATE_SELECT) joy |= 0x04;
    if (kbd & KBD_STATE_START)  joy |= 0x08;
    if (kbd & KBD_STATE_UP)     joy |= 0x10;
    if (kbd & KBD_STATE_DOWN)   joy |= 0x20;
    if (kbd & KBD_STATE_LEFT)   joy |= 0x40;
    if (kbd & KBD_STATE_RIGHT)  joy |= 0x80;
    return joy;
}

static void real_main(void)
{
#if defined(VGA_HSTX)
    /* Leave SDK boot-default clock (150 MHz). DispHSTX reconfigures as
     * needed. */
#elif !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO)
    /* HSTX: 252 MHz, HSTX clock = 252 / 2 = 126 MHz */
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    set_flash_timings(252, 88);
    sleep_ms(10);
    set_sys_clock_khz(252000, true);
#else
    /* PIO HDMI / composite TV: 378 MHz */
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    set_flash_timings(378, 88);
    sleep_ms(10);
    set_sys_clock_khz(378000, true);
#endif

    stdio_init_all();
    uart_logging_init();
    uart_logging_register();

    /* Dump previous-run fault info over CDC before any other init, so even
     * early boot crashes are debuggable. Survives via watchdog scratch regs. */
    report_previous_fault();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

#if defined(VGA_HSTX)
    /* Bring up DispHSTX before any other peripheral — it owns Core 1
     * and configures sys_clock itself. */
    vga_hstx_start();
#endif

    paint_stack();

    printf("\n=== frank-nes (QuickNES) ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    /* Init PSRAM early — operator new/realloc redirect to PSRAM */
    try_init_psram();

    printf("qnes_init...\n");
    if (qnes_init(SAMPLE_RATE) != 0) {
        printf("qnes_init FAILED\n");
        while (1) sleep_ms(100);
    }
    printf("qnes_init OK\n");

    /* Provide PSRAM for tile cache (large CHR ROMs need ~256KB+) */
    if (psram_available) {
        void *tc = (void *)(0x15000000 + 2 * 1024 * 1024);
        qnes_set_tile_cache_buf(tc, 1 * 1024 * 1024);
    }

    settings_load();
    apply_runtime_settings();
#if !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO) && !defined(VGA_HSTX)
    build_par_luts();
#endif

    /* Phase 1: scan SD directory and load CRC cache (fast, no HDMI needed) */
    int num_roms = 0;
    long preloaded_rom_size = 0;
    if (psram_available) {
        num_roms = rom_selector_preload_scan(&preloaded_rom_size);
        xip_cache_invalidate_all();
    }

    bool rom_loaded = false;
    xip_cache_invalidate_all();

#ifdef VIDEO_COMPOSITE
    /* Start composite TV output — Core 1 runs the TV driver */
    memset(tv_framebuf, 0, sizeof(tv_framebuf));
    multicore_launch_core1(render_core_tv);
    sleep_ms(200);
    audio_fill_silence(SAMPLE_RATE / 60 * 6);
#elif defined(HDMI_PIO)
    /* Autodetect VGA vs HDMI by probing HDMI connector pins */
    {
        extern bool SELECT_VGA;
#ifdef HAS_TV
        uint8_t link = testPins(HDMI_BASE_PIN, HDMI_BASE_PIN + 1);
        SELECT_VGA = (link == 0) || (link == 0x1F);
#else
        SELECT_VGA = false;
#endif
        printf("HDMI_PIO: SELECT_VGA=%d\n", SELECT_VGA);
    }
    /* Start PIO HDMI/VGA output — runs on Core 0 via DMA ISR, no Core 1 needed */
    memset(soft_framebuf, 0, sizeof(soft_framebuf));
    graphics_buffer_width = NES_WIDTH;
    graphics_buffer_height = NES_HEIGHT;
    graphics_set_buffer(soft_framebuf);
    graphics_init();  /* dispatches to VGA or HDMI based on SELECT_VGA */
    graphics_set_offset(32, 0);  /* center 256px NES in 320px display */
    {
        extern bool SELECT_VGA;
        /* Set initial palette to black */
        int limit = SELECT_VGA ? 256 : 251;
        for (int i = 0; i < limit; i++) graphics_set_palette(i, 0);
        if (!SELECT_VGA) {
            graphics_set_palette_hdmi(255, 0);
            graphics_restore_sync_colors();
        }
    }
    sleep_ms(200);
    audio_fill_silence(SAMPLE_RATE / 60 * 6);
#else
    /* Start HSTX HDMI or HSTX VGA (M2 only) */
    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;

#if !defined(VGA_HSTX)
    hstx_di_queue_init();
#endif
    audio_fill_silence(SAMPLE_RATE / 60 * 6); /* pre-fill ~100ms */
    video_output_set_vsync_callback(vsync_cb);
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

#if !defined(VGA_HSTX)
    /* HDMI: derive clk_hstx from pll_sys (252 MHz / 2 = 126 MHz).
     * VGA: driver configures clk_hstx = clk_sys = 252 MHz directly. */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys), 126000000);
#endif

    pico_hdmi_set_audio_sample_rate(SAMPLE_RATE);
#if !defined(VGA_HSTX)
    video_output_set_scanline_callback(scanline_callback);
#endif
#if defined(VGA_HSTX)
    /* DispHSTX already kicked Core 1 from video_output_init() running on
     * Core 0. Launching Core 1 again here would race/fail silently. */
    (void)video_output_core1_run;
#else
    multicore_launch_core1(video_output_core1_run);
#endif
    sleep_ms(100);
#endif

    /* Phase 2: CRC computation + metadata loading with progress display */
    if (psram_available && num_roms > 0) {
        rom_selector_preload_init_display();
        rom_selector_preload_index();
    }

    /* Init NES gamepad PIO driver */
    nespad_begin(clock_get_hz(clk_sys) / 1000,
                 NESPAD_CLK_PIN, NESPAD_DATA_PIN, NESPAD_LATCH_PIN);

    /* Init PS/2 keyboard */
    ps2kbd_init();
    printf("PS/2 keyboard initialized (CLK=%d, DATA=%d)\n", PS2_PIN_CLK, PS2_PIN_DATA);

#if USB_HID_ENABLED
    usbhid_init();
    printf("USB HID Host initialized\n");
#endif

    /* Show welcome screen */
    welcome_screen_show();

    /* Check SD card availability */
    {
        bool sd_ok = false;
        if (psram_available) {
            sd_ok = rom_selector_sd_ok();
        } else {
            /* Quick SD card check for non-PSRAM path */
            FATFS tmp_fs;
            if (f_mount(&tmp_fs, "", 1) == FR_OK) {
                sd_ok = true;
                f_unmount("");
            }
        }
        if (!sd_ok) {
            sd_error_show();
        }
    }

    /* If the SD is present but the ROM library is empty (missing /nes or
     * no .nes files), surface the reason to the user before opening the
     * file browser. The carousel is disabled because there is nothing to
     * show in it. Only shown once per boot. */
    if (psram_available && num_roms == 0) {
        rom_scan_result_t sr = rom_selector_scan_result();
        if (sr == ROM_SCAN_NO_NES_DIR || sr == ROM_SCAN_NO_ROMS) {
            rom_selector_no_roms_notice();
            g_settings.selector_mode = SELECTOR_MODE_BROWSER;
        }
    }

    while (1) {  /* outer loop: ROM selector → emulation → reset → ROM selector */

    /* Show ROM selector */
    rom_loaded = false;

    /* Apply emulation region before loading any ROM */
    qnes_set_region(g_settings.emu_mode == EMULATION_MODE_DENDY
                    ? QNES_REGION_DENDY : QNES_REGION_NTSC);

    /* Apply per-scanline sprite limit (ON = 8/scanline hardware behavior, OFF = render all) */
    qnes_set_sprite_limit(g_settings.sprite_limit ? 1 : 0);

    /* Apply audio equalizer preset */
    qnes_set_audio_eq(g_settings.audio_eq);

    /* Apply per-game debug/practice toggles */
    qnes_set_bg_disabled(g_settings.bg_disabled);
    qnes_set_channel_mute_mask(g_settings.chan_mute_mask);

    /* The selector is also used as a file browser when there are no ROMs
     * in /nes — it will skip the carousel and show the browser directly. */
    bool selector_available = psram_available &&
        (num_roms > 0 || rom_selector_scan_result() == ROM_SCAN_NO_ROMS
                      || rom_selector_scan_result() == ROM_SCAN_NO_NES_DIR);

    if (selector_available) {
        long rom_size = 0;
        if (rom_selector_show(&rom_size)) {
            sd_rom_buf = (uint8_t *)rom_selector_get_rom_data();
            printf("Initializing emulator (%ld bytes)...\n", rom_size);
            if (qnes_load_rom_inplace(sd_rom_buf, rom_size) == 0) {
                printf("Emulator initialized OK\n");
                rom_loaded = true;
                apply_cheat_file();
            }
        }
    }

    /* Fallback: try loading first .nes from SD — only when selector was
     * not available (no PSRAM).  If the selector was shown but the chosen
     * ROM failed to load, loop back instead of silently loading a different
     * game (which also risks OOM). */
    if (!rom_loaded && !selector_available) {
        long sd_rom_size = 0;
        uint8_t *sd_rom = try_load_rom_from_sd(&sd_rom_size);
        if (sd_rom) {
            if (qnes_load_rom_inplace(sd_rom, sd_rom_size) == 0) {
                printf("SD ROM loaded (fallback)\n");
                rom_loaded = true;
                apply_cheat_file();
            }
            if (!rom_loaded && !psram_available && sd_rom_buf)
                free(sd_rom_buf);
        }
    }

#ifdef HAS_NES_ROM
    if (!rom_loaded) {
        long rom_size = (long)(nes_rom_end - nes_rom_data);
        if (qnes_load_rom_inplace(nes_rom_data, rom_size) == 0) {
            printf("Flash ROM loaded\n");
            rom_loaded = true;
            if (g_rom_name[0] == '\0')
                snprintf(g_rom_name, sizeof(g_rom_name), "flash_rom");
            apply_cheat_file();
        }
    }
#endif

    if (rom_loaded) {
        bool reset_requested = false;
        while (1) {
#if defined(VGA_HSTX)
            /* DispHSTX VGA runs its own framebuffer scanout on Core 1.
             * Blit the previously-posted frame here so the next emulation
             * iteration can deliver a new one. */
            vga_hstx_service();
#elif !defined(VIDEO_COMPOSITE) && !defined(HDMI_PIO)
            /* Wait for vsync — Core 1 applies pending frame during vblank. */
            while (pending_pixels != NULL) {
#if USB_HID_ENABLED
                usbhid_task();
#endif
                __wfe();
            }
            vsync_flag = 0;
#endif

#if USB_HID_ENABLED
            usbhid_task();
#endif

            /* Fresh gamepad read right at vsync — input from NOW, not
             * from the previous frame. ~100µs cost is negligible. */
            nespad_read();
            ps2kbd_tick();
            
            uint16_t kbd_state = ps2kbd_get_state();

#if USB_HID_ENABLED
            kbd_state |= usbhid_get_kbd_state();
#endif

            /* Check for menu hotkey (Start+Select, F12) */
            if (settings_check_hotkey()) {
                settings_result_t result = settings_menu_show(test_pixels);
                if (result == SETTINGS_RESULT_RESET) {
                    qnes_close();
                    g_rom_name[0] = '\0';
                    reset_requested = true;
                    break;
                }
                /* Push menu-driven settings into runtime state, then
                 * restore the game palette (which honors g_settings.palette). */
                apply_runtime_settings();
                update_palette(pal_write_idx);
                pending_pal_idx = pal_write_idx;
                pal_write_idx ^= 1;
                continue;
            }

            /* Build per-source joypad values */
            int nespad1_joy = nespad_to_qnes(nespad_state);
            int nespad2_joy = nespad_to_qnes(nespad_state2);
            int kbd_joy = ps2kbd_to_qnes(kbd_state);
            int usb1_joy = 0, usb2_joy = 0;

#if USB_HID_ENABLED
            for (int ui = 0; ui < 2; ui++) {
                if (!usbhid_gamepad_connected_idx(ui)) continue;
                usbhid_gamepad_state_t gp;
                usbhid_get_gamepad_state_idx(ui, &gp);

                int uj = 0;
                if (gp.dpad & 0x01) uj |= 0x10; // Up
                if (gp.dpad & 0x02) uj |= 0x20; // Down
                if (gp.dpad & 0x04) uj |= 0x40; // Left
                if (gp.dpad & 0x08) uj |= 0x80; // Right
                if (gp.buttons & 0x01) uj |= 0x01; // A -> NES A
                if (gp.buttons & 0x02) uj |= 0x02; // B -> NES B
                if (gp.buttons & 0x04) uj |= 0x01; // C -> NES A
                if (gp.buttons & 0x08) uj |= 0x02; // X -> NES B
                if (gp.buttons & 0x40) uj |= 0x08; // Start -> NES Start
                if (gp.buttons & 0x80) uj |= 0x04; // Select -> NES Select

                if (ui == 0) usb1_joy = uj; else usb2_joy = uj;
            }
#endif

            /* Route inputs based on settings */
            int joypad1 = 0, joypad2 = 0;

            switch (g_settings.p1_mode) {
                case INPUT_MODE_ANY:      joypad1 = nespad1_joy | nespad2_joy | kbd_joy | usb1_joy | usb2_joy; break;
                case INPUT_MODE_NES1:     joypad1 = nespad1_joy; break;
                case INPUT_MODE_NES2:     joypad1 = nespad2_joy; break;
                case INPUT_MODE_USB1:     joypad1 = usb1_joy; break;
                case INPUT_MODE_USB2:     joypad1 = usb2_joy; break;
                case INPUT_MODE_KEYBOARD: joypad1 = kbd_joy; break;
                case INPUT_MODE_DISABLED: break;
            }

            switch (g_settings.p2_mode) {
                case INPUT_MODE_ANY:      joypad2 = nespad1_joy | nespad2_joy | kbd_joy | usb1_joy | usb2_joy; break;
                case INPUT_MODE_NES1:     joypad2 = nespad1_joy; break;
                case INPUT_MODE_NES2:     joypad2 = nespad2_joy; break;
                case INPUT_MODE_USB1:     joypad2 = usb1_joy; break;
                case INPUT_MODE_USB2:     joypad2 = usb2_joy; break;
                case INPUT_MODE_KEYBOARD: joypad2 = kbd_joy; break;
                case INPUT_MODE_DISABLED: break;
            }

            /* Turbo / autofire + A/B swap.
             *
             * Bit layout (matches nespad_to_qnes): A=0, B=1, Sel=2, Start=3,
             * U=4, D=5, L=6, R=7. When the corresponding turbo rate is non-OFF
             * AND the user is actually holding the button, we gate it through
             * a frame counter so the button reads as pressed only on a subset
             * of frames (producing rapid-fire at 10/15/30 Hz assuming 60 fps).
             * Applied symmetrically to both players. */
            static uint32_t turbo_frame = 0;
            turbo_frame++;
            int turbo_a_mask = 0, turbo_b_mask = 0;
            /* Frame-divider thresholds: at 60 fps, period of 6 frames = 10 Hz,
             * period 4 = 15 Hz, period 2 = 30 Hz. Mask on frames where the
             * half-period says "pressed". */
            switch (g_settings.turbo_a) {
                case TURBO_10: if (((turbo_frame / 3) & 1) == 0) turbo_a_mask = 0x01; else turbo_a_mask = 0x00; break;
                case TURBO_15: if (((turbo_frame / 2) & 1) == 0) turbo_a_mask = 0x01; else turbo_a_mask = 0x00; break;
                case TURBO_30: if ((turbo_frame & 1) == 0)        turbo_a_mask = 0x01; else turbo_a_mask = 0x00; break;
                case TURBO_OFF:
                default:       turbo_a_mask = 0x01; break; /* no gating */
            }
            switch (g_settings.turbo_b) {
                case TURBO_10: if (((turbo_frame / 3) & 1) == 0) turbo_b_mask = 0x02; else turbo_b_mask = 0x00; break;
                case TURBO_15: if (((turbo_frame / 2) & 1) == 0) turbo_b_mask = 0x02; else turbo_b_mask = 0x00; break;
                case TURBO_30: if ((turbo_frame & 1) == 0)        turbo_b_mask = 0x02; else turbo_b_mask = 0x00; break;
                case TURBO_OFF:
                default:       turbo_b_mask = 0x02; break;
            }
            /* Apply per-button turbo gates, then optional A/B swap. */
            int *jps[2] = { &joypad1, &joypad2 };
            for (int k = 0; k < 2; k++) {
                int v = *jps[k];
                if (g_settings.turbo_a != TURBO_OFF) v = (v & ~0x01) | (v & turbo_a_mask);
                if (g_settings.turbo_b != TURBO_OFF) v = (v & ~0x02) | (v & turbo_b_mask);
                if (g_settings.swap_ab) {
                    int a = v & 0x01, b = (v >> 1) & 0x01;
                    v = (v & ~0x03) | b | (a << 1);
                }
                *jps[k] = v;
            }

#ifdef VGA_HSTX_AUTOSTART
            /* Diagnostic: simulate continuous gameplay input so captured frames
             * reflect real stress on the driver.
             *
             * Strategy: for the first ~60 emulated seconds alternate Start
             * presses (30 frame on / 30 frame off) — this debounces cleanly
             * through the 1P/2P menu, then skips the long intro cutscene.
             * Anywhere else the game ignores Start. After that we hold Right
             * and pulse B so Bill runs and rapid-fires, which is the stress
             * case the user reported ("signal disappears while running"). */
            static uint32_t autoplay_frame = 0;
            int auto_joy = 0;
            uint32_t phase = autoplay_frame % 60;
            if (autoplay_frame < 3600) {
                /* Pulsed Start (30 on, 30 off). */
                if (phase < 30) auto_joy |= 0x08;
            } else {
                auto_joy |= 0x80;                          /* Right (hold) */
                if (((autoplay_frame / 4) & 1) == 0)
                    auto_joy |= 0x02;                      /* + B (rapid) */
            }
            autoplay_frame++;
            joypad1 |= auto_joy;
#endif
            qnes_emulate_frame(joypad1, joypad2);

            /* Push NES audio into DI queue. No padding — produce only what
             * the NES generates. Carry handles 4-sample boundary. Tiny rate
             * deficit (~1.5 samples/frame) handled by ISR silence fallback. */
            int16_t tmp[1024];
            long n = qnes_read_samples(tmp, 1024);

            if (n > 0) {
                audio_push_samples(tmp, (int)n);
            }

            update_palette(pal_write_idx);
            pending_pal_idx = pal_write_idx;
            pal_write_idx ^= 1;

#ifdef VIDEO_COMPOSITE
            tv_copy_frame(qnes_get_pixels(), 272);
#elif defined(HDMI_PIO)
            soft_copy_frame(qnes_get_pixels(), 272);
#elif defined(VGA_HSTX)
            vga_hstx_post_frame(qnes_get_pixels(), 272);
#else
            pending_pitch = 272;
            pending_pixels = qnes_get_pixels();
#endif

        }
        if (reset_requested) continue;
    } else if (selector_available) {
        /* Selector was available but load failed — loop back to selector */
        printf("ROM load failed, returning to selector\n");
        continue;
    } else {
        printf("No ROM loaded (no SD card ROM, no flash ROM).\n");
        while (1) { sleep_ms(100); }
    }

    break;  /* no reset — should not reach here normally */

    }  /* end outer while(1) loop */
}

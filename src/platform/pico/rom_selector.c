/*
 * MurmNES - ROM Selector Menu
 * Displays cartridges with cover art from SD card metadata.
 * Uses 8-bit indexed mode with a fixed 6x6x6 RGB color cube palette.
 * SPDX-License-Identifier: MIT
 */

#include "rom_selector.h"
#include "board_config.h"
#include "pico/stdlib.h"
#include "nespad.h"
#include "ps2kbd_wrapper.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

#ifdef USB_HID_ENABLED
#include "usbhid.h"
#endif

/* Symbols from main_pico.c */
extern volatile const uint8_t *pending_pixels;
extern volatile long pending_pitch;
extern volatile uint32_t vsync_flag;
extern uint32_t rgb565_palette_32[2][256];
extern int pal_write_idx;
extern volatile int pending_pal_idx;
extern uint8_t test_pixels[];
extern void audio_fill_silence(int count);

#define SCREEN_W 256
#define SCREEN_H 240
#define SAMPLE_RATE 44100

/* ─── Fixed 6x6x6 RGB color cube palette (216 colors) ────────────── */

/* Palette layout:
 *   0       = black (background)
 *   1-216   = 6x6x6 RGB cube
 *   217-255 = reserved for UI (cartridge body, text, etc.)
 */
#define PAL_BLACK     0
#define PAL_CUBE_BASE 1
#define PAL_CART_BODY  217  /* NES cart gray */
#define PAL_CART_LIGHT 218  /* top edge highlight */
#define PAL_CART_DARK  219  /* bottom/right shadow */
#define PAL_CART_LABEL 220  /* dark label background */
#define PAL_CART_RIDGE 221  /* ridge lines */
#define PAL_CART_SLOT  222  /* connector slot */
#define PAL_WHITE      223
#define PAL_GRAY       224
#define PAL_BG         225

static const uint8_t cube_levels[6] = {0, 51, 102, 153, 204, 255};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void setup_selector_palette(void) {
    uint32_t *pal = rgb565_palette_32[pal_write_idx];

    /* Entry 0: black */
    pal[0] = 0;

    /* 6x6x6 color cube */
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = PAL_CUBE_BASE + r * 36 + g * 6 + b;
                uint16_t c = rgb565(cube_levels[r], cube_levels[g], cube_levels[b]);
                pal[idx] = c | ((uint32_t)c << 16);
            }

    /* UI colors — NES cartridge style */
    uint16_t c;
    c = rgb565(0x88, 0x88, 0x88); pal[PAL_CART_BODY]  = c | ((uint32_t)c << 16);
    c = rgb565(0xA0, 0xA0, 0xA0); pal[PAL_CART_LIGHT] = c | ((uint32_t)c << 16);
    c = rgb565(0x60, 0x60, 0x60); pal[PAL_CART_DARK]  = c | ((uint32_t)c << 16);
    c = rgb565(0x18, 0x18, 0x20); pal[PAL_CART_LABEL] = c | ((uint32_t)c << 16);
    c = rgb565(0x70, 0x70, 0x70); pal[PAL_CART_RIDGE] = c | ((uint32_t)c << 16);
    c = rgb565(0x30, 0x30, 0x30); pal[PAL_CART_SLOT]  = c | ((uint32_t)c << 16);
    c = 0xFFFF;                    pal[PAL_WHITE]      = c | ((uint32_t)c << 16);
    c = rgb565(0x80, 0x80, 0x80); pal[PAL_GRAY]       = c | ((uint32_t)c << 16);
    c = rgb565(0x1A, 0x1A, 0x22); pal[PAL_BG]         = c | ((uint32_t)c << 16);

    pending_pal_idx = pal_write_idx;
    pal_write_idx ^= 1;
}

/* Map an RGB555 pixel to the nearest 6x6x6 cube palette index */
static uint8_t rgb555_to_pal(uint16_t p) {
    uint8_t r5 = (p >> 10) & 0x1F;
    uint8_t g5 = (p >> 5) & 0x1F;
    uint8_t b5 = p & 0x1F;
    /* Scale 0-31 to 0-5 (nearest cube level) */
    int ri = (r5 * 5 + 15) / 31;
    int gi = (g5 * 5 + 15) / 31;
    int bi = (b5 * 5 + 15) / 31;
    return (uint8_t)(PAL_CUBE_BASE + ri * 36 + gi * 6 + bi);
}

/* ─── Framebuffer helpers (8-bit indexed into test_pixels) ────────── */

static uint8_t *fb;  /* = test_pixels */

static inline void fb_pixel(int x, int y, uint8_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        fb[y * SCREEN_W + x] = color;
}

static void fb_fill(uint8_t color) {
    memset(fb, color, SCREEN_W * SCREEN_H);
}

static void fb_rect(int x, int y, int w, int h, uint8_t color) {
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++) {
        if (yy < 0) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
        if (x0 < x1) memset(&fb[yy * SCREEN_W + x0], color, x1 - x0);
    }
}

static void fb_hline(int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_H) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    if (x0 < x1) memset(&fb[y * SCREEN_W + x0], color, x1 - x0);
}

static void fb_vline(int x, int y, int h, uint8_t color) {
    if (x < 0 || x >= SCREEN_W) return;
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++)
        if (yy >= 0) fb[yy * SCREEN_W + x] = color;
}

/* ─── 5x7 font ───────────────────────────────────────────────────── */

static const uint8_t glyphs[][7] = {
    [' '-' '] = {0,0,0,0,0,0,0},
    ['!'-' '] = {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    ['('-' '] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'-' '] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['-'-' '] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'-' '] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    ['/'-' '] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    ['0'-' '] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'-' '] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'-' '] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'-' '] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    ['4'-' '] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'-' '] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    ['6'-' '] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    ['7'-' '] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'-' '] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'-' '] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
    [':'-' '] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    ['<'-' '] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    ['>'-' '] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    ['A'-' '] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'-' '] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'-' '] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'-' '] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'-' '] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'-' '] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
    ['J'-' '] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    ['K'-' '] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'-' '] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'-' '] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'-' '] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'-' '] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'-' '] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'-' '] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'-' '] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'-' '] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'-' '] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'-' '] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    ['W'-' '] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},
    ['Y'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},
    ['Z'-' '] = {0x1F,0x02,0x04,0x08,0x10,0x10,0x1F},
};

static void fb_char(int x, int y, char ch, uint8_t color) {
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    int idx = c - ' ';
    if (idx < 0 || idx >= (int)(sizeof(glyphs)/sizeof(glyphs[0]))) return;
    const uint8_t *g = glyphs[idx];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1u << (4 - col)))
                fb_pixel(x + col, y + row, color);
        }
    }
}

static void fb_text(int x, int y, const char *s, uint8_t color) {
    for (; *s; s++) { fb_char(x, y, *s, color); x += 6; }
}

static void fb_text_center(int y, const char *s, uint8_t color) {
    int x = (SCREEN_W - (int)strlen(s) * 6) / 2;
    fb_text(x, y, s, color);
}

/* ─── CRC32 ───────────────────────────────────────────────────────── */

static uint32_t crc32_table[256];
static bool crc32_ready = false;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

static uint32_t crc32_file(FIL *fil, int skip) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[512];
    f_lseek(fil, skip);
    while (1) {
        UINT br;
        if (f_read(fil, buf, sizeof(buf), &br) != FR_OK || br == 0) break;
        for (UINT i = 0; i < br; i++)
            crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ─── ROM list ────────────────────────────────────────────────────── */

#define MAX_ROMS 128

typedef struct {
    char filename[64];
    uint32_t crc;
    bool crc_valid;
} rom_entry_t;

static rom_entry_t rom_list[MAX_ROMS];
static int rom_count = 0;

static int scan_roms(void) {
    rom_count = 0;
    DIR dir;
    if (f_opendir(&dir, "/nes") != FR_OK) return 0;
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0' && rom_count < MAX_ROMS) {
        if (fno.fattrib & AM_DIR) continue;
        size_t len = strlen(fno.fname);
        if (len < 4) continue;
        if ((fno.fname[len-4] != '.') ||
            (fno.fname[len-3] != 'n' && fno.fname[len-3] != 'N') ||
            (fno.fname[len-2] != 'e' && fno.fname[len-2] != 'E') ||
            (fno.fname[len-1] != 's' && fno.fname[len-1] != 'S'))
            continue;
        strncpy(rom_list[rom_count].filename, fno.fname, sizeof(rom_list[0].filename) - 1);
        rom_list[rom_count].crc_valid = false;
        rom_count++;
    }
    f_closedir(&dir);
    return rom_count;
}

static void ensure_crc(int idx) {
    if (rom_list[idx].crc_valid) return;
    char path[ROM_PATH_MAX];
    snprintf(path, sizeof(path), "/nes/%s", rom_list[idx].filename);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        rom_list[idx].crc = crc32_file(&fil, 16);
        rom_list[idx].crc_valid = true;
        f_close(&fil);
        printf("CRC32(%s) = %08lX\n", rom_list[idx].filename, (unsigned long)rom_list[idx].crc);
    }
}

/* ─── Metadata (pre-loaded into memory at startup) ────────────────── */

/* All images stored sequentially in PSRAM starting at 3MB offset */
#define IMG_PSRAM_BASE (0x11000000 + 3 * 1024 * 1024)

typedef struct {
    uint16_t width, height;
    uint16_t *pixels;  /* points into PSRAM, NULL if no image */
    char title[64];    /* game title from metadata, or empty */
} rom_meta_t;

static rom_meta_t rom_meta[MAX_ROMS];
static uint32_t psram_alloc_offset = 0;  /* running PSRAM allocation pointer */

static void load_rom_metadata(int idx) {
    rom_meta[idx].width = 0;
    rom_meta[idx].height = 0;
    rom_meta[idx].pixels = NULL;
    rom_meta[idx].title[0] = '\0';

    if (!rom_list[idx].crc_valid) return;
    uint32_t crc = rom_list[idx].crc;
    char hex_char = "0123456789ABCDEF"[(crc >> 28) & 0xF];

    /* Load title */
    {
        char path[128];
        snprintf(path, sizeof(path), "/nes/metadata/descr/%c/%08lX.txt", hex_char, (unsigned long)crc);
        FIL fil;
        if (f_open(&fil, path, FA_READ) == FR_OK) {
            char buf[512];
            UINT br;
            if (f_read(&fil, buf, sizeof(buf) - 1, &br) == FR_OK) {
                buf[br] = '\0';
                char *start = strstr(buf, "<name>");
                if (start) {
                    start += 6;
                    char *end = strstr(start, "</name>");
                    if (end) {
                        int len = end - start;
                        if (len > 63) len = 63;
                        memcpy(rom_meta[idx].title, start, len);
                        rom_meta[idx].title[len] = '\0';
                    }
                }
            }
            f_close(&fil);
        }
    }

    /* Load image into PSRAM */
    {
        char path[128];
        snprintf(path, sizeof(path), "/nes/metadata/Images/160/%c/%08lX.555", hex_char, (unsigned long)crc);
        FIL fil;
        if (f_open(&fil, path, FA_READ) != FR_OK) return;

        uint8_t hdr[4];
        UINT br;
        if (f_read(&fil, hdr, 4, &br) != FR_OK || br != 4) { f_close(&fil); return; }

        uint16_t w = hdr[0] | (hdr[1] << 8);
        uint16_t h = hdr[2] | (hdr[3] << 8);
        if (w == 0 || w > 320 || h == 0 || h > 240) { f_close(&fil); return; }

        uint32_t data_size = (uint32_t)w * h * 2;
        uint16_t *pixels = (uint16_t *)(IMG_PSRAM_BASE + psram_alloc_offset);

        if (f_read(&fil, pixels, data_size, &br) == FR_OK && br == data_size) {
            rom_meta[idx].width = w;
            rom_meta[idx].height = h;
            rom_meta[idx].pixels = pixels;
            psram_alloc_offset += data_size;
            /* Align to 4 bytes */
            psram_alloc_offset = (psram_alloc_offset + 3) & ~3;
            printf("Image: %s (%dx%d)\n", path, w, h);
        }
        f_close(&fil);
    }
}

/* ─── NES-style cartridge rendering (pixel-perfect) ──────────────── */

/*
 * NES cart layout (from reference pixel art):
 *   ┌─┬──────┬──────────────┬─┐   ← top edge with corner cuts
 *   │ │RIDGES│              │ │
 *   │ │      │   LABEL      │ │   ← ridges on LEFT, label on RIGHT
 *   │ │      │   (image)    │ │
 *   │ │      │              │ │
 *   │ │      ├──────────────┤ │
 *   │ │      │    ▽         │ │   ← arrow below label
 *   ├─┴──────┴──────────────┴─┤
 *   │        BOTTOM BASE       │   ← connector base
 *   └─────────────────────────-┘
 */

#define CART_W    134
#define CART_H    158
#define CART_X    ((SCREEN_W - CART_W) / 2)
#define CART_Y    10

/* Left outer strip (thin edge) */
#define LSTRIP_W  4

/* Ridge panel (left side, inside the strip) */
#define RIDGE_X   (CART_X + LSTRIP_W + 1)
#define RIDGE_W   30
#define RIDGE_TOP (CART_Y + 4)

/* Label area (right side) */
#define LABEL_X   (RIDGE_X + RIDGE_W + 3)
#define LABEL_Y   (CART_Y + 5)
#define LABEL_W   (CART_X + CART_W - LSTRIP_W - 1 - LABEL_X)

/* Bottom base section */
#define BASE_H    24
#define BASE_Y    (CART_Y + CART_H - BASE_H)

/* Label height goes down to above the base */
#define LABEL_H   (BASE_Y - 22 - LABEL_Y)

/* Arrow section (between label bottom and base) */
#define ARROW_Y   (LABEL_Y + LABEL_H + 6)

static void draw_cartridge(int selected) {
    fb_fill(PAL_BG);

    /* ── Main body fill ── */
    fb_rect(CART_X, CART_Y, CART_W, CART_H, PAL_CART_BODY);

    /* ── Corner cuts (top-left and top-right notches) ── */
    fb_rect(CART_X, CART_Y, 3, 2, PAL_BG);
    fb_rect(CART_X + CART_W - 3, CART_Y, 3, 2, PAL_BG);

    /* ── Outer edges ── */
    /* Top edge highlight */
    fb_hline(CART_X + 3, CART_Y, CART_W - 6, PAL_CART_LIGHT);
    /* Left edge highlight */
    fb_vline(CART_X, CART_Y + 2, CART_H - 2, PAL_CART_LIGHT);
    /* Right edge shadow */
    fb_vline(CART_X + CART_W - 1, CART_Y + 2, CART_H - 2, PAL_CART_DARK);
    /* Bottom edge shadow */
    fb_hline(CART_X, CART_Y + CART_H - 1, CART_W, PAL_CART_DARK);

    /* ── Left outer strip (narrow vertical bar) ── */
    fb_vline(CART_X + LSTRIP_W, CART_Y + 2, CART_H - BASE_H - 2, PAL_CART_DARK);

    /* ── Ridge panel (horizontal vents on the left) ── */
    int ridge_bottom = BASE_Y - 4;
    int ridge_count = (ridge_bottom - RIDGE_TOP) / 5;
    for (int i = 0; i < ridge_count; i++) {
        int ry = RIDGE_TOP + i * 5;
        fb_hline(RIDGE_X, ry,     RIDGE_W, PAL_CART_RIDGE);
        fb_hline(RIDGE_X, ry + 1, RIDGE_W, PAL_CART_DARK);
        fb_hline(RIDGE_X, ry + 2, RIDGE_W, PAL_CART_BODY);
    }

    /* ── Separator between ridges and label ── */
    fb_vline(LABEL_X - 2, CART_Y + 3, CART_H - BASE_H - 5, PAL_CART_DARK);
    fb_vline(LABEL_X - 1, CART_Y + 3, CART_H - BASE_H - 5, PAL_CART_LIGHT);

    /* ── Label area (dark background) ── */
    fb_rect(LABEL_X, LABEL_Y, LABEL_W, LABEL_H, PAL_CART_LABEL);
    /* Label border */
    fb_hline(LABEL_X - 1, LABEL_Y - 1, LABEL_W + 2, PAL_CART_DARK);
    fb_hline(LABEL_X - 1, LABEL_Y + LABEL_H, LABEL_W + 2, PAL_CART_DARK);
    fb_vline(LABEL_X - 1, LABEL_Y - 1, LABEL_H + 2, PAL_CART_DARK);
    fb_vline(LABEL_X + LABEL_W, LABEL_Y - 1, LABEL_H + 2, PAL_CART_DARK);

    /* ── Blit image into label (top-aligned, width-limited, no crop) ── */
    rom_meta_t *meta = &rom_meta[selected];
    if (meta->pixels) {
        int iw = meta->width;
        int ih = meta->height;
        /* Scale to fit label width if needed */
        if (iw > LABEL_W) {
            ih = ih * LABEL_W / iw;
            iw = LABEL_W;
        }
        if (ih > LABEL_H) ih = LABEL_H;

        int ix = LABEL_X + (LABEL_W - iw) / 2;
        int iy = LABEL_Y;

        for (int y = 0; y < ih; y++) {
            int sy = y * meta->height / ih;
            for (int x = 0; x < iw; x++) {
                int sx = x * meta->width / iw;
                uint16_t px = meta->pixels[sy * meta->width + sx];
                fb_pixel(ix + x, iy + y, rgb555_to_pal(px));
            }
        }
    }

    /* ── Small triangle arrow below label ── */
    int ax = LABEL_X + LABEL_W / 2;
    int ay = ARROW_Y;
    for (int row = 0; row < 5; row++) {
        fb_hline(ax - (4 - row), ay + row, (4 - row) * 2 + 1, PAL_CART_DARK);
    }

    /* ── Base section divider ── */
    fb_hline(CART_X + 1, BASE_Y, CART_W - 2, PAL_CART_DARK);
    fb_hline(CART_X + 1, BASE_Y + 1, CART_W - 2, PAL_CART_LIGHT);

    /* ── Title below cartridge ── */
    const char *title = meta->title[0] ? meta->title : rom_list[selected].filename;
    char dt[40];
    int max_c = (SCREEN_W - 20) / 6;
    if (max_c > 39) max_c = 39;
    strncpy(dt, title, max_c);
    dt[max_c] = '\0';
    fb_text_center(CART_Y + CART_H + 6, dt, PAL_WHITE);

    /* Counter */
    char counter[16];
    snprintf(counter, sizeof(counter), "%d / %d", selected + 1, rom_count);
    fb_text_center(CART_Y + CART_H + 20, counter, PAL_GRAY);

    /* Nav hint */
    fb_text_center(SCREEN_H - 14, "< LEFT/RIGHT >   A: START", PAL_GRAY);
}

/* ─── Input ───────────────────────────────────────────────────────── */

#define BTN_LEFT  0x01
#define BTN_RIGHT 0x02
#define BTN_A     0x04
#define BTN_START 0x08

static int read_selector_buttons(void) {
    nespad_read();
    ps2kbd_tick();
    int buttons = 0;
    uint32_t pad = nespad_state | nespad_state2;
    if (pad & DPAD_LEFT)  buttons |= BTN_LEFT;
    if (pad & DPAD_RIGHT) buttons |= BTN_RIGHT;
    if (pad & DPAD_A)     buttons |= BTN_A;
    if (pad & DPAD_START) buttons |= BTN_START;
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_LEFT)  buttons |= BTN_LEFT;
    if (kbd & KBD_STATE_RIGHT) buttons |= BTN_RIGHT;
    if (kbd & KBD_STATE_A)     buttons |= BTN_A;
    if (kbd & KBD_STATE_START) buttons |= BTN_START;
#ifdef USB_HID_ENABLED
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        if (gp.dpad & 0x04) buttons |= BTN_LEFT;
        if (gp.dpad & 0x08) buttons |= BTN_RIGHT;
        if (gp.buttons & 0x01) buttons |= BTN_A;
        if (gp.buttons & 0x40) buttons |= BTN_START;
    }
#endif
    return buttons;
}

/* ─── Main selector loop ──────────────────────────────────────────── */

static void selector_wait_vsync(void) {
    while (pending_pixels != NULL) {
#ifdef USB_HID_ENABLED
        usbhid_task();
#endif
        __asm volatile ("wfe");
    }
    vsync_flag = 0;
}

/* Post current framebuffer and wait for vsync — keeps HDMI alive during loading */
static void post_frame(void) {
    selector_wait_vsync();
    pending_pitch = SCREEN_W;
    pending_pixels = fb;
}

/* Show a loading screen with progress */
static void draw_loading(int current, int total) {
    fb_fill(PAL_BG);
    fb_text_center(SCREEN_H / 2 - 10, "LOADING...", PAL_WHITE);
    char progress[24];
    snprintf(progress, sizeof(progress), "%d / %d", current, total);
    fb_text_center(SCREEN_H / 2 + 4, progress, PAL_GRAY);
}

/* Pre-compute CRCs and load all metadata at startup (with loading screen) */
static void preload_all_roms(void) {
    psram_alloc_offset = 0;
    for (int i = 0; i < rom_count; i++) {
        draw_loading(i + 1, rom_count);
        post_frame();
        ensure_crc(i);
        load_rom_metadata(i);
    }
}

bool rom_selector_show(long *out_rom_size) {
    fb = test_pixels;

    static FATFS sel_fs;
    if (f_mount(&sel_fs, "", 1) != FR_OK) {
        printf("ROM selector: SD mount failed\n");
        return false;
    }

    int count = scan_roms();
    printf("ROM selector: found %d ROMs\n", count);

    if (count == 0) {
        f_unmount("");
        return false;
    }

    /* Set up 6x6x6 palette */
    setup_selector_palette();

    /* Pre-load all CRCs, titles, and images (shows loading screen) */
    preload_all_roms();

    int selected = 0;
    int prev_selected = -1;
    int prev_buttons = 0;
    uint32_t hold_counter = 0;

    while (1) {
        selector_wait_vsync();

        int buttons = read_selector_buttons();
        int pressed = buttons & ~prev_buttons;
        if (buttons != 0 && buttons == prev_buttons) {
            hold_counter++;
            if (hold_counter > 20 && (hold_counter % 5) == 0)
                pressed = buttons;
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        if (pressed & BTN_LEFT)
            selected = (selected - 1 + rom_count) % rom_count;
        if (pressed & BTN_RIGHT)
            selected = (selected + 1) % rom_count;

        if (pressed & (BTN_A | BTN_START)) {
            /* Set ROM name */
            extern char g_rom_name[64];
            const char *fname = rom_list[selected].filename;
            size_t nlen = strlen(fname);
            if (nlen >= 4) nlen -= 4;
            if (nlen >= sizeof(g_rom_name)) nlen = sizeof(g_rom_name) - 1;
            memcpy(g_rom_name, fname, nlen);
            g_rom_name[nlen] = '\0';

            /* Show loading message while reading ROM from SD */
            printf("Selected: %s\n", fname);
            fb_fill(PAL_BG);
            fb_text_center(SCREEN_H / 2 - 3, "LOADING...", PAL_WHITE);
            post_frame();

            /* Load ROM into PSRAM_BASE */
            char rom_path[ROM_PATH_MAX];
            snprintf(rom_path, sizeof(rom_path), "/nes/%s", fname);
            printf("Opening %s...\n", rom_path);
            FIL fil;
            bool ok = false;
            FRESULT fr = f_open(&fil, rom_path, FA_READ);
            printf("f_open=%d\n", fr);
            if (fr == FR_OK) {
                FSIZE_t fsize = f_size(&fil);
                printf("Reading %lu bytes into PSRAM...\n", (unsigned long)fsize);
                uint8_t *rom_buf = (uint8_t *)0x11000000;  /* PSRAM_BASE */
                UINT br;
                FRESULT fr2 = f_read(&fil, rom_buf, (UINT)fsize, &br);
                printf("f_read=%d br=%u\n", fr2, br);
                if (fr2 == FR_OK && br == (UINT)fsize) {
                    *out_rom_size = (long)fsize;
                    ok = true;
                    printf("ROM loaded OK\n");
                } else {
                    printf("ROM read FAILED\n");
                }
                f_close(&fil);
            }

            f_unmount("");
            return ok;
        }

        if (selected != prev_selected) {
            prev_selected = selected;
            draw_cartridge(selected);  /* instant — all data in RAM/PSRAM */
        }

        pending_pitch = SCREEN_W;
        pending_pixels = fb;
    }
}

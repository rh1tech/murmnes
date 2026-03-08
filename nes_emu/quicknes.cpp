/*
 * QuickNES C wrapper implementation
 */

#include "quicknes.h"
#include "Nes_Emu.h"
#include "Data_Reader.h"
#include <stdio.h>

/* Emulator allocated on demand — avoids global C++ constructor running before main() */
static Nes_Emu *emu;

/* Double-buffered pixel output: emulator writes to back buffer while
   the display reads from the front buffer (no tearing). */
#define PIXEL_BUF_SIZE ((256 + 16) * (240 + 2))
static uint8_t pixel_bufs[2][PIXEL_BUF_SIZE];
static int back_buf = 0;  /* index emulator writes to */
static int front_buf = 0; /* index display reads from */

static bool initialized = false;
static bool rom_loaded = false;

/* External tile cache buffer (PSRAM) — set by qnes_set_tile_cache_buf */
static void *ext_tile_cache_buf = NULL;
static long ext_tile_cache_size = 0;

void qnes_set_tile_cache_buf(void *buf, long size)
{
    ext_tile_cache_buf = buf;
    ext_tile_cache_size = size;
}

void *qnes_get_tile_cache_buf(long *out_size)
{
    if (out_size) *out_size = ext_tile_cache_size;
    return ext_tile_cache_buf;
}

int qnes_init(long sample_rate)
{
    if (initialized)
        return 0;

    emu = new Nes_Emu;
    if (!emu) {
        printf("qnes: failed to allocate Nes_Emu\n");
        return -1;
    }

    back_buf = 0;
    front_buf = 0;
    emu->set_pixels(pixel_bufs[back_buf], 256 + 16);

    const char *err = emu->set_sample_rate(sample_rate);
    if (err) {
        printf("set_sample_rate: %s\n", err);
        return -1;
    }

    emu->set_palette_range(0);
    initialized = true;
    return 0;
}

int qnes_load_rom(const void *data, long size)
{
    if (!initialized)
        return -1;

    Mem_File_Reader reader(data, size);
    Auto_File_Reader in(reader);
    const char *err = emu->load_ines(in);
    if (err) {
        printf("load_ines: %s\n", err);
        return -1;
    }

    rom_loaded = true;
    return 0;
}

int qnes_load_rom_inplace(const void *data, long size)
{
    if (!initialized)
        return -1;

    const char *err = emu->load_ines_data(data, size);
    if (err) {
        printf("load_ines_data: %s\n", err);
        return -1;
    }

    rom_loaded = true;
    return 0;
}

int qnes_emulate_frame(int joypad1, int joypad2)
{
    if (!rom_loaded)
        return -1;

    const char *err = emu->emulate_frame(joypad1, joypad2);
    if (err)
        return -1;

    /* Frame complete — swap buffers. Display now reads the just-finished
       frame while the emulator will write to the other buffer next time. */
    front_buf = back_buf;
    back_buf ^= 1;
    emu->set_pixels(pixel_bufs[back_buf], 256 + 16);
    return 0;
}

const uint8_t *qnes_get_pixels(void)
{
    if (!rom_loaded)
        return 0;

    /* frame().pixels was set during emulate_frame() and still points
       into the front buffer (the swap doesn't overwrite it). */
    return emu->frame().pixels;
}

const int16_t *qnes_get_palette(int *out_size)
{
    if (!rom_loaded)
        return 0;

    const Nes_Emu::frame_t &f = emu->frame();
    if (out_size)
        *out_size = f.palette_size;
    return f.palette;
}

const qnes_rgb_t *qnes_get_color_table(void)
{
    /* Nes_Emu::rgb_t and qnes_rgb_t have identical layout */
    return (const qnes_rgb_t *)Nes_Emu::nes_colors;
}

long qnes_read_samples(int16_t *out, long max_samples)
{
    if (!rom_loaded)
        return 0;

    return emu->read_samples(out, max_samples);
}

void qnes_reset(int full_reset)
{
    if (rom_loaded)
        emu->reset(full_reset != 0);
}

void qnes_close(void)
{
    if (rom_loaded) {
        emu->close();
        rom_loaded = false;
    }
}

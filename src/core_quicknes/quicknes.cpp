/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#include "quicknes.h"
#include "nes_emu.h"
#include "nes_state.h"
#include "data_reader.h"
#include "abstract_file.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

extern "C" void ppu_set_region(int region);

/* Region mode: 0=NTSC, 1=Dendy */
static int current_region = QNES_REGION_NTSC;

void qnes_set_region(int region) { current_region = region; }
int qnes_get_region(void) { return current_region; }

/* Sprite limit mode: true=8/scanline (hardware accurate), false=unlimited (no flicker) */
static bool sprite_limit_enabled = true;

/* Current equalizer preset — remembered so the setting survives re-init. */
static int current_eq_preset = QNES_EQ_NES;

static Nes_Emu::equalizer_t const *eq_preset_ptr(int preset)
{
    switch (preset) {
        case QNES_EQ_FAMICOM: return &Nes_Emu::famicom_eq;
        case QNES_EQ_TV:      return &Nes_Emu::tv_eq;
        case QNES_EQ_FLAT:    return &Nes_Emu::flat_eq;
        case QNES_EQ_CRISP:   return &Nes_Emu::crisp_eq;
        case QNES_EQ_TINNY:   return &Nes_Emu::tinny_eq;
        case QNES_EQ_NES:
        default:              return &Nes_Emu::nes_eq;
    }
}

static void apply_region_settings(void);

/* Emulator allocated on demand — avoids global C++ constructor running before main() */
static Nes_Emu *emu;

/* Double-buffered pixel output: emulator writes to back buffer while
   the display reads from the front buffer (no tearing).
   Define QNES_SINGLE_PIXEL_BUF on memory-tight platforms (no PSRAM): saves 64 KiB
   BSS at the cost of sprite-blink persistence when sprites drop off (30 Hz flicker). */
#ifdef QNES_SINGLE_PIXEL_BUF
#define PIXEL_BUF_COUNT 1
#else
#define PIXEL_BUF_COUNT 2
#endif
#define PIXEL_BUF_SIZE ((256 + 16) * (240 + 2))
static uint8_t pixel_bufs[PIXEL_BUF_COUNT][PIXEL_BUF_SIZE];
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

void qnes_set_sprite_limit(int enabled)
{
    sprite_limit_enabled = enabled ? true : false;
    if (emu) {
        emu->set_sprite_mode(sprite_limit_enabled
            ? Nes_Emu::sprites_visible
            : Nes_Emu::sprites_enhanced);
    }
}

void qnes_set_audio_eq(int preset)
{
    if (preset < 0 || preset >= QNES_EQ_COUNT) preset = QNES_EQ_NES;
    current_eq_preset = preset;
    if (emu) {
        emu->set_equalizer(*eq_preset_ptr(preset));
    }
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
    emu->set_sprite_mode(sprite_limit_enabled
        ? Nes_Emu::sprites_visible
        : Nes_Emu::sprites_enhanced);
    emu->set_equalizer(*eq_preset_ptr(current_eq_preset));
    initialized = true;
    return 0;
}

static void apply_region_settings(void)
{
    ppu_set_region(current_region);
    if (current_region == QNES_REGION_DENDY) {
        Nes_Emu::frame_rate = 50;
        Nes_Emu::cpu_clock_rate = 1773447;
    } else {
        Nes_Emu::frame_rate = 60;
        Nes_Emu::cpu_clock_rate = 1789773;
    }
    if (emu)
        emu->set_frame_rate(Nes_Emu::frame_rate);
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

    apply_region_settings();
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

    apply_region_settings();
    rom_loaded = true;
    return 0;
}

int __attribute__((section(".time_critical.qnes_emulate_frame"))) qnes_emulate_frame(int joypad1, int joypad2)
{
    if (!rom_loaded)
        return -1;

    const char *err = emu->emulate_frame(joypad1, joypad2);
    if (err)
        return -1;

    /* Count visible sprites for blink detection */
    {
        const uint8_t *oam = emu->oam_data();
        int vis = 0;
        for (int i = 0; i < 64; i++)
            if (oam[i * 4] < 0xF0) vis++;
        emu->visible_sprite_count = vis;
    }

    /* Frame complete — swap buffers. Display now reads the just-finished
       frame while the emulator will write to the other buffer next time. */
    front_buf = back_buf;
#if PIXEL_BUF_COUNT > 1
    back_buf ^= 1;

    /* Sprite blink persistence: when the visible sprite count drops
       significantly between frames (30 Hz invincibility blink), merge
       sprite pixels from the previous frame into the current one.  This
       is only applied on blink-hide frames so moving sprites on normal
       frames never leave trails.  Fixes HDMI display timing race where
       the vsync phase can lock to the "hidden" blink frames. */
    {
        static int prev_sprite_count = 0;
        int cur_count = emu->visible_sprite_count;

        if (prev_sprite_count - cur_count >= 3) {
            /* Blink-hide frame: copy sprite pixels from previous frame */
            uint8_t *cur = pixel_bufs[front_buf];
            const uint8_t *prev = pixel_bufs[back_buf];
            for (int i = 0; i < PIXEL_BUF_SIZE; i++) {
                if (!(cur[i] & 0x10) && (prev[i] & 0x10))
                    cur[i] = prev[i];
            }
        }
        prev_sprite_count = cur_count;
    }
#endif

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

/* FatFS-backed Data_Writer: streams directly to an open file (no malloc) */
class FatFS_Writer : public Data_Writer {
    FIL *fil;
public:
    FatFS_Writer(FIL *f) : fil(f) {}
    const char *write(const void *p, long n) {
        UINT bw;
        FRESULT fr = f_write(fil, p, (UINT)n, &bw);
        if (fr != FR_OK || bw != (UINT)n) return "SD write error";
        return 0;
    }
};

/* FatFS-backed Data_Reader: streams directly from an open file (no malloc) */
class FatFS_Reader : public Data_Reader {
    FIL *fil;
public:
    FatFS_Reader(FIL *f, long size) : fil(f) { set_remain(size); }
    const char *read_v(void *p, int n) {
        UINT br;
        FRESULT fr = f_read(fil, p, (UINT)n, &br);
        if (fr != FR_OK || br != (UINT)n) return "SD read error";
        return 0;
    }
    const char *skip_v(int n) {
        FRESULT fr = f_lseek(fil, f_tell(fil) + n);
        if (fr != FR_OK) return "SD seek error";
        return 0;
    }
};

/* Nes_State allocated on demand to avoid permanent ~25KB SRAM usage */
static Nes_State *save_load_state = NULL;

static Nes_State *get_save_load_state(void) {
    if (!save_load_state) {
        save_load_state = new Nes_State;
    }
    return save_load_state;
}

int qnes_save_state(qnes_file_t file)
{
    if (!rom_loaded) return -1;

    Nes_State *state = get_save_load_state();
    if (!state) return -1;

    emu->save_state(state);

    FatFS_Writer writer((FIL *)file);
    const char *err = state->write(Auto_File_Writer(writer));
    if (err) {
        printf("qnes_save_state: %s\n", err);
        return -1;
    }
    return 0;
}

int qnes_load_state(qnes_file_t file, long file_size)
{
    if (!rom_loaded) return -1;

    FatFS_Reader reader((FIL *)file, file_size);
    Auto_File_Reader in(reader);

    Nes_State *state = get_save_load_state();
    if (!state) return -1;

    const char *err = state->read(in);
    if (err) {
        printf("qnes_load_state: %s\n", err);
        return -1;
    }

    emu->load_state(*state);
    return 0;
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

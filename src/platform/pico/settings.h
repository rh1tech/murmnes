/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

// Input mode values
#define INPUT_MODE_ANY      0
#define INPUT_MODE_NES1     1
#define INPUT_MODE_NES2     2
#define INPUT_MODE_USB1     3
#define INPUT_MODE_USB2     4
#define INPUT_MODE_KEYBOARD 5
#define INPUT_MODE_DISABLED 6
#define INPUT_MODE_COUNT    7

// Audio output mode
#define AUDIO_MODE_HDMI     0
#define AUDIO_MODE_I2S      1
#define AUDIO_MODE_PWM      2
#define AUDIO_MODE_DISABLED 3
#define AUDIO_MODE_PCM5122  4  // Waveshare PCM5122 I2S DAC (Z0 only)
#define AUDIO_MODE_COUNT    5

// Volume range (0-100, step 10)
#define VOLUME_MIN  0
#define VOLUME_MAX  100
#define VOLUME_STEP 10

// ROM selector view mode
#define SELECTOR_MODE_CAROUSEL 0
#define SELECTOR_MODE_BROWSER  1

// Emulation mode (region)
#define EMULATION_MODE_NES   0
#define EMULATION_MODE_DENDY 1
#define EMULATION_MODE_COUNT 2

// Audio equalizer presets (must match QNES_EQ_* in quicknes.h)
#define AUDIO_EQ_NES     0
#define AUDIO_EQ_FAMICOM 1
#define AUDIO_EQ_TV      2
#define AUDIO_EQ_FLAT    3
#define AUDIO_EQ_CRISP   4
#define AUDIO_EQ_TINNY   5
#define AUDIO_EQ_COUNT   6

// Low-pass amount — 0..10, applied on top of the EQ preset.
#define LOWPASS_MIN 0
#define LOWPASS_MAX 10

// Overscan crop (rows hidden top+bottom AND columns hidden left+right)
#define OVERSCAN_OFF  0  // show full 256x240 (except the always-clipped black frame)
#define OVERSCAN_8    1  // hide 8 rows / 8 cols (classic NTSC TV safe area)
#define OVERSCAN_16   2  // hide 16 rows / 16 cols (more aggressive)
#define OVERSCAN_COUNT 3

// Palette selector
#define PALETTE_NES        0  // QuickNES default
#define PALETTE_FIREBRANDX 1  // FirebrandX
#define PALETTE_WAVEBEAM   2  // Wavebeam
#define PALETTE_COMPOSITE  3  // Composite direct
#define PALETTE_CUSTOM     4  // Loaded from SD (Batch 3)
#define PALETTE_COUNT      5

// Turbo rate (A or B held → rapid press at N Hz)
#define TURBO_OFF 0
#define TURBO_10  1  // 10 Hz
#define TURBO_15  2  // 15 Hz
#define TURBO_30  3  // 30 Hz
#define TURBO_COUNT 4

// Button remap targets. Values match the NES joypad bit index with
// REMAP_NONE encoded as 0xFF so a source mapped to nothing disappears
// from the joypad word.
#define REMAP_A      0
#define REMAP_B      1
#define REMAP_SELECT 2
#define REMAP_START  3
#define REMAP_UP     4
#define REMAP_DOWN   5
#define REMAP_LEFT   6
#define REMAP_RIGHT  7
#define REMAP_NONE   0xFF

// We expose remapping for A, B, Select, Start only. U/D/L/R pass through —
// swapping directions is almost never what users want and would bloat the UI.
#define REMAP_SRC_COUNT 4

typedef struct {
    uint8_t p1_mode;        // Player 1 input mode (INPUT_MODE_*)
    uint8_t p2_mode;        // Player 2 input mode (INPUT_MODE_*)
    uint8_t audio_mode;     // Audio output (AUDIO_MODE_*)
    uint8_t volume;         // Master volume 0-100
    uint8_t selector_mode;  // ROM selector view (SELECTOR_MODE_*)
    uint8_t emu_mode;       // Emulation mode (EMULATION_MODE_*)
    uint8_t sprite_limit;   // 1 = 8 sprites/scanline (default), 0 = unlimited (no flicker)
    uint8_t audio_eq;       // Audio equalizer preset (AUDIO_EQ_*)
    uint8_t overscan;       // Overscan crop (OVERSCAN_*)
    uint8_t palette;        // Palette selector (PALETTE_*)
    uint8_t turbo_a;        // Turbo A rate (TURBO_*)
    uint8_t turbo_b;        // Turbo B rate (TURBO_*)
    uint8_t swap_ab;        // 1 = swap A and B buttons (shortcut; also set via remap)
    uint8_t bg_disabled;    // 1 = skip background layer rendering
    uint8_t chan_mute_mask; // 5-bit 2A03 channel mute bitmask (QNES_CHAN_*)
    uint8_t expansion_muted;// 1 = mute mapper-owned expansion audio
    uint8_t lowpass;        // Extra low-pass amount 0..LOWPASS_MAX
    /* Per-source button remap. Index = source bit (0=A, 1=B, 2=Sel, 3=Start).
     * Value = REMAP_* target bit (0..7) or REMAP_NONE. Defaults to identity. */
    uint8_t remap[REMAP_SRC_COUNT];
    char browser_path[280]; // Last file browser directory
    char browser_file[256]; // Last launched file name (in browser_path)
} settings_t;

extern settings_t g_settings;

typedef enum {
    SETTINGS_RESULT_EXIT,
    SETTINGS_RESULT_RESET,
    SETTINGS_RESULT_RESTART,   /* Soft-reset the currently running ROM */
} settings_result_t;

/**
 * Load settings from SD card (/nes/.settings)
 * If file doesn't exist, uses defaults.
 */
void settings_load(void);

/**
 * Save current settings to SD card (/nes/.settings)
 */
void settings_save(void);

/* Current ROM name (no path/extension) — set by main_pico.c during ROM load */
extern char g_rom_name[64];

/**
 * Check if menu hotkey is pressed (Start+Select, ESC, or F12)
 * Call this during the emulation loop.
 */
bool settings_check_hotkey(void);

/**
 * Display settings menu and block until user exits.
 * @param screen_buffer 256*240 byte buffer for menu rendering
 * @return SETTINGS_RESULT_CANCEL when user exits
 */
settings_result_t settings_menu_show(uint8_t *screen_buffer);

#endif /* SETTINGS_H */

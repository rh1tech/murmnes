/*
 * FRANK NES - ROM Selector Menu
 * Displays cartridges with cover art from SD card metadata.
 * SPDX-License-Identifier: MIT
 */

#ifndef ROM_SELECTOR_H
#define ROM_SELECTOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum ROM filename length (including path) */
#define ROM_PATH_MAX 280

/* Result of the SD scan performed during preload. */
typedef enum {
    ROM_SCAN_OK = 0,        /* at least one .nes found in /nes */
    ROM_SCAN_NO_SD,         /* SD mount failed */
    ROM_SCAN_NO_NES_DIR,    /* SD mounted but /nes directory missing */
    ROM_SCAN_NO_ROMS,       /* /nes exists but has no .nes files */
} rom_scan_result_t;

/**
 * Phase 1: Mount SD, scan /nes for .nes files, load CRC cache.
 * Fast — no per-file I/O. Call BEFORE HDMI starts.
 * @param out_rom_size  Unused (always set to 0), kept for API compat
 * @return Number of ROMs found
 */
int rom_selector_preload_scan(long *out_rom_size);

/**
 * Initialize framebuffer and palette for the preload progress screen.
 * Call AFTER HDMI starts but BEFORE rom_selector_preload_index().
 */
void rom_selector_preload_init_display(void);

/**
 * Phase 2: Compute missing CRCs, load metadata, remember last ROM.
 * Slow — shows progress bar on screen. Call AFTER HDMI starts.
 * SD is already mounted from preload_scan; unmounted on return.
 */
void rom_selector_preload_index(void);

/**
 * Show the ROM selector UI. Loads the selected ROM from SD on demand.
 * Call AFTER HDMI starts.
 * @param out_rom_size  Receives size of selected ROM in PSRAM
 * @return true if ROM selected and loaded
 */
bool rom_selector_show(long *out_rom_size);

/**
 * Get PSRAM pointer to the last selected/loaded ROM data.
 */
void *rom_selector_get_rom_data(void);

/**
 * Show the welcome/splash screen. Call AFTER HDMI starts.
 * Waits for user input or auto-continues after timeout.
 */
void welcome_screen_show(void);

/**
 * Show "No SD Card" error screen for a few seconds.
 */
void sd_error_show(void);

/**
 * Returns true if the SD card was successfully mounted during preload.
 */
bool rom_selector_sd_ok(void);

/**
 * Get the result of the last SD scan (why the ROM library is empty, if it is).
 */
rom_scan_result_t rom_selector_scan_result(void);

/**
 * Display a "no ROMs" notice with instructions, then wait for user input.
 * Uses the scan result to tailor the message.
 */
void rom_selector_no_roms_notice(void);

#ifdef __cplusplus
}
#endif

#endif /* ROM_SELECTOR_H */

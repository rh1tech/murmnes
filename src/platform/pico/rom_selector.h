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

#ifdef __cplusplus
}
#endif

#endif /* ROM_SELECTOR_H */

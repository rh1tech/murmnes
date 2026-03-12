/*
 * MurmNES - ROM Selector Menu
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
 * Scan SD card for ROMs and load metadata (CRCs, titles).
 * ROM data is NOT loaded — it will be loaded on demand when selected.
 * Call BEFORE HDMI starts.
 * @param out_rom_size  Unused (always set to 0), kept for API compat
 * @return Number of ROMs found
 */
int rom_selector_preload(long *out_rom_size);

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

#ifdef __cplusplus
}
#endif

#endif /* ROM_SELECTOR_H */

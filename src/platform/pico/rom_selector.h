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
 * Show the ROM selector menu. Scans /nes/ for .nes files,
 * displays cartridges with metadata images.
 *
 * On success, the selected ROM is loaded into PSRAM at PSRAM_BASE
 * and out_rom_size is set. The caller should use qnes_load_rom_inplace
 * on PSRAM_BASE directly (no re-read from SD needed).
 *
 * @param out_rom_size  Receives the size of the loaded ROM in bytes
 * @return true if a ROM was selected and loaded, false if no ROMs found
 */
bool rom_selector_show(long *out_rom_size);

#ifdef __cplusplus
}
#endif

#endif /* ROM_SELECTOR_H */

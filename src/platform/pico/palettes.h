/*
 * FRANK NES - Built-in NES color palettes
 *
 * Each palette is a 64-entry RGB888 table covering the 64 base NES colors.
 * The emphasis modes (4 more copies for 256 entries total) are synthesized
 * at lookup time by the QuickNES core — we only override the base colors.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PALETTES_H
#define PALETTES_H

#include <stdint.h>

typedef struct { uint8_t r, g, b; } palette_rgb_t;

/* 64 base colors — QuickNES default. Exposed here so callers can restore. */
extern const palette_rgb_t palette_nes_default[64];

/* FirebrandX's "Neutral" palette — widely used reference for modern NTSC */
extern const palette_rgb_t palette_firebrandx[64];

/* Wavebeam — calibrated against a real CRT */
extern const palette_rgb_t palette_wavebeam[64];

/* Composite direct — Bisqwit's pseudo-composite decode */
extern const palette_rgb_t palette_composite[64];

/* User-loaded palette. Populated by palette_load_custom() from SD; zeroed
 * (and `palette_custom_loaded` false) before that. Selecting PALETTE_CUSTOM
 * when this hasn't been loaded falls back to palette_nes_default. */
extern palette_rgb_t palette_custom[64];
extern int palette_custom_loaded;

/* Load a custom palette from a .pal file on SD. Accepts both the common
 * 192-byte (64 RGB triples) and 1536-byte (512 RGB triples — full NES
 * master table) formats. Only the first 64 base colors are used.
 * Returns 0 on success, non-zero if the file is missing or malformed. */
int palette_load_custom(const char *path);

#endif /* PALETTES_H */

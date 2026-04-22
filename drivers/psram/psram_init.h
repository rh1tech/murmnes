/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"

// Initialize PSRAM with compile-time frequency
void psram_init(uint cs_pin);

// Initialize PSRAM with runtime-specified frequency (in MHz)
void psram_init_with_freq(uint cs_pin, uint16_t max_freq_mhz);

#endif

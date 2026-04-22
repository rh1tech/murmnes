/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#ifndef PS2KBD_WRAPPER_H
#define PS2KBD_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NES key codes returned by ps2kbd_get_key()
#define NES_KEY_UP     0x01
#define NES_KEY_DOWN   0x02
#define NES_KEY_LEFT   0x03
#define NES_KEY_RIGHT  0x04
#define NES_KEY_A      0x05
#define NES_KEY_B      0x06
#define NES_KEY_SELECT 0x07
#define NES_KEY_START  0x08
#define NES_KEY_ESC    0x09
#define NES_KEY_F12    0x0A
#define NES_KEY_F11    0x0B
#define NES_KEY_PGUP   0x0C
#define NES_KEY_PGDN   0x0D
#define NES_KEY_HOME   0x0E
#define NES_KEY_END    0x0F
#define NES_KEY_F3     0x10

// Keyboard state bits for ps2kbd_get_state()
#define KBD_STATE_UP     (1 << 0)
#define KBD_STATE_DOWN   (1 << 1)
#define KBD_STATE_LEFT   (1 << 2)
#define KBD_STATE_RIGHT  (1 << 3)
#define KBD_STATE_A      (1 << 4)
#define KBD_STATE_B      (1 << 5)
#define KBD_STATE_SELECT (1 << 6)
#define KBD_STATE_START  (1 << 7)
#define KBD_STATE_ESC    (1 << 8)
#define KBD_STATE_F12    (1 << 9)
#define KBD_STATE_F11    (1 << 10)
#define KBD_STATE_PGUP   (1 << 11)
#define KBD_STATE_PGDN   (1 << 12)
#define KBD_STATE_HOME   (1 << 13)
#define KBD_STATE_END    (1 << 14)
#define KBD_STATE_F3     (1 << 15)

void ps2kbd_init(void);
void ps2kbd_tick(void);
int ps2kbd_get_key(int* pressed, unsigned char* key);
uint16_t ps2kbd_get_state(void);
int ps2kbd_get_raw_char(void);

#ifdef __cplusplus
}
#endif

#endif

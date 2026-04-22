/*
 * FRANK NES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: MIT
 */

#include "board_config.h"
#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"

// Simple ring buffer for key events (avoids std::queue to save RAM)
#define EVENT_QUEUE_SIZE 16

struct KeyEvent {
    uint8_t pressed;
    uint8_t key;
};

static KeyEvent event_queue[EVENT_QUEUE_SIZE];
static volatile uint8_t queue_head = 0;  // Next write position
static volatile uint8_t queue_tail = 0;  // Next read position

static inline bool queue_empty(void) {
    return queue_head == queue_tail;
}

static inline bool queue_full(void) {
    return ((queue_head + 1) & (EVENT_QUEUE_SIZE - 1)) == queue_tail;
}

static void queue_push(uint8_t pressed, uint8_t key) {
    if (!queue_full()) {
        event_queue[queue_head].pressed = pressed;
        event_queue[queue_head].key = key;
        queue_head = (queue_head + 1) & (EVENT_QUEUE_SIZE - 1);
    }
}

static bool queue_pop(uint8_t* pressed, uint8_t* key) {
    if (queue_empty()) return false;
    *pressed = event_queue[queue_tail].pressed;
    *key = event_queue[queue_tail].key;
    queue_tail = (queue_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    return true;
}

// Raw character ring buffer for text input (search dialog)
#define RAW_CHAR_QUEUE_SIZE 16
static uint8_t raw_char_queue[RAW_CHAR_QUEUE_SIZE];
static volatile uint8_t raw_char_head = 0;
static volatile uint8_t raw_char_tail = 0;

static void raw_char_push(uint8_t ch) {
    uint8_t next = (raw_char_head + 1) & (RAW_CHAR_QUEUE_SIZE - 1);
    if (next != raw_char_tail) {
        raw_char_queue[raw_char_head] = ch;
        raw_char_head = next;
    }
}

static int raw_char_pop(void) {
    if (raw_char_head == raw_char_tail) return -1;
    uint8_t ch = raw_char_queue[raw_char_tail];
    raw_char_tail = (raw_char_tail + 1) & (RAW_CHAR_QUEUE_SIZE - 1);
    return ch;
}

static uint8_t hid_to_ascii(uint8_t code, uint8_t modifier) {
    bool shift = (modifier & 0x22) != 0;
    if (code >= 0x04 && code <= 0x1D) {
        return shift ? ('A' + (code - 0x04)) : ('a' + (code - 0x04));
    }
    if (code >= 0x1E && code <= 0x26) {
        return '1' + (code - 0x1E);
    }
    if (code == 0x27) return '0';
    if (code == 0x2C) return ' ';
    if (code == 0x2A) return '\b';
    return 0;
}

// HID to NES key mapping
// Key mapping:
//   Arrow keys -> D-pad (Up/Down/Left/Right)
//   Z, X       -> NES B, A buttons
//   Space      -> Select
//   Enter      -> Start
//   ESC        -> Settings menu / Back
// Returns 0 if no mapping
static unsigned char hid_to_nes(uint8_t code) {
    switch (code) {
        // Arrow keys -> D-pad
        case 0x52: return NES_KEY_UP;     // Up arrow
        case 0x51: return NES_KEY_DOWN;   // Down arrow
        case 0x50: return NES_KEY_LEFT;   // Left arrow
        case 0x4F: return NES_KEY_RIGHT;  // Right arrow

        // Z = B, X = A (standard NES keyboard convention)
        case 0x1D: return NES_KEY_B;      // Z key
        case 0x1B: return NES_KEY_A;      // X key

        // Start = Enter or Keypad Enter
        case 0x28: return NES_KEY_START;  // Enter
        case 0x58: return NES_KEY_START;  // Keypad Enter

        // Select = Space
        case 0x2C: return NES_KEY_SELECT; // Space

        // ESC = Settings menu / Back
        case 0x29: return NES_KEY_ESC;    // Escape

        // F3 = Search
        case 0x3C: return NES_KEY_F3;     // F3

        // F11 = File browser
        case 0x44: return NES_KEY_F11;    // F11

        // F12 = Settings menu
        case 0x45: return NES_KEY_F12;    // F12

        // Navigation keys
        case 0x4B: return NES_KEY_PGUP;   // Page Up
        case 0x4E: return NES_KEY_PGDN;   // Page Down
        case 0x4A: return NES_KEY_HOME;   // Home
        case 0x4D: return NES_KEY_END;    // End

        default: return 0;
    }
}

static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    // Check keys - new key presses
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unsigned char k = hid_to_nes(curr->keycode[i]);
                if (k) queue_push(1, k);
                uint8_t ch = hid_to_ascii(curr->keycode[i], curr->modifier);
                if (ch) raw_char_push(ch);
            }
        }
    }

    // Check keys - key releases
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unsigned char k = hid_to_nes(prev->keycode[i]);
                if (k) queue_push(0, k);
            }
        }
    }
}

static Ps2Kbd_Mrmltr* kbd = nullptr;
static volatile uint16_t g_kbd_state = 0;

static uint16_t key_to_state_bit(uint8_t key) {
    switch (key) {
        case NES_KEY_UP:     return KBD_STATE_UP;
        case NES_KEY_DOWN:   return KBD_STATE_DOWN;
        case NES_KEY_LEFT:   return KBD_STATE_LEFT;
        case NES_KEY_RIGHT:  return KBD_STATE_RIGHT;
        case NES_KEY_A:      return KBD_STATE_A;
        case NES_KEY_B:      return KBD_STATE_B;
        case NES_KEY_SELECT: return KBD_STATE_SELECT;
        case NES_KEY_START:  return KBD_STATE_START;
        case NES_KEY_ESC:    return KBD_STATE_ESC;
        case NES_KEY_F12:    return KBD_STATE_F12;
        case NES_KEY_F11:    return KBD_STATE_F11;
        case NES_KEY_PGUP:   return KBD_STATE_PGUP;
        case NES_KEY_PGDN:   return KBD_STATE_PGDN;
        case NES_KEY_HOME:   return KBD_STATE_HOME;
        case NES_KEY_END:    return KBD_STATE_END;
        case NES_KEY_F3:     return KBD_STATE_F3;
        default: return 0;
    }
}

extern "C" void ps2kbd_init(void) {
#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO)
    kbd = new Ps2Kbd_Mrmltr(pio1, PS2_PIN_CLK, key_handler);
#else
    kbd = new Ps2Kbd_Mrmltr(pio0, PS2_PIN_CLK, key_handler);
#endif
    kbd->init_gpio();
    g_kbd_state = 0;
}

extern "C" void ps2kbd_tick(void) {
    if (kbd) kbd->tick();

    uint8_t p, k;
    while (queue_pop(&p, &k)) {
        uint16_t bit = key_to_state_bit(k);
        if (bit) {
            if (p) {
                g_kbd_state |= bit;
            } else {
                g_kbd_state &= ~bit;
            }
        }
    }
}

extern "C" int ps2kbd_get_key(int* pressed, unsigned char* key) {
    (void)pressed;
    (void)key;
    return 0;
}

extern "C" uint16_t ps2kbd_get_state(void) {
    return g_kbd_state;
}

extern "C" int ps2kbd_get_raw_char(void) {
    return raw_char_pop();
}

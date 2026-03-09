#include "nespad.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

/*
 * IRQ-triggered NES/SNES gamepad reader via PIO.
 * Based on pico-infonesPlus M1/M2 variant (shared latch/clock).
 *
 * The PIO SM idles at "irq wait 0" between reads — zero bus activity
 * until nespad_read() clears the IRQ to trigger a read cycle (~100µs).
 * This avoids continuous PIO traffic that can disrupt HSTX HDMI output.
 */

/* PIO program: M1/M2 variant (shared latch+clock, single data pin per SM)
 * Reads 8 bits (NES) with right-shift autopush.
 * Sideset = CLK pin (1 bit). Set = LATCH pin. In = DATA pin.
 *
 * .wrap_target
 *   irq    wait 0      side 1      ; idle with CLK HIGH, wait for CPU
 *   set    pins, 1     side 1 [10] ; LATCH HIGH, 11µs pulse
 *   set    x, 7        side 1      ; 8 buttons to read
 *   set    pins, 0     side 0      ; LATCH LOW, CLK LOW — first bit available
 *   in     pins, 1     side 0 [4]  ; read data bit, CLK LOW
 *   set    pins, 0     side 1 [4]  ; CLK HIGH (rising edge advances register)
 *   jmp    x--, 3      side 1      ; loop (jumps to "set pins,0 side 0")
 * .wrap
 */
static const uint16_t nespad_program_instructions[] = {
    0xD020, /*  0: irq    wait 0          side 1       */
    0xFA01, /*  1: set    pins, 1         side 1 [10]  */
    0xF027, /*  2: set    x, 7            side 1       */
    0xE000, /*  3: set    pins, 0         side 0       */
    0x4401, /*  4: in     pins, 1         side 0 [4]   */
    0xF400, /*  5: set    pins, 0         side 1 [4]   */
    0x1043, /*  6: jmp    x--, 3          side 1       */
};

#define NESPAD_WRAP_TARGET 0
#define NESPAD_WRAP        6

static const struct pio_program nespad_program = {
    .instructions = nespad_program_instructions,
    .length = 7,
    .origin = -1,
};

static PIO pio = pio1;
static int8_t sm = -1;
static bool pad_initialized = false;

uint32_t nespad_state = 0;
uint32_t nespad_state2 = 0;

bool nespad_begin(uint32_t cpu_khz, uint8_t clkPin, uint8_t dataPin,
                  uint8_t latPin)
{
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0)
        return false;
    if (!pio_can_add_program(pio, &nespad_program)) {
        pio_sm_unclaim(pio, sm);
        sm = -1;
        return false;
    }

    uint offset = pio_add_program(pio, &nespad_program);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + NESPAD_WRAP_TARGET, offset + NESPAD_WRAP);
    sm_config_set_sideset(&c, 1, false, false);

    sm_config_set_sideset_pins(&c, clkPin);
    sm_config_set_in_pins(&c, dataPin);
    sm_config_set_set_pins(&c, latPin, 1);

    pio_gpio_init(pio, clkPin);
    pio_gpio_init(pio, dataPin);
    pio_gpio_init(pio, latPin);

    gpio_set_pulls(dataPin, true, false);

    pio_sm_set_pindirs_with_mask(pio, sm,
                                  (1u << clkPin) | (1u << latPin),
                                  (1u << clkPin) | (1u << dataPin) |
                                      (1u << latPin));

    /* Right-shift, autopush at 8 bits */
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_clkdiv_int_frac(&c, cpu_khz / 1000, 0); /* 1 MHz */

    pio_set_irq0_source_enabled(pio, (enum pio_interrupt_source)(pis_interrupt0 + sm), false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    pad_initialized = true;
    return true;
}

/*
 * Read NES gamepad state. Triggers PIO read, blocks ~100µs for result.
 * Right-shift autopush at 8 bits: first bit (A) lands at LSB.
 * Result after >>24 ^ 0xFF: 0x01=A, 0x02=B, 0x04=Sel, 0x08=Start,
 * 0x10=Up, 0x20=Down, 0x40=Left, 0x80=Right.
 */
void nespad_read()
{
    if (!pad_initialized || sm < 0)
        return;

    /* Trigger PIO read by clearing IRQ 0 */
    pio_interrupt_clear(pio, 0);

    /* Block until PIO pushes 8-bit result to FIFO (~100µs) */
    uint32_t raw = pio_sm_get_blocking(pio, sm);

    /* 8-bit result is in upper byte (right-shift autopush at 8 bits) */
    uint8_t buttons = (raw >> 24) ^ 0xFF; /* invert: pressed = 1 */

    /* Convert to DPAD_* format used by nespad.h */
    uint32_t state = 0;
    if (buttons & 0x01) state |= DPAD_A;
    if (buttons & 0x02) state |= DPAD_B;
    if (buttons & 0x04) state |= DPAD_SELECT;
    if (buttons & 0x08) state |= DPAD_START;
    if (buttons & 0x10) state |= DPAD_UP;
    if (buttons & 0x20) state |= DPAD_DOWN;
    if (buttons & 0x40) state |= DPAD_LEFT;
    if (buttons & 0x80) state |= DPAD_RIGHT;

    nespad_state = state;
    nespad_state2 = 0; /* No Joy2 in single-pin mode */
}

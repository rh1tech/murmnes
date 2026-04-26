/*
 * NES/SNES gamepad driver via PIO
 * Based on pico-infonesPlus by shuichitakano and fhoedemakers
 * https://github.com/fhoedemakers/pico-infonesPlus
 * SPDX-License-Identifier: MIT
 *
 * Fork maintained as part of FRANK NES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 */

#include "nespad.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

/*
 * IRQ-triggered NES/SNES gamepad reader via PIO.
 * Based on pico-infonesPlus M1/M2 variant (shared latch/clock).
 *
 * For dual-port boards (two NES controllers), two state machines run the
 * same program on the same PIO. They share CLK + LATCH (both SMs drive the
 * pins identically and in lockstep) and each SM reads its own DATA pin.
 *
 * The PIO SM idles at "irq wait 0" between reads — zero bus activity
 * until nespad_read_start() clears the IRQ to trigger a read cycle (~100µs).
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
static int8_t sm1 = -1;
static int8_t sm2 = -1;
static bool pad_initialized = false;

uint32_t nespad_state = 0;
uint32_t nespad_state2 = 0;

/* Configure one SM to run nespad_program with the given data pin.
 * Both SMs share clkPin (sideset) and latPin (set). */
static bool configure_sm(uint sm, uint offset, uint clkPin, uint dataPin,
                         uint latPin, uint32_t cpu_khz)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + NESPAD_WRAP_TARGET, offset + NESPAD_WRAP);
    sm_config_set_sideset(&c, 1, false, false);

    sm_config_set_sideset_pins(&c, clkPin);
    sm_config_set_in_pins(&c, dataPin);
    sm_config_set_set_pins(&c, latPin, 1);

    pio_gpio_init(pio, dataPin);
    gpio_set_pulls(dataPin, true, false);

    /* CLK and LATCH are driven by both SMs identically; pindirs for those
     * are set per-SM so each SM owns output enable on the shared pins. */
    pio_sm_set_pindirs_with_mask(pio, sm,
                                  (1u << clkPin) | (1u << latPin),
                                  (1u << clkPin) | (1u << dataPin) |
                                      (1u << latPin));

    /* Right-shift, autopush at 8 bits */
    sm_config_set_in_shift(&c, true, true, 8);
    sm_config_set_clkdiv_int_frac(&c, cpu_khz / 1000, 0); /* 1 MHz */

    pio_sm_clear_fifos(pio, sm);
    pio_sm_init(pio, sm, offset, &c);
    return true;
}

bool nespad_begin(uint32_t cpu_khz, uint8_t clkPin, uint8_t dataPin,
                  uint8_t data2Pin, uint8_t latPin)
{
    const bool have_pad2 = (data2Pin != NESPAD_DATA_PIN_NONE);

    sm1 = pio_claim_unused_sm(pio, false);
    if (sm1 < 0)
        return false;
    if (have_pad2) {
        sm2 = pio_claim_unused_sm(pio, false);
        if (sm2 < 0) {
            pio_sm_unclaim(pio, sm1);
            sm1 = -1;
            return false;
        }
    }
    if (!pio_can_add_program(pio, &nespad_program)) {
        pio_sm_unclaim(pio, sm1);
        sm1 = -1;
        if (sm2 >= 0) {
            pio_sm_unclaim(pio, sm2);
            sm2 = -1;
        }
        return false;
    }

    uint offset = pio_add_program(pio, &nespad_program);

    /* Shared CLK + LATCH are initialized once. */
    pio_gpio_init(pio, clkPin);
    pio_gpio_init(pio, latPin);

    configure_sm((uint)sm1, offset, clkPin, dataPin, latPin, cpu_khz);
    if (have_pad2)
        configure_sm((uint)sm2, offset, clkPin, data2Pin, latPin, cpu_khz);

    /* Disable SM IRQ bubbling to CPU — we only use "irq wait 0" as a gate. */
    pio_set_irq0_source_enabled(pio, (enum pio_interrupt_source)(pis_interrupt0 + sm1), false);
    if (have_pad2)
        pio_set_irq0_source_enabled(pio, (enum pio_interrupt_source)(pis_interrupt0 + sm2), false);

    /* Start both SMs in lockstep so they agree on CLK/LATCH edges. */
    uint32_t sm_mask = 1u << sm1;
    if (have_pad2) sm_mask |= 1u << sm2;
    pio_enable_sm_mask_in_sync(pio, sm_mask);

    pad_initialized = true;
    return true;
}

/* Trigger PIO read (non-blocking). Result ready in ~100µs via nespad_read_finish().
 * Clearing IRQ 0 releases both SMs simultaneously. */
void nespad_read_start(void)
{
    if (!pad_initialized || sm1 < 0)
        return;
    pio_interrupt_clear(pio, 0);
}

/* Decode raw 8-bit button byte (shifted into upper byte by autopush)
 * into a DPAD_* bitmask. 0xFF (all pressed) → treat as disconnected. */
static uint32_t decode_buttons(uint32_t raw)
{
    uint8_t buttons = (raw >> 24) ^ 0xFF;
    if (buttons == 0xFF) return 0;

    uint32_t state = 0;
    if (buttons & 0x01) state |= DPAD_A;
    if (buttons & 0x02) state |= DPAD_B;
    if (buttons & 0x04) state |= DPAD_SELECT;
    if (buttons & 0x08) state |= DPAD_START;
    if (buttons & 0x10) state |= DPAD_UP;
    if (buttons & 0x20) state |= DPAD_DOWN;
    if (buttons & 0x40) state |= DPAD_LEFT;
    if (buttons & 0x80) state |= DPAD_RIGHT;
    return state;
}

/* Drain one SM's RX FIFO with a short timeout. Returns true if a value was
 * consumed into *out; false on timeout (caller keeps previous state). */
static bool drain_sm(uint sm, uint32_t *out)
{
    for (int i = 0; i < 5000 && pio_sm_is_rx_fifo_empty(pio, sm); i++)
        tight_loop_contents();
    if (pio_sm_is_rx_fifo_empty(pio, sm))
        return false;
    *out = pio->rxf[sm];
    return true;
}

/* Wait for PIO read to complete and update nespad_state[2].
 * Call ~100µs+ after nespad_read_start(). If called later, returns instantly. */
void nespad_read_finish(void)
{
    if (!pad_initialized || sm1 < 0)
        return;

    uint32_t raw = 0;
    if (drain_sm((uint)sm1, &raw))
        nespad_state = decode_buttons(raw);

    if (sm2 >= 0) {
        raw = 0;
        if (drain_sm((uint)sm2, &raw))
            nespad_state2 = decode_buttons(raw);
    } else {
        nespad_state2 = 0;
    }
}

/* Convenience: trigger + block. Use start/finish for overlapped reads. */
void nespad_read(void)
{
    nespad_read_start();
    nespad_read_finish();
}

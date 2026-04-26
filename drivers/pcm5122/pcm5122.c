/*
 * FRANK NES - PCM5122 I2C control driver
 * SPDX-License-Identifier: MIT
 */

#include "pcm5122.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>

/* The Waveshare board ties ADR1/ADR2 such that the DAC answers at 0x4D on
 * most revisions. Older variants (and some clones) come up at 0x4C. We
 * probe both and keep whichever one acknowledges. */
#define PCM5122_I2C_BUS    i2c1
#define PCM5122_I2C_BAUD   100000u

#define PCM5122_REG_PAGE_SELECT 0x00
#define PCM5122_REG_RESET       0x01
#define PCM5122_REG_POWER       0x02
#define PCM5122_REG_MUTE        0x03
#define PCM5122_REG_PLL_SRC     0x0D
#define PCM5122_REG_DAC_CLK_SRC 0x0E
#define PCM5122_REG_IGNORE_ERR  0x25

static bool reg_write(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    int n = i2c_write_blocking(PCM5122_I2C_BUS, addr, buf, 2, false);
    return n == 2;
}

static bool probe_addr(uint8_t addr) {
    /* Page select to 0 — the simplest write that every variant accepts and
     * that we need anyway before touching any other register. */
    return reg_write(addr, PCM5122_REG_PAGE_SELECT, 0x00);
}

bool pcm5122_init(unsigned int sda_pin, unsigned int scl_pin) {
    i2c_init(PCM5122_I2C_BUS, PCM5122_I2C_BAUD);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);

    /* Give the DAC a moment after power-up / pin-mux before talking to it. */
    sleep_ms(10);

    uint8_t addr = 0x4D;
    if (!probe_addr(addr)) {
        addr = 0x4C;
        if (!probe_addr(addr)) {
            printf("PCM5122: no ACK on 0x4C/0x4D\n");
            return false;
        }
    }
    printf("PCM5122: detected at 0x%02X\n", addr);

    /* Enter standby before reconfiguring clocks. */
    reg_write(addr, PCM5122_REG_POWER, 0x10);
    sleep_ms(1);

    /* Reset registers + modules. */
    reg_write(addr, PCM5122_REG_RESET, 0x11);
    sleep_ms(1);
    reg_write(addr, PCM5122_REG_RESET, 0x00);

    /* PLL reference = BCK (the Pico doesn't provide MCLK). DAC clock source
     * tracks the PLL output. */
    reg_write(addr, PCM5122_REG_PLL_SRC,     0x10);
    reg_write(addr, PCM5122_REG_DAC_CLK_SRC, 0x10);

    /* Ignore SCK/BCK detection errors — we only feed BCK/LRCK. */
    reg_write(addr, PCM5122_REG_IGNORE_ERR, 0x08);

    /* Unmute both channels. */
    reg_write(addr, PCM5122_REG_MUTE, 0x00);

    /* Release standby and powerdown — DAC will lock once BCK starts. */
    reg_write(addr, PCM5122_REG_POWER, 0x00);
    sleep_ms(5);

    return true;
}

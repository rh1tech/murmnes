/*
 * FRANK NES - NES Emulator for RP2350
 * Board configuration: z0 (RP2350-PiZero / Waveshare)
 *
 * PIO-HDMI only (HDMI_BASE_PIN=32). No TV/VGA.
 * Uses waveshare_rp2350_pizero board definition for Pico SDK.
 */

#ifndef BOARD_Z0_H
#define BOARD_Z0_H

/* PS/2 Keyboard */
#define KBD_CLOCK_PIN 14
#define KBD_DATA_PIN  15
#define PS2_PIN_CLK   14
#define PS2_PIN_DATA  15

/* SD Card (hardware SPI1 — pins above 29 require SPI1) */
#define SDCARD_SPI_BUS       spi1
#define SDCARD_PIN_SPI0_SCK  30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40
#define SDCARD_PIN_SPI0_CS   43

/* NES Gamepad */
#define NESPAD_CLK_PIN    4
#define NESPAD_LATCH_PIN  5
#define NESPAD_DATA_PIN   7
#define NESPAD_DATA2_PIN  8

/* I2S Audio */
#define HAS_I2S 1
#define I2S_DATA_PIN       10
#define I2S_CLOCK_PIN_BASE 11

/* PWM Audio */
#define PWM_PIN0 10
#define PWM_PIN1 11

/* Waveshare PCM5122 Audio Board
 * I2S data/clock layout must satisfy the existing audio_i2s.pio program:
 * clock_pin_base = BCK (lower side-set bit), clock_pin_base+1 = LCK. BCK=18
 * and LCK=19 are consecutive so the shared i2s_audio driver drives them
 * directly. The DAC is configured once via I2C1 on GP2/GP3 to release
 * standby and run its PLL from BCK (no MCLK is provided by the Pico). */
#define HAS_PCM5122 1
#define PCM5122_I2S_DATA       21
#define PCM5122_I2S_CLOCK_BASE 18  /* BCK=18, LCK=19 */
#define PCM5122_I2C_SDA        2
#define PCM5122_I2C_SCL        3

/* No TV/VGA */

/* No UART logging */
#define NO_UART_LOGGING 1

/* PSRAM built-in on GP47 */
#define PSRAM_CS_PIN_RP2350B 47
#define PSRAM_CS_PIN_RP2350A 47

/* Video: PIO-HDMI only (GPIO 32-39) */
#define HDMI_BASE_PIN 32
#define VGA_BASE_PIN  32

#endif /* BOARD_Z0_H */

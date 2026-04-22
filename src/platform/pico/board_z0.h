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
#define NESPAD_CLK_PIN   4
#define NESPAD_LATCH_PIN 5
#define NESPAD_DATA_PIN  7

/* I2S Audio */
#define HAS_I2S 1
#define I2S_DATA_PIN       21
#define I2S_CLOCK_PIN_BASE 18

/* PWM Audio */
#define PWM_PIN0 10
#define PWM_PIN1 11

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

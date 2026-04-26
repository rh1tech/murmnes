/*
 * FRANK NES - NES Emulator for RP2350
 * Board configuration: M2 (Murmulator 2.0)
 */

#ifndef BOARD_M2_H
#define BOARD_M2_H

/* PS/2 Keyboard */
#define KBD_CLOCK_PIN 2
#define KBD_DATA_PIN  3
#define PS2_PIN_CLK   2
#define PS2_PIN_DATA  3

/* SD Card (PIO-SPI) */
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5

/* NES Gamepad */
#define NESPAD_CLK_PIN    20
#define NESPAD_LATCH_PIN  21
#define NESPAD_DATA_PIN   26
#define NESPAD_DATA2_PIN  27

/* I2S Audio (optional external DAC) */
#define HAS_I2S 1
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

/* PWM Audio (RC low-pass filter on pins) */
#define PWM_PIN0 10
#define PWM_PIN1 11

/* Composite TV output */
#define HAS_TV 1
#define TV_BASE_PIN 12

/* PSRAM (QMI auto-detect CS pin at runtime) */
#define PSRAM_CS_PIN_RP2350B 47
#define PSRAM_CS_PIN_RP2350A 8

/* Video: HSTX capable (GPIO 12-19) */
#define HAS_HSTX 1
#define HDMI_BASE_PIN 12
#define VGA_BASE_PIN  12

#endif /* BOARD_M2_H */

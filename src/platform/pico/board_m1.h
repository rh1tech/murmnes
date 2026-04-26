/*
 * FRANK NES - NES Emulator for RP2350
 * Board configuration: M1 (Murmulator 1.x)
 *
 * Different pinout from M2. PIO-HDMI/VGA at GPIO 6-13.
 * No HSTX (HDMI_BASE_PIN != 12).
 */

#ifndef BOARD_M1_H
#define BOARD_M1_H

/* PS/2 Keyboard */
#define KBD_CLOCK_PIN 0
#define KBD_DATA_PIN  1
#define PS2_PIN_CLK   0
#define PS2_PIN_DATA  1

/* SD Card (PIO-SPI) */
#define SDCARD_PIN_SPI0_SCK  2
#define SDCARD_PIN_SPI0_MOSI 3
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5

/* NES Gamepad */
#define NESPAD_CLK_PIN    14
#define NESPAD_LATCH_PIN  15
#define NESPAD_DATA_PIN   16
#define NESPAD_DATA2_PIN  17

/* I2S Audio */
#define HAS_I2S 1
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

/* PWM Audio */
#define PWM_PIN0 26
#define PWM_PIN1 27

/* Composite TV output */
#define HAS_TV 1
#define TV_BASE_PIN 6

/* No UART logging (GPIO 0 is KBD) */
#define NO_UART_LOGGING 1

/* PSRAM (QMI auto-detect CS pin at runtime) */
#define PSRAM_CS_PIN_RP2350B 47
#define PSRAM_CS_PIN_RP2350A 8

/* Video: PIO-HDMI/VGA only (GPIO 6-13, no HSTX) */
#define HDMI_BASE_PIN 6
#define VGA_BASE_PIN  6

#endif /* BOARD_M1_H */

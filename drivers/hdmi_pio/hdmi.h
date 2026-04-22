/*
 * PIO-based HDMI/VGA driver for frank-nes
 * Adapted from murmsnes HDMI driver by Mikhail Matveev
 * SPDX-License-Identifier: MIT
 *
 * Supports autodetection: testPins() probes GPIO12/13 to determine
 * whether VGA or HDMI is connected. graphics_init() dispatches
 * to VGA or HDMI accordingly based on the global SELECT_VGA flag.
 */

#pragma once
#ifndef HDMI_PIO_H_
#define HDMI_PIO_H_

#include "inttypes.h"
#include "stdbool.h"
#include "hardware/dma.h"

#define VIDEO_DMA_IRQ (DMA_IRQ_0)

/* --- HDMI pin configuration --- */

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN (12)
#endif

#define HDMI_PIN_RGB_notBGR (1)
#define HDMI_PIN_invert_diffpairs (1)

#ifndef PIO_VIDEO
#define PIO_VIDEO pio0
#endif
#ifndef PIO_VIDEO_ADDR
#define PIO_VIDEO_ADDR pio0
#endif

#ifndef beginHDMI_PIN_data
#define beginHDMI_PIN_data (HDMI_BASE_PIN+2)
#endif

#ifndef beginHDMI_PIN_clk
#define beginHDMI_PIN_clk (HDMI_BASE_PIN)
#endif

/* --- VGA pin configuration (shared PIO/DMA — only one active) --- */

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN (12)
#endif

#define PIO_VGA pio0
#define VGA_DMA_IRQ (DMA_IRQ_0)

/* --- Common types --- */

typedef struct video_mode_t{
  int h_total;
  int h_width;
  int freq;
  int vgaPxClk;
} video_mode_t;

enum graphics_mode_t {
    TEXTMODE_DEFAULT,
    GRAPHICSMODE_DEFAULT,
};

/* --- VGA/HDMI autodetection --- */

/* Set by main before calling graphics_init() */
extern bool SELECT_VGA;

/* Probe two GPIO pins; returns bitmask.
 * VGA detected when result == 0 or result == 0x1F. */
int testPins(uint32_t pin0, uint32_t pin1);

/* --- Dispatching API (VGA or HDMI based on SELECT_VGA) --- */

/* Call this instead of graphics_init_hdmi() — dispatches automatically */
void graphics_init(void);

/* Call this instead of graphics_set_palette_hdmi() — dispatches automatically */
void graphics_set_palette(const uint8_t i, const uint32_t color888);

/* Call this instead of graphics_set_bgcolor_hdmi() — dispatches automatically */
void graphics_set_bgcolor(const uint32_t color888);

/* --- HDMI-specific API (always available, called internally or when !SELECT_VGA) --- */

void graphics_init_hdmi(void);
void graphics_set_buffer(uint8_t *buffer);
uint8_t* graphics_get_buffer(void);
uint32_t graphics_get_width(void);
uint32_t graphics_get_height(void);
void graphics_set_res(int w, int h);
void graphics_set_palette_hdmi(uint8_t i, uint32_t color888);
uint32_t graphics_get_palette(uint8_t i);
void graphics_restore_sync_colors(void);
void graphics_set_bgcolor_hdmi(uint32_t color888);
void graphics_set_mode(enum graphics_mode_t mode);
void graphics_set_scanlines(bool active);
uint8_t* get_line_buffer(int line);
void graphics_set_shift(int x, int y);

struct video_mode_t graphics_get_video_mode(int mode);

/* VGA-specific setup (only used when SELECT_VGA) */
void graphics_set_offset(const int x, const int y);
void graphics_set_mode_vga(enum graphics_mode_t mode);

/* Global buffer dimensions - set before calling graphics_init() */
extern int graphics_buffer_width;
extern int graphics_buffer_height;

#endif /* HDMI_PIO_H_ */

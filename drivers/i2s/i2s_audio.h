/*
 * FRANK NES - I2S Audio Driver
 * Chained double-buffer DMA via PIO
 * Adapted from murmgenesis audio driver.
 * SPDX-License-Identifier: MIT
 */

#ifndef I2S_AUDIO_H
#define I2S_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2S audio output on PIO0.
 * @param data_pin     GPIO for I2S serial data
 * @param clock_pin_base GPIO for LRCLK (clock_pin_base+1 = BCLK)
 * @param sample_rate  Audio sample rate in Hz (e.g. 44100)
 */
void i2s_audio_init(unsigned int data_pin, unsigned int clock_pin_base, uint32_t sample_rate);

/**
 * Push mono 16-bit samples to I2S output (duplicated to stereo).
 * Blocks briefly if both DMA buffers are in-flight.
 * @param buf   Mono 16-bit PCM samples
 * @param count Number of samples
 */
void i2s_audio_push_samples(const int16_t *buf, int count);

/**
 * Push silence frames to I2S (keeps DMA running without pops).
 * @param count Number of silence samples
 */
void i2s_audio_fill_silence(int count);

/**
 * Shut down I2S audio (stop DMA, disable PIO SM).
 */
void i2s_audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* I2S_AUDIO_H */

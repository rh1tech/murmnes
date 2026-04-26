/*
 * FRANK NES - PWM Audio Driver
 * DMA-paced double-buffered PWM playback via DREQ from a second (pacer) slice.
 * SPDX-License-Identifier: MIT
 */

#ifndef PWM_AUDIO_H
#define PWM_AUDIO_H

#include <stdint.h>
#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the PWM audio path.
 *   pin_l, pin_r: GPIOs wired to the audio PWM slice (may be the same slice
 *                 or two different slices; if they share a slice they share a
 *                 DMA stream, otherwise pin_r is mirrored from pin_l).
 *   sample_rate:  output sample rate in Hz (e.g. 44100).
 * Safe to call multiple times — subsequent calls are no-ops. */
void pwm_audio_init(uint pin_l, uint pin_r, uint32_t sample_rate);

/* Push signed 16-bit mono samples. Non-blocking as long as a DMA buffer is
 * free; blocks briefly if both buffers are still playing. */
void pwm_audio_push_samples(const int16_t *buf, int count);

/* Drop `count` samples' worth of silence at the configured sample rate. */
void pwm_audio_fill_silence(int count);

#ifdef __cplusplus
}
#endif

#endif /* PWM_AUDIO_H */

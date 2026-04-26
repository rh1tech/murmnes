/*
 * FRANK NES - PCM5122 I2C control driver
 *
 * Brings the Waveshare PCM5122 "Audio HAT" out of standby and configures
 * it to derive its internal audio clocks from BCK alone (no MCLK supplied
 * by the host). Audio data itself is streamed over I2S via the shared
 * i2s_audio driver — this module only handles the initial I2C setup.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCM5122_H
#define PCM5122_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure the PCM5122 DAC over I2C.
 *
 * Selects page 0, switches the clock source to BCK, holds a short reset,
 * selects I2S/16-bit, unmutes, and releases the standby power-down bit.
 *
 * @param i2c_sda_pin GPIO for I2C SDA (routed to I2C1 via GPIO function)
 * @param i2c_scl_pin GPIO for I2C SCL (routed to I2C1 via GPIO function)
 * @return true on success, false if no ACK from the DAC
 */
bool pcm5122_init(unsigned int i2c_sda_pin, unsigned int i2c_scl_pin);

#ifdef __cplusplus
}
#endif

#endif /* PCM5122_H */

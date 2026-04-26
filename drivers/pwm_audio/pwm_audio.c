/*
 * FRANK NES - PWM Audio Driver
 *
 * Architecture:
 *   - One PWM slice drives the speaker pin(s) AND paces the DMA. It is
 *     configured so that exactly one WRAP occurs per output sample
 *     period (wrap = sys_clk / sample_rate - 1), giving ~12.5 bits of
 *     amplitude resolution at sys_clk=252 MHz / 44.1 kHz.
 *   - Each PWM WRAP raises DREQ_PWM_WRAPn, which fires a single 32-bit
 *     DMA write into the slice's packed CC register (low16 = CC_A,
 *     high16 = CC_B). The new CC value is latched at the following
 *     wrap (RP2 PWM is double-buffered), so amplitude updates every
 *     sample period without tearing.
 *   - Double-buffered DMA ring: CH_A runs buf 0, chains to CH_B which
 *     runs buf 1, which chains back to CH_A. An IRQ refills whichever
 *     buffer just completed. This mirrors the i2s_audio.c pattern.
 *
 * Why one-pulse-per-sample and not a high-frequency carrier? A carrier
 * significantly above the sample rate (e.g. 61 kHz vs 44.1 kHz) wraps a
 * non-integer number of times per sample period. Some samples then get
 * a single pulse applied, others get two, and the result is audible
 * intermodulation/aliasing distortion. Matching carrier to sample rate
 * eliminates this; the residual 44.1 kHz tone is inaudible and filtered
 * by the RC stage on the board output.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pwm_audio.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

/* PWM wrap is computed at init to place one PWM cycle per sample period,
 * giving an amplitude resolution of sys_clk / sample_rate counts. At
 * 252 MHz / 44.1 kHz that's 5714 ≈ 12.5 bits. Rather than retune per-call
 * we clamp the final CC values against the runtime-derived wrap. */
static uint32_t pwm_wrap = 4095;
static uint32_t pwm_center = 2047;

/* Ring buffer sized so a single chunk fits the largest per-frame sample
 * delivery (Dendy = 882 samples @ 44.1 kHz / 50 fps) with headroom.
 * Per-chunk size is runtime-configurable via pwm_audio_set_frame_rate so
 * consumer pacing matches the emulator (60 fps NTSC, 50 fps Dendy). */
#define DMA_BUFFER_SAMPLES 960
#define DMA_BUFFER_COUNT   2
#define PREROLL_BUFFERS    2

/* IRQ selection:
 *   - HDMI HSTX / HDMI PIO / composite TV video: all claim DMA_IRQ_0.
 *   - DispHSTX VGA: claims DMA_IRQ_1.
 * Pick the IRQ the video driver does NOT own. Mirrors i2s_audio.c. */
#if defined(VGA_HSTX)
  #define PWM_AUDIO_DMA_IRQ DMA_IRQ_0
  #define PWM_AUDIO_INTS    dma_hw->ints0
  #define PWM_AUDIO_IRQ_SET dma_channel_set_irq0_enabled
#else
  #define PWM_AUDIO_DMA_IRQ DMA_IRQ_1
  #define PWM_AUDIO_INTS    dma_hw->ints1
  #define PWM_AUDIO_IRQ_SET dma_channel_set_irq1_enabled
#endif

/* Dedicated DMA channels — picked so they don't clash with HDMI (0,1) or
 * I2S (10,11). */
#define PWM_DMA_CH_A 8
#define PWM_DMA_CH_B 9

/* Packed CC word per sample: low16 = CC_A, high16 = CC_B. Same slice drives
 * both, or a single channel + a mirrored/parked second pin. */
static uint32_t __attribute__((aligned(4))) dma_bufs[DMA_BUFFER_COUNT][DMA_BUFFER_SAMPLES];

static volatile uint32_t bufs_free_mask = 0;
static volatile int preroll_count = 0;
static volatile bool dma_running = false;

static bool initialized = false;
static uint audio_slice = 0;
static bool stereo_same_slice = false;
static uint32_t cached_sample_rate = 0;
static volatile uint32_t dma_xfer_count = 0;

static void __not_in_flash_func(pwm_audio_dma_irq_handler)(void) {
    uint32_t ints = PWM_AUDIO_INTS;
    uint32_t mask = (1u << PWM_DMA_CH_A) | (1u << PWM_DMA_CH_B);
    ints &= mask;
    if (!ints) return;

    if (ints & (1u << PWM_DMA_CH_A)) {
        PWM_AUDIO_INTS = (1u << PWM_DMA_CH_A);
        dma_channel_set_read_addr(PWM_DMA_CH_A, dma_bufs[0], false);
        dma_channel_set_trans_count(PWM_DMA_CH_A, dma_xfer_count, false);
        bufs_free_mask |= 1u;
    }
    if (ints & (1u << PWM_DMA_CH_B)) {
        PWM_AUDIO_INTS = (1u << PWM_DMA_CH_B);
        dma_channel_set_read_addr(PWM_DMA_CH_B, dma_bufs[1], false);
        dma_channel_set_trans_count(PWM_DMA_CH_B, dma_xfer_count, false);
        bufs_free_mask |= 2u;
    }
}

void pwm_audio_init(uint pin_l, uint pin_r, uint32_t sample_rate) {
    if (initialized) return;

    cached_sample_rate = sample_rate;

    /* Compute wrap so the PWM wraps exactly once per sample.
     * wrap_period_cycles = sys_clk / sample_rate, so wrap register = that - 1.
     * clkdiv stays at 1.0 so each PWM count is one sys_clk tick — the finest
     * amplitude resolution available. At 252 MHz / 44.1 kHz → wrap ≈ 5713
     * (~12.5 bits). At 378 MHz → wrap ≈ 8571 (~13 bits). */
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t wrap_plus_one = sys_clk / sample_rate;
    if (wrap_plus_one < 2) wrap_plus_one = 2;
    if (wrap_plus_one > 65536) wrap_plus_one = 65536;
    pwm_wrap = wrap_plus_one - 1;
    pwm_center = pwm_wrap / 2;

    /* Configure audio GPIOs as PWM. */
    gpio_set_function(pin_l, GPIO_FUNC_PWM);
    audio_slice = pwm_gpio_to_slice_num(pin_l);

    uint slice_r = pwm_gpio_to_slice_num(pin_r);
    stereo_same_slice = (slice_r == audio_slice);
    if (stereo_same_slice) {
        gpio_set_function(pin_r, GPIO_FUNC_PWM);
    } else {
        /* Different slice — drive it with its own static mid-level PWM so
         * the pin doesn't dangle. Only one channel actually carries audio. */
        pwm_config cfg2 = pwm_get_default_config();
        pwm_config_set_clkdiv(&cfg2, 1.0f);
        pwm_config_set_wrap(&cfg2, (uint16_t)pwm_wrap);
        gpio_set_function(pin_r, GPIO_FUNC_PWM);
        pwm_init(slice_r, &cfg2, true);
        pwm_set_gpio_level(pin_r, (uint16_t)pwm_center);
    }

    /* Configure audio slice: wraps at sample_rate, drives the pin AND
     * paces DMA via DREQ_PWM_WRAPn. */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, (uint16_t)pwm_wrap);
    pwm_init(audio_slice, &cfg, false);
    pwm_set_both_levels(audio_slice, (uint16_t)pwm_center, (uint16_t)pwm_center);

    /* Pre-fill DMA buffers with mid-level silence */
    uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
    for (int b = 0; b < DMA_BUFFER_COUNT; b++)
        for (int i = 0; i < DMA_BUFFER_SAMPLES; i++)
            dma_bufs[b][i] = silence;

    /* Default to NTSC pacing (60 fps). pwm_audio_set_frame_rate(50) switches
     * the per-chunk transfer count for Dendy. Must match producer rate so
     * the emulator isn't back-pressured and silence-padded per frame. */
    {
        uint32_t n = sample_rate / 60;
        if (n < 1) n = 1;
        if (n > DMA_BUFFER_SAMPLES) n = DMA_BUFFER_SAMPLES;
        dma_xfer_count = n;
    }

    /* Claim DMA channels and configure ping-pong chain */
    dma_channel_abort(PWM_DMA_CH_A);
    dma_channel_abort(PWM_DMA_CH_B);
    while (dma_channel_is_busy(PWM_DMA_CH_A) || dma_channel_is_busy(PWM_DMA_CH_B))
        tight_loop_contents();
    dma_channel_claim(PWM_DMA_CH_A);
    dma_channel_claim(PWM_DMA_CH_B);

    /* DREQ from the audio slice — one DMA transfer per PWM wrap, i.e. per
     * sample period. */
    uint dreq = DREQ_PWM_WRAP0 + audio_slice;

    dma_channel_config cfg_a = dma_channel_get_default_config(PWM_DMA_CH_A);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, dreq);
    channel_config_set_chain_to(&cfg_a, PWM_DMA_CH_B);

    dma_channel_config cfg_b = dma_channel_get_default_config(PWM_DMA_CH_B);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, dreq);
    channel_config_set_chain_to(&cfg_b, PWM_DMA_CH_A);

    /* Destination: packed CC register for the audio slice. Writing 32 bits
     * to &pwm_hw->slice[s].cc updates CC_A (low) and CC_B (high) in one go. */
    volatile uint32_t *cc_addr = &pwm_hw->slice[audio_slice].cc;

    dma_channel_configure(PWM_DMA_CH_A, &cfg_a, (void *)cc_addr,
                          dma_bufs[0], dma_xfer_count, false);
    dma_channel_configure(PWM_DMA_CH_B, &cfg_b, (void *)cc_addr,
                          dma_bufs[1], dma_xfer_count, false);

    PWM_AUDIO_INTS = (1u << PWM_DMA_CH_A) | (1u << PWM_DMA_CH_B);
    irq_set_exclusive_handler(PWM_AUDIO_DMA_IRQ, pwm_audio_dma_irq_handler);
    irq_set_priority(PWM_AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(PWM_AUDIO_DMA_IRQ, true);
    PWM_AUDIO_IRQ_SET(PWM_DMA_CH_A, true);
    PWM_AUDIO_IRQ_SET(PWM_DMA_CH_B, true);

    preroll_count = 0;
    bufs_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    dma_running = false;

    /* Kick the audio slice — DREQ pulses start flowing immediately. The
     * DMA chain stays idle until preroll_count reaches PREROLL_BUFFERS
     * and commit_buf() calls dma_channel_start(). */
    pwm_set_enabled(audio_slice, true);

    initialized = true;

    printf("PWM audio: init slice=%u rate=%lu wrap=%lu (~%u-bit)\n",
        audio_slice, (unsigned long)sample_rate, (unsigned long)pwm_wrap,
        (unsigned)(32u - __builtin_clz(pwm_wrap)));
}

/* Claim an idle DMA buffer (blocks briefly if both are still playing).
 * Returns the buffer index. Must be matched by commit_buf(). */
static uint8_t claim_buf(void) {
    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free = bufs_free_mask;
        if (!dma_running) {
            uint8_t idx = (uint8_t)preroll_count;
            if (idx < DMA_BUFFER_COUNT && (free & (1u << idx))) {
                bufs_free_mask &= ~(1u << idx);
                restore_interrupts(irq_state);
                return idx;
            }
        } else if (free) {
            uint8_t idx = (free & 1u) ? 0 : 1;
            bufs_free_mask &= ~(1u << idx);
            restore_interrupts(irq_state);
            return idx;
        }
        restore_interrupts(irq_state);
        tight_loop_contents();
    }
}

static void commit_buf(uint8_t idx, uint32_t frame_count) {
    /* Pad the rest of the DMA buffer with mid-level silence so we never
     * click on buffer underrun. */
    if (frame_count < dma_xfer_count) {
        uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
        for (uint32_t i = frame_count; i < dma_xfer_count; i++)
            dma_bufs[idx][i] = silence;
    }

    __dmb();

    if (!dma_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            dma_channel_start(PWM_DMA_CH_A);
            dma_running = true;
        }
    }
}

void pwm_audio_push_samples(const int16_t *buf, int count) {
    /* Pack signed 16-bit mono directly into the claimed DMA buffer.
     * Each sample becomes an unsigned level in [0..pwm_wrap] mirrored
     * into low16=CC_A, high16=CC_B so the 32-bit DMA write updates both
     * channels atomically.
     *
     * Amplitude mapping:
     *   Scale the signed 16-bit input into a half-range window around
     *   pwm_center, leaving 6 dB of headroom relative to the rail. This
     *   is cheap (a shift, an add, two saturations) and handles peaky
     *   QuickNES output without clipping the RC output stage on the
     *   board. The window is computed from the runtime pwm_wrap so it
     *   tracks whatever wrap the init settled on for the current
     *   sys_clk / sample_rate. */
    const int32_t center = (int32_t)pwm_center;
    const int32_t max_lvl = (int32_t)pwm_wrap;
    /* Amplitude window around center. QuickNES is hot and the RC output
     * stage on these boards has very little headroom before the downstream
     * amplifier clips. Matching the I2S driver (which attenuates by ÷8 =
     * 18 dB) — use an eighth-range swing so the peak CC excursion is
     * pwm_wrap/8 around center, leaving ~18 dB of margin. Full-range
     * saturation is still clamped below as a last defense. */
    const int32_t swing = (int32_t)(pwm_wrap >> 3); /* eighth-range ≈ 18 dB headroom */

    int pos = 0;

    while (pos < count) {
        int chunk = count - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        uint8_t idx = claim_buf();
        uint32_t *dst = dma_bufs[idx];

        for (int i = 0; i < chunk; i++) {
            int32_t s = buf[pos + i];
            /* s is in [-32768..32767]. Map to [-swing..swing] with a
             * multiply and a shift. Using 32-bit math gives exact
             * scaling regardless of the runtime pwm_wrap. */
            int32_t scaled = (s * swing) >> 15;
            int32_t lvl = scaled + center;
            if (lvl < 0) lvl = 0;
            if (lvl > max_lvl) lvl = max_lvl;
            uint32_t u = (uint32_t)lvl;
            dst[i] = (u << 16) | u;
        }

        commit_buf(idx, (uint32_t)chunk);
        pos += chunk;
    }
}

void pwm_audio_set_frame_rate(int frame_rate) {
    if (frame_rate <= 0) return;
    if (cached_sample_rate == 0) return; /* init hasn't run yet */
    uint32_t n = cached_sample_rate / (uint32_t)frame_rate;
    if (n < 1) n = 1;
    if (n > DMA_BUFFER_SAMPLES) n = DMA_BUFFER_SAMPLES;
    /* Single-word write is atomic on Cortex-M33; the DMA IRQ handler
     * picks it up when arming the next chain-to transfer. */
    dma_xfer_count = n;
}

void pwm_audio_fill_silence(int count) {
    /* Fill directly into the claimed DMA buffer — no separate staging. */
    uint32_t silence = ((uint32_t)pwm_center << 16) | pwm_center;
    int remaining = count;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        uint8_t idx = claim_buf();
        uint32_t *dst = dma_bufs[idx];
        for (int i = 0; i < chunk; i++) dst[i] = silence;

        commit_buf(idx, (uint32_t)chunk);
        remaining -= chunk;
    }
}

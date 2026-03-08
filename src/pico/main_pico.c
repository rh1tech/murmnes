/*
 * murmnes - NES Emulator for RP2350
 * QuickNES core with HDMI output via HSTX
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#if !USB_HID_ENABLED
#include "pico/stdio_usb.h"
#endif

#include "hardware/clocks.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

#include "quicknes.h"

/* 16KB stack in main SRAM — scratch_y (4KB) is too small for QuickNES */
static uint8_t big_stack[16384] __attribute__((aligned(8)));
static void real_main(void);

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

#define NES_WIDTH 256
#define NES_HEIGHT 240
#define SAMPLE_RATE 44100


/* ROM embedded in flash by CMake (objcopy) */
#ifdef HAS_NES_ROM
extern const uint8_t nes_rom_data[];
extern const uint8_t nes_rom_end[];
#endif

/* Palette lookup: NES indexed pixel -> RGB565 */
static uint16_t rgb565_palette[256];

/* Pointer to current frame pixels (set after each emulate_frame) */
static const uint8_t *frame_pixels;
static long frame_pitch;

/* Vsync flag — set by Core 1 DMA ISR, cleared by Core 0 after emulating */
static volatile uint32_t vsync_flag;

static void __not_in_flash("vsync") vsync_cb(void)
{
    vsync_flag = 1;
    __sev(); /* wake Core 0 from WFE */
}

/* NES audio buffer — filled after each emulated frame, drained into HDMI queue.
 * 44100 Hz / 60 fps ≈ 735 mono samples per frame. */
#define NES_AUDIO_BUF_SIZE 1024
static int16_t nes_audio_buf[NES_AUDIO_BUF_SIZE];
static int nes_audio_count = 0;
static int nes_audio_pos = 0;
static int audio_frame_counter = 0;

static bool push_audio_packet(const audio_sample_t *samples)
{
    hstx_packet_t packet;
    int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
    hstx_data_island_t island;
    hstx_encode_data_island(&island, &packet, false, true);
    if (!hstx_di_queue_push(&island))
        return false;
    audio_frame_counter = new_fc;
    return true;
}

static volatile bool audio_busy = false;

static void feed_audio(void)
{
    if (audio_busy) return;
    audio_busy = true;

    /* Push real NES audio samples */
    while (nes_audio_pos + 4 <= nes_audio_count) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            int16_t s = nes_audio_buf[nes_audio_pos + i];
            samples[i].left = s;
            samples[i].right = s;
        }
        if (!push_audio_packet(samples))
            break;
        nes_audio_pos += 4;
    }

    audio_busy = false;
}

static struct repeating_timer audio_timer;
static bool audio_timer_cb(struct repeating_timer *t) { (void)t; feed_audio(); return true; }

static void feed_silence(void)
{
    while (hstx_di_queue_get_level() < 400) {
        audio_sample_t samples[4] = {0};
        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}

/* Build RGB565 palette from QuickNES frame palette + color table */
static void update_palette(void)
{
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();

    if (!pal || !colors)
        return;

    for (int i = 0; i < pal_size && i < 256; i++) {
        int idx = pal[i];
        if (idx < 0 || idx >= 512)
            idx = 0x0F; /* black */
        const qnes_rgb_t *c = &colors[idx];
        rgb565_palette[i] = ((c->r & 0xF8) << 8) | ((c->g & 0xFC) << 3) | (c->b >> 3);
    }
}

/* Scanline callback: convert indexed pixels to RGB565, doubled to 640x480
 * Runs on Core 1 DMA ISR — must be in RAM, no flash access */
void __not_in_flash("scanline") scanline_callback(
    uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    /* Each NES line is doubled vertically (480 / 240 = 2) */
    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;
    const uint8_t *src = frame_pixels + nes_line * frame_pitch;

    /* Left border: 64 pixels = 32 words of black (640 - 256*2 = 128, half = 64) */
    for (int i = 0; i < 32; i++)
        dst[i] = 0;

    /* NES pixels: each pixel doubled horizontally = 512 pixels = 256 words */
    for (int x = 0; x < NES_WIDTH; x++) {
        uint16_t c = rgb565_palette[src[x]];
        dst[32 + x] = c | (c << 16);
    }

    /* Right border */
    for (int i = 288; i < 320; i++)
        dst[i] = 0;
}

/* Generate test pattern when no ROM is loaded */
static uint8_t test_pixels[NES_WIDTH * NES_HEIGHT];

static void generate_test_pattern(void)
{
    /* Simple color bars using direct RGB565 palette */
    static const uint16_t bars[] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    for (int i = 0; i < 256; i++)
        rgb565_palette[i] = bars[i % 8];

    for (int y = 0; y < NES_HEIGHT; y++) {
        for (int x = 0; x < NES_WIDTH; x++) {
            test_pixels[y * NES_WIDTH + x] = (x >> 5) % 8;
        }
    }

    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;
}

static void error_loop(const char *msg)
{
    printf("ERROR: %s\n", msg);
    generate_test_pattern();
    while (1) { feed_audio(); sleep_ms(16); }
}

/* Stack watermark: paint stack with 0xDEADBEEF, later check how much was used */
static void paint_stack(void)
{
    volatile uint32_t sp;
    __asm volatile ("MOV %0, SP" : "=r" (sp));
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t *end = (uint32_t *)(sp - 256);
    while (p < end)
        *p++ = 0xDEADBEEF;
}

static uint32_t check_stack_free(void)
{
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t count = 0;
    while (*p == 0xDEADBEEF) {
        p++;
        count += 4;
    }
    return count;
}

/* HardFault handler — store fault info, pump USB to flush, blink LED */
static volatile uint32_t fault_pc, fault_lr, fault_cfsr, fault_mmfar, fault_bfar;
static volatile bool fault_occurred = false;

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile (
        "MRS r0, MSP\n"
        "B hardfault_handler_c\n"
    );
}

void __attribute__((used)) hardfault_handler_c(uint32_t *stack)
{
    fault_pc = stack[6];
    fault_lr = stack[5];
    fault_cfsr = *(volatile uint32_t *)0xE000ED28;
    fault_mmfar = *(volatile uint32_t *)0xE000ED34;
    fault_bfar = *(volatile uint32_t *)0xE000ED38;
    fault_occurred = true;

    /* Re-enable interrupts so USB can flush */
    __asm volatile ("CPSIE i");

    for (int attempt = 0; attempt < 20; attempt++) {
        printf("!FAULT! PC=%08lx LR=%08lx CFSR=%08lx MMFAR=%08lx BFAR=%08lx stk=%lu\n",
               (unsigned long)fault_pc, (unsigned long)fault_lr,
               (unsigned long)fault_cfsr, (unsigned long)fault_mmfar,
               (unsigned long)fault_bfar, (unsigned long)check_stack_free());
        for (volatile int d = 0; d < 2000000; d++) {} /* busy delay */
    }

    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        for (volatile int d = 0; d < 500000; d++) {}
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        for (volatile int d = 0; d < 500000; d++) {}
    }
}

int main(void)
{
    /* Switch to large stack before doing anything else */
    __asm volatile ("MSR MSP, %0" :: "r" (big_stack + sizeof(big_stack)));
    real_main();
    __builtin_unreachable();
}

static void real_main(void)
{
    set_sys_clock_khz(252000, true);
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    paint_stack();

#if !USB_HID_ENABLED
    /* Wait for USB serial console */
    for (int i = 0; i < 50; i++) {
        if (stdio_usb_connected()) break;
        sleep_ms(100);
    }
#else
    sleep_ms(500);
#endif

    printf("\n=== murmnes (QuickNES) ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    printf("qnes_init...\n");
    if (qnes_init(SAMPLE_RATE) != 0) {
        printf("qnes_init FAILED\n");
        while (1) sleep_ms(100);
    }
    printf("qnes_init OK\n");

#ifdef HAS_NES_ROM
    long rom_size = (long)(nes_rom_end - nes_rom_data);
    printf("qnes_load_rom (%ld bytes)...\n", rom_size);
    if (qnes_load_rom(nes_rom_data, rom_size) != 0) {
        printf("qnes_load_rom FAILED\n");
        while (1) sleep_ms(100);
    }
    printf("ROM loaded OK\n");
#endif

    /* Start HDMI directly — all flash-heavy init is done */
    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;

    hstx_di_queue_init();
    feed_silence();
    video_output_set_vsync_callback(vsync_cb);
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    pico_hdmi_set_audio_sample_rate(SAMPLE_RATE);
    video_output_set_scanline_callback(scanline_callback);
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    /* Timer feeds audio every 4ms during emulation, preventing queue drain */
    add_repeating_timer_ms(-4, audio_timer_cb, NULL, &audio_timer);
    printf("HDMI active\n");

#ifdef HAS_NES_ROM

    uint32_t frame_count = 0;
    while (1) {
        feed_audio();

        for (int wait = 0; !vsync_flag && wait < 20000; wait++) {
            feed_audio();
            __wfe();
        }
        vsync_flag = 0;

        int joypad = (frame_count >= 60 && frame_count < 63) ? 0x08 : 0;
        qnes_emulate_frame(joypad, 0);

        /* Read NES audio generated this frame, preserving leftover samples.
         * Set audio_busy to block timer ISR during buffer swap. */
        audio_busy = true;
        int leftover = nes_audio_count - nes_audio_pos;
        if (leftover > 0)
            memmove(nes_audio_buf, nes_audio_buf + nes_audio_pos, leftover * sizeof(int16_t));
        nes_audio_pos = 0;
        nes_audio_count = leftover + (int)qnes_read_samples(nes_audio_buf + leftover, NES_AUDIO_BUF_SIZE - leftover);
        audio_busy = false;

        update_palette();
        frame_pitch = 272;
        frame_pixels = qnes_get_pixels();

        feed_audio();

        frame_count++;
    }
#else
    printf("No ROM embedded.\n");
    while (1) { sleep_ms(100); }
#endif
}

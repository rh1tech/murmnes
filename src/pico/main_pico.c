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
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "quicknes.h"
#include "psram_init.h"
#include "ff.h"

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

/*
 * Audio pipeline — same architecture as pico-infonesPlus:
 *   Core 0: encode NES audio → push pre-encoded packets to DI queue
 *   Core 1 ISR: pop from DI queue → HDMI output
 *   One shared queue. No intermediate buffers. No background task.
 *
 * All encoding functions are __not_in_flash (SRAM) so Core 0 can
 * encode without flash contention after running QuickNES from flash.
 * DI queue (512 entries = ~43ms) survives the ~10ms emulation gap.
 */
static int audio_frame_counter = 0;

/* Encode mono NES samples into HDMI audio packets and push to DI queue.
 * Carries leftover samples (1-3) between calls to avoid cumulative loss.
 * Runs on Core 0. All called functions are in SRAM (__not_in_flash). */
static int16_t audio_carry[3];
static int audio_carry_count = 0;

static void __not_in_flash("audio") audio_push_samples(const int16_t *buf, int count)
{
    /* Merge carry from previous call */
    int16_t merged[4];
    int pos = 0;

    if (audio_carry_count > 0) {
        for (int i = 0; i < audio_carry_count; i++)
            merged[i] = audio_carry[i];
        int need = 4 - audio_carry_count;
        if (need > count) need = count;
        for (int i = 0; i < need; i++)
            merged[audio_carry_count + i] = buf[i];
        pos = need;
        if (audio_carry_count + need == 4) {
            audio_sample_t samples[4];
            for (int i = 0; i < 4; i++) {
                samples[i].left = merged[i];
                samples[i].right = merged[i];
            }
            hstx_packet_t packet;
            int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
            hstx_data_island_t island;
            hstx_encode_data_island(&island, &packet, false, true);
            if (!hstx_di_queue_push(&island)) {
                /* Queue full — keep carry for next call */
                return;
            }
            audio_frame_counter = new_fc;
        }
        audio_carry_count = 0;
    }

    /* Push full packets */
    while (pos + 4 <= count) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            samples[i].left = buf[pos + i];
            samples[i].right = buf[pos + i];
        }
        hstx_packet_t packet;
        int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island)) {
            // Queue full: just drop the remainder of the packet (we could block, but that stalls NES)
            // Save ONLY what fits, or avoid overrunning audio_carry!
            break;
        }
        audio_frame_counter = new_fc;
        pos += 4;
    }

    /* Save leftover for next call */
    audio_carry_count = count - pos;
    if (audio_carry_count > 3) {
        // We dropped packets due to full queue, clamp carry to avoid buffer overflow
        audio_carry_count = count % 4;
        pos = count - audio_carry_count;
    }
    for (int i = 0; i < audio_carry_count; i++)
        audio_carry[i] = buf[pos + i];
    hstx_di_queue_update_silence(audio_frame_counter);
}

static void audio_fill_silence(int count)
{
    int16_t silence[4] = {0};
    for (int i = 0; i < count / 4; i++) {
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
    while (1) { sleep_ms(16); }
}

/* Try to initialize PSRAM, returns true on success */
static bool psram_available = false;
#define PSRAM_BASE ((volatile uint8_t *)0x11000000)

static bool try_init_psram(void)
{
    /* Try RP2350B pin first, then RP2350A */
    static const uint cs_pins[] = { PSRAM_CS_PIN_RP2350B, PSRAM_CS_PIN_RP2350A };
    for (int i = 0; i < 2; i++) {
        psram_init(cs_pins[i]);
        /* Write/read test */
        PSRAM_BASE[0] = 0xA5;
        PSRAM_BASE[1] = 0x5A;
        __compiler_memory_barrier();
        if (PSRAM_BASE[0] == 0xA5 && PSRAM_BASE[1] == 0x5A) {
            printf("PSRAM detected on CS pin %u\n", cs_pins[i]);
            psram_available = true;
            return true;
        }
    }
    printf("No PSRAM detected\n");
    return false;
}

/* Load first .nes ROM from SD card "nes" directory.
 * Returns ROM data pointer and sets *out_size. NULL on failure.
 * If PSRAM is available, ROM is loaded there; otherwise uses malloc.
 * Caller may free the buffer after qnes_load_rom() since QuickNES copies it. */
static uint8_t *sd_rom_buf = NULL;

static uint8_t *try_load_rom_from_sd(long *out_size)
{
    *out_size = 0;

    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD mount failed (%d)\n", fr);
        return NULL;
    }
    printf("SD card mounted\n");

    DIR dir;
    fr = f_opendir(&dir, "/nes");
    if (fr != FR_OK) {
        printf("SD: /nes directory not found (%d)\n", fr);
        f_unmount("");
        return NULL;
    }

    /* Find first .nes file */
    char filepath[280];
    FILINFO fno;
    bool found = false;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR)
            continue;
        size_t len = strlen(fno.fname);
        if (len >= 4 &&
            (fno.fname[len-4] == '.') &&
            (fno.fname[len-3] == 'n' || fno.fname[len-3] == 'N') &&
            (fno.fname[len-2] == 'e' || fno.fname[len-2] == 'E') &&
            (fno.fname[len-1] == 's' || fno.fname[len-1] == 'S')) {
            snprintf(filepath, sizeof(filepath), "/nes/%s", fno.fname);
            found = true;
            printf("SD: found ROM: %s (%lu bytes)\n", filepath, (unsigned long)fno.fsize);
            break;
        }
    }
    f_closedir(&dir);

    if (!found) {
        printf("SD: no .nes files in /nes\n");
        f_unmount("");
        return NULL;
    }

    FIL fil;
    fr = f_open(&fil, filepath, FA_READ);
    if (fr != FR_OK) {
        printf("SD: failed to open %s (%d)\n", filepath, fr);
        f_unmount("");
        return NULL;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize < 16 || fsize > 2 * 1024 * 1024) {
        printf("SD: invalid ROM size %lu\n", (unsigned long)fsize);
        f_close(&fil);
        f_unmount("");
        return NULL;
    }

    /* Allocate buffer: prefer PSRAM, fall back to SRAM malloc */
    if (psram_available) {
        sd_rom_buf = (uint8_t *)PSRAM_BASE;
        printf("SD: loading ROM into PSRAM\n");
    } else {
        sd_rom_buf = (uint8_t *)malloc(fsize);
        if (!sd_rom_buf) {
            printf("SD: malloc failed for %lu bytes\n", (unsigned long)fsize);
            f_close(&fil);
            f_unmount("");
            return NULL;
        }
        printf("SD: loading ROM into SRAM\n");
    }

    UINT bytes_read;
    fr = f_read(&fil, sd_rom_buf, (UINT)fsize, &bytes_read);
    f_close(&fil);
    f_unmount("");

    if (fr != FR_OK || bytes_read != (UINT)fsize) {
        printf("SD: read error (%d, got %u/%lu)\n", fr, bytes_read, (unsigned long)fsize);
        if (!psram_available)
            free(sd_rom_buf);
        sd_rom_buf = NULL;
        return NULL;
    }

    printf("SD: loaded %lu bytes\n", (unsigned long)fsize);
    *out_size = (long)fsize;
    return sd_rom_buf;
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

/* Configure flash timing for overclocked CPU.
 * Must run from SRAM — flash is being reconfigured. */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz)
{
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000)
        divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000)
        rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000 |
                           rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                           divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void real_main(void)
{
    /* Overclock to 378 MHz — same pattern as murmgenesis */
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    set_flash_timings(378, 88);
    sleep_ms(10);
    if (!set_sys_clock_khz(378000, false))
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

    /* Init PSRAM early — operator new/realloc redirect to PSRAM */
    try_init_psram();

    printf("qnes_init...\n");
    if (qnes_init(SAMPLE_RATE) != 0) {
        printf("qnes_init FAILED\n");
        while (1) sleep_ms(100);
    }
    printf("qnes_init OK\n");

    /* Provide PSRAM for tile cache (large CHR ROMs need ~256KB+) */
    if (psram_available) {
        /* ROM lives at PSRAM_BASE (up to 2MB), tile cache after that */
        void *tc = (void *)(0x11000000 + 2 * 1024 * 1024);
        qnes_set_tile_cache_buf(tc, 1 * 1024 * 1024);
    }

    /* Try loading ROM: SD card first, then flash fallback */
    bool rom_loaded = false;

    {
        long sd_rom_size = 0;
        uint8_t *sd_rom = try_load_rom_from_sd(&sd_rom_size);
        if (sd_rom) {
            printf("qnes_load_rom_inplace from SD (%ld bytes)...\n", sd_rom_size);
            if (qnes_load_rom_inplace(sd_rom, sd_rom_size) == 0) {
                printf("SD ROM loaded OK\n");
                rom_loaded = true;
            } else {
                printf("qnes_load_rom_inplace (SD) FAILED\n");
            }
            /* inplace: PRG/CHR point into sd_rom_buf, do NOT free it */
            if (!rom_loaded && !psram_available && sd_rom_buf)
                free(sd_rom_buf);
        }
    }

#ifdef HAS_NES_ROM
    if (!rom_loaded) {
        long rom_size = (long)(nes_rom_end - nes_rom_data);
        printf("qnes_load_rom_inplace from flash (%ld bytes)...\n", rom_size);
        if (qnes_load_rom_inplace(nes_rom_data, rom_size) == 0) {
            printf("Flash ROM loaded OK\n");
            rom_loaded = true;
        } else {
            printf("qnes_load_rom_inplace (flash) FAILED\n");
        }
    }
#endif

    /* Start HDMI directly — all flash-heavy init is done */
    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;

    hstx_di_queue_init();
    audio_fill_silence(SAMPLE_RATE / 60 * 6); /* pre-fill ~100ms */
    video_output_set_vsync_callback(vsync_cb);
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

    /* Override HSTX clock: PLL_USB reconfigured to 126 MHz.
     * Independent of sys_clk — works at any CPU speed. */
    pll_deinit(pll_usb);
    pll_init(pll_usb, 1, 756000000, 6, 1); /* 12 × 63 / 6 = 126 MHz */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    126000000, 126000000);

    pico_hdmi_set_audio_sample_rate(SAMPLE_RATE);
    video_output_set_scanline_callback(scanline_callback);
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);
    printf("HDMI active\n");

    if (rom_loaded) {
        uint32_t frame_count = 0;
        while (1) {
            for (int wait = 0; !vsync_flag && wait < 20000; wait++)
                __wfe();
            vsync_flag = 0;

            int joypad = (frame_count >= 60 && frame_count < 63) ? 0x08 : 0;
            qnes_emulate_frame(joypad, 0);

            /* Push NES audio into DI queue. No padding — produce only what
             * the NES generates. Carry handles 4-sample boundary. Tiny rate
             * deficit (~1.5 samples/frame) handled by ISR silence fallback. */
            int16_t tmp[1024];
            long n = qnes_read_samples(tmp, 1024);

            if (n > 0) {
                audio_push_samples(tmp, (int)n);
            }

            update_palette();
            frame_pitch = 272;
            frame_pixels = qnes_get_pixels();

            frame_count++;
        }
    } else {
        printf("No ROM loaded (no SD card ROM, no flash ROM).\n");
        generate_test_pattern();
        while (1) { sleep_ms(100); }
    }
}

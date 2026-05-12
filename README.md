# FRANK NES

Official page: **[frank.rh1.tech](https://frank.rh1.tech/)** — hub for all FRANK boards and firmware.

NES (Nintendo Entertainment System) emulator for Raspberry Pi Pico 2 (RP2350) with HDMI/VGA/TV output, SD card ROM browser, NES/SNES gamepad, USB gamepad, PS/2 keyboard, and audio over HDMI, I2S, or PWM.

Based on [QuickNES](https://github.com/libretro/QuickNES_Core) by Shay Green (blargg).

## Screenshots

| ROM Selector | Gameplay | Settings |
|:---:|:---:|:---:|
| ![ROM Selector](screenshots/screen1.png) | ![Gameplay](screenshots/screen2.png) | ![Settings](screenshots/screen3.png) |

## Supported Platforms

frank-nes supports five RP2350-based hardware platforms with different video output options:

| Platform | Board | Video Outputs | Audio |
|----------|-------|---------------|-------|
| **m2** | [Murmulator 2.0](https://murmulator.ru) / [FRANK](https://rh1.tech/projects/frank?area=about) | HDMI (HSTX), HDMI/VGA (PIO), Composite TV | HDMI, I2S, PWM |
| **m1** | Murmulator 1.x | HDMI/VGA (PIO), Composite TV | I2S, PWM |
| **pc** | [Olimex RP2040-PICO-PC](https://www.olimex.com/Products/MicroPython/PICO/RP2040-PICO-PC/) | HDMI (HSTX) | HDMI, PWM |
| **dv** | [Pimoroni Pico DV](https://shop.pimoroni.com/products/pimoroni-pico-dv-demo-base) | HDMI/VGA (PIO) | I2S, PWM |
| **z0** | [Waveshare RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm) | HDMI/VGA (PIO) | I2S, PWM |

Select the platform at build time: `PLATFORM=dv ./build.sh`

The default platform is **m2**. Each platform has its own board configuration header with pin assignments for SD card, PS/2 keyboard, NES gamepad, audio, and PSRAM. The build system auto-selects the correct video driver and rejects incompatible combinations.

## Features

- Multiple video outputs: HDMI via HSTX, PIO-based HDMI/VGA with autodetect, composite TV
- Full NES APU sound emulation (pulse, triangle, noise, DMC) over HDMI, I2S, or PWM
- VRC6, VRC7, FME-7, and Namco 163 expansion audio support
- NES and Dendy (PAL) emulation modes
- 8MB QSPI PSRAM support for ROM loading and tile cache
- SD card ROM browser with cover art, game info, and animated cartridge selector
- File browser mode for navigating large ROM collections and subdirectories
- ROM search (F3 / Select+A)
- On-demand ROM loading (ROMs loaded from SD when selected, not at boot)
- 6-slot save states with color thumbnails
- NES and SNES gamepad support (directly connected)
- USB gamepad support (via native USB Host)
- PS/2 keyboard support
- Configurable input routing (map any input device to Player 1 or Player 2)
- Master volume control
- Runtime settings menu with persistence

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350) or compatible board
- **8MB QSPI PSRAM** (mandatory)
- **HDMI or VGA connector** (directly connected via resistors, no encoder needed)
- **SD card module** (SPI mode)
- **NES or SNES gamepad** (directly connected) – OR –
- **USB gamepad** (via native USB port)
- **I2S DAC module** (e.g., TDA1387, PCM5102) for external audio output (optional, PWM audio works without any DAC)

> **Note:** When USB HID is enabled, the native USB port is used for gamepad input. USB serial console (CDC) is disabled in this mode; use UART for debug output.

### PSRAM

FRANK NES requires 8MB PSRAM to run the ROM selector. Without PSRAM, only a single ROM embedded in flash or the first ROM on SD card can be loaded. You can obtain PSRAM-equipped hardware in several ways:

1. **Solder a PSRAM chip** on top of the Flash chip on a Pico 2 clone (SOP-8 flash chips are only available on clones, not the original Pico 2)
2. **Build a [Nyx 2](https://rh1.tech/projects/nyx?area=nyx2)** – a DIY RP2350 board with integrated PSRAM
3. **Purchase a [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107)** – a ready-made Pico 2 with 8MB PSRAM

## Pin Assignment (M2 Layout)

> Pin assignments for other platforms are defined in `src/platform/pico/board_*.h`.

### HDMI (via 270 Ohm resistors)

| Signal | GPIO |
|--------|------|
| CLK-   | 12   |
| CLK+   | 13   |
| D0-    | 14   |
| D0+    | 15   |
| D1-    | 16   |
| D1+    | 17   |
| D2-    | 18   |
| D2+    | 19   |

### SD Card (SPI mode)

| Signal  | GPIO |
|---------|------|
| CLK     | 6    |
| MOSI    | 7    |
| MISO    | 4    |
| CS      | 5    |

### NES/SNES Gamepad

| Signal | GPIO |
|--------|------|
| CLK    | 20   |
| LATCH  | 21   |
| DATA 1 | 26   |
| DATA 2 | 27   |

> **Note:** Gamepad 2 uses the same CLK and LATCH as Gamepad 1, only the DATA pin differs.

### I2S Audio (optional)

| Signal | GPIO |
|--------|------|
| DATA   | 9    |
| BCLK   | 10   |
| LRCLK  | 11   |

### PS/2 Keyboard

| Signal | GPIO |
|--------|------|
| CLK    | 2    |
| DATA   | 3    |

### PSRAM (auto-detected)

| Chip Package | GPIO |
|--------------|------|
| RP2350B      | 47   |
| RP2350A      | 8    |

## How to Use

### SD Card Setup

1. Format an SD card as **FAT32**
2. Create a `nes` directory in the root
3. Copy `.nes` ROM files into the `nes/` directory
4. (Optional) Copy game metadata for cover art and game info – extract `sdcard/metadata.zip` to your SD card's `nes/` directory
5. Insert the SD card and power on the device

### First Boot and Caching

On the **first boot**, FRANK NES scans all ROM files in `nes` and computes a CRC32 checksum for each one. This is used to look up cover art and game metadata. **This process can be slow if you have many ROMs** - a few seconds per file depending on size.

The checksums are cached in `nes/.crc_cache` so subsequent boots are fast. The cache is automatically updated when new ROMs are added.

> **Recommendation:** Keep the number of ROMs on your SD card reasonable (under 50-100). Loading metadata for hundreds of ROMs will slow down boot time significantly, even with caching. The ROM selector loads cover art on-the-fly as you browse.

### Welcome Screen

On boot, a welcome screen is displayed with the FRANK NES logo, version, and author information. Press **A** or **Start** to continue (or wait 10 seconds for auto-continue).

If no SD card is detected, an error message is shown for 5 seconds before falling back to any ROM embedded in flash.

### ROM Selector

After the welcome screen, the ROM selector displays your game library as animated NES cartridges with cover art:

- **Left / Right** – Browse ROMs
- **Up** – Show game info panel (year, genre, players, description)
- **Down** – Hide game info panel
- **A / Start** – Load selected ROM and start playing

The last selected ROM is remembered across reboots.

### During Gameplay

- **Select + Start** (gamepad), **F12** or **ESC** (keyboard) – Open settings menu

### Game Metadata

For cover art and game info in the ROM selector, place metadata files on the SD card:

```
nes/metadata/Images/160/{X}/{CRC32}.555   – Cover art (RGB555 format)
nes/metadata/descr/{X}/{CRC32}.txt        – Game info (XML format)
```

Where `{X}` is the first hex digit of the CRC32, and `{CRC32}` is the 8-digit uppercase hex checksum (computed from the ROM data after the 16-byte iNES header).

## Controller Support

### NES Gamepad

| NES Button | Action         |
|------------|----------------|
| D-pad      | Movement       |
| A          | A              |
| B          | B              |
| Start      | Start          |
| Select     | Select         |
| Select + Start | Settings menu |

### SNES Gamepad

The emulator automatically detects SNES controllers.

| SNES Button     | NES Button |
|-----------------|------------|
| D-pad           | D-pad      |
| B (bottom)      | A          |
| Y (left)        | B          |
| Start           | Start      |
| Select          | Select     |
| Select + Start  | Settings menu |

### USB Gamepad

Standard USB gamepads are supported with automatic button mapping when built with USB HID support.

### PS/2 / USB Keyboard

| Key        | NES Button |
|------------|------------|
| Arrow keys | D-pad      |
| Z          | A          |
| X          | B          |
| Enter      | Start      |
| Space      | Select     |
| F12 / ESC  | Settings menu |

## Settings Menu

Press **Select + Start** during gameplay (or **F12** / **ESC** on keyboard) to open the settings menu:

| Setting       | Options                                          |
|---------------|--------------------------------------------------|
| Player 1      | Any, NES Pad 1, NES Pad 2, USB 1, USB 2, Keyboard |
| Player 2      | NES Pad 1, NES Pad 2, USB 1, USB 2, Keyboard, Disabled |
| Audio         | HDMI, I2S, PWM, Disabled (availability depends on platform) |
| Volume        | 0% – 100% (10% steps)                            |
| Mode          | NES (NTSC 60 Hz), Dendy (PAL 50 Hz)              |
| Save Game     | Save state to SD card (6 slots with thumbnails)   |
| Load Game     | Load state from SD card                           |
| Back to ROM Selector | Return to ROM browser (resets emulator)    |
| Back to Game  | Resume gameplay                                   |

Settings are saved to `nes/.settings` and persist across reboots. Save states are stored in `nes/.save/{rom_name}_{slot}.sav`.

## Building

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (version 2.0+)
2. Set environment variable: `export PICO_SDK_PATH=/path/to/pico-sdk`
3. Install ARM GCC toolchain

### Build

```bash
git clone https://github.com/rh1tech/frank-nes.git
cd frank-nes
./build.sh                    # Default: M2, HSTX HDMI
PLATFORM=dv ./build.sh        # Pimoroni Pico DV
PLATFORM=m1 ./build.sh        # Murmulator 1.x (PIO HDMI/VGA)
PLATFORM=pc ./build.sh        # Olimex PICO-PC (HSTX HDMI)
PLATFORM=z0 ./build.sh        # RP2350-PiZero (PIO HDMI/VGA)
```

Output: `build/frank-nes.uf2`

### Build Options

You can pass options via environment variables:

```bash
# Platform selection (default: m2)
PLATFORM=dv ./build.sh

# Composite TV output (M1 and M2 only)
VIDEO_COMPOSITE=1 ./build.sh

# PIO HDMI/VGA output (auto-selected for m1, dv, z0)
HDMI_PIO=1 ./build.sh

# Embed a ROM directly in flash (no SD card needed)
NES_ROM=path/to/game.nes ./build.sh

# Enable USB HID gamepad support
USB_HID=1 ./build.sh
```

### Release Build

Build all 8 platform/video variants at once:

```bash
./release.sh          # Interactive version prompt
./release.sh 1.05     # Specify version
```

This produces UF2 files for every supported platform and video combination.

### Flashing

```bash
# With device in BOOTSEL mode:
./flash.sh

# Or manually:
picotool load build/frank-nes.uf2
```

## Troubleshooting

### Device won't boot after changing settings

Remove the `nes/.settings` file from the SD card to restore defaults.

### ROM selector shows no cover art

Make sure the metadata files are in the correct directory structure on the SD card. See [Game Metadata](#game-metadata) above.

### First boot is very slow

This is normal – CRC32 checksums are being computed for all ROMs. Subsequent boots will be fast thanks to the cache file. Reduce the number of ROMs if boot time is unacceptable.

## License

Copyright (c) 2026 Mikhail Matveev <<xtreme@rh1.tech>>

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for details.

Note: The QuickNES core is licensed under LGPL-2.1-or-later. The PS/2 keyboard driver is licensed under GPL-2.0-or-later. The HDMI driver (pico_hdmi) is released under the Unlicense. Other components have their own licenses as noted below.

## Acknowledgments

This project incorporates code and ideas from the following projects:

| Project | Author(s) | License | Used For |
|---------|-----------|---------|----------|
| [QuickNES](https://github.com/libretro/QuickNES_Core) | Shay Green (blargg) | LGPL-2.1 | NES emulation core |
| [EMU2413](https://github.com/libretro/QuickNES_Core) | Mitsutaka Okazaki, xodnizel | Custom | VRC7 (YM2413/OPLL) sound emulation |
| [pico_hdmi](https://github.com/fliperama86/pico_hdmi) | fliperama86 | Unlicense | HSTX-native HDMI output with audio |
| [pico-spec](https://github.com/DnCraptor/pico-spec) | DnCraptor | GPL-3.0 | PIO HDMI/VGA driver, composite TV driver, multi-platform pin configs |
| [FatFS](http://elm-chan.org/fsw/ff/) | ChaN | Custom permissive | FAT32 filesystem for SD card |
| [pico_fatfs_test](https://github.com/elehobica/pico_fatfs_test) | Elehobica | BSD-2-Clause | SD card PIO-SPI driver |
| [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) | shuichitakano, fhoedemakers | MIT | NES/SNES gamepad PIO driver, game metadata |
| [PS/2 keyboard driver](https://github.com/mrmltr) | mrmltr | GPL-2.0 | PS/2 keyboard PIO driver |
| [TinyUSB](https://github.com/hathach/tinyusb) | Ha Thach | MIT | USB HID host driver |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | Raspberry Pi Foundation | BSD-3-Clause | Hardware abstraction layer |

Special thanks to:

- Shay Green (blargg) for the QuickNES emulator core and Blip_Buffer audio library
- Mitsutaka Okazaki for the EMU2413 OPLL emulator
- fliperama86 for the pico_hdmi HSTX HDMI output library
- DnCraptor for the pico-spec project (PIO HDMI/VGA, composite TV drivers, and multi-platform board configurations)
- shuichitakano for the original pico-infones Pico port and NES gamepad PIO driver
- fhoedemakers for pico-infonesPlus and game metadata
- The libretro team for maintaining the QuickNES fork
- Nintendo for the original NES hardware
- The Raspberry Pi Foundation for the RP2350 and Pico SDK
- The Murmulator community for hardware designs and testing

## Author

Mikhail Matveev <<xtreme@rh1.tech>>

[https://rh1.tech](https://rh1.tech) | [GitHub](https://github.com/rh1tech/frank-nes)

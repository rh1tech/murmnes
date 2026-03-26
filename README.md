# FRANK NES

NES (Nintendo Entertainment System) emulator for Raspberry Pi Pico 2 (RP2350) with HDMI output, SD card ROM browser, NES/SNES gamepad, USB gamepad, PS/2 keyboard, and I2S audio support.

Based on [QuickNES](https://github.com/libretro/QuickNES_Core) by Shay Green (blargg).

## Screenshots

| ROM Selector | Gameplay | Settings |
|:---:|:---:|:---:|
| ![ROM Selector](screenshots/screen1.png) | ![Gameplay](screenshots/screen2.png) | ![Settings](screenshots/screen3.png) |

## Supported Board

This firmware is designed for the **M2** board layout on RP2350-based boards with integrated HDMI, SD card, and PSRAM:

- **[FRANK](https://rh1.tech/projects/frank?area=about)** – A versatile development board based on RP Pico 2 with HDMI output and extensive I/O options.
- **[Murmulator](https://murmulator.ru)** – A compact retro-computing platform based on RP Pico 2, designed for emulators and classic games.

Both boards provide all necessary peripherals out of the box (no additional wiring required).

## Features

- Native 640x480 HDMI video output via HSTX (doubled from 256x240 NES resolution)
- Full NES APU sound emulation (pulse, triangle, noise, DMC) over HDMI or I2S
- VRC6, VRC7, FME-7, and Namco 163 expansion audio support
- 8MB QSPI PSRAM support for ROM loading and tile cache
- SD card ROM browser with cover art, game info, and animated cartridge selector
- On-demand ROM loading (ROMs loaded from SD when selected, not at boot)
- Save states (save and load game progress to SD card)
- NES and SNES gamepad support (directly connected)
- USB gamepad support (via native USB Host)
- PS/2 keyboard support
- Configurable input routing (map any input device to Player 1 or Player 2)
- Master volume control
- Runtime settings menu with persistence

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350) or compatible board
- **8MB QSPI PSRAM** (mandatory)
- **HDMI connector** (directly connected via resistors, no HDMI encoder needed)
- **SD card module** (SPI mode)
- **NES or SNES gamepad** (directly connected) – OR –
- **USB gamepad** (via native USB port)
- **I2S DAC module** (e.g., TDA1387, PCM5102) for external audio output (optional)

> **Note:** When USB HID is enabled, the native USB port is used for gamepad input. USB serial console (CDC) is disabled in this mode; use UART for debug output.

### PSRAM

MurmNES requires 8MB PSRAM to run the ROM selector. Without PSRAM, only a single ROM embedded in flash or the first ROM on SD card can be loaded. You can obtain PSRAM-equipped hardware in several ways:

1. **Solder a PSRAM chip** on top of the Flash chip on a Pico 2 clone (SOP-8 flash chips are only available on clones, not the original Pico 2)
2. **Build a [Nyx 2](https://rh1.tech/projects/nyx?area=nyx2)** – a DIY RP2350 board with integrated PSRAM
3. **Purchase a [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107)** – a ready-made Pico 2 with 8MB PSRAM

## Pin Assignment (M2 Layout)

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

On the **first boot**, MurmNES scans all ROM files in `nes` and computes a CRC32 checksum for each one. This is used to look up cover art and game metadata. **This process can be slow if you have many ROMs** - a few seconds per file depending on size.

The checksums are cached in `nes/.crc_cache` so subsequent boots are fast. The cache is automatically updated when new ROMs are added.

> **Recommendation:** Keep the number of ROMs on your SD card reasonable (under 50-100). Loading metadata for hundreds of ROMs will slow down boot time significantly, even with caching. The ROM selector loads cover art on-the-fly as you browse.

### Welcome Screen

On boot, a welcome screen is displayed with the MurmNES logo, version, and author information. Press **A** or **Start** to continue (or wait 10 seconds for auto-continue).

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
| Audio         | HDMI, I2S, Disabled                               |
| Volume        | 0% – 100% (10% steps)                            |
| Save Game     | Save state to SD card                             |
| Load Game     | Load state from SD card                           |
| Back to ROM Selector | Return to ROM browser (resets emulator)    |
| Back to Game  | Resume gameplay                                   |

Settings are saved to `nes/.settings` and persist across reboots. Save states are stored in `nes/.save/{rom_name}.sav`.

## Building

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (version 2.0+)
2. Set environment variable: `export PICO_SDK_PATH=/path/to/pico-sdk`
3. Install ARM GCC toolchain

### Build

```bash
git clone https://github.com/rh1tech/murmnes.git
cd murmnes
./build.sh
```

The build script compiles the firmware with USB HID support enabled by default. Output: `build/murmnes.uf2`

### Build Options

You can pass options via environment variables:

```bash
# Embed a ROM directly in flash (no SD card needed)
NES_ROM=path/to/game.nes ./build.sh
```

### Flashing

```bash
# With device in BOOTSEL mode:
./flash.sh

# Or manually:
picotool load build/murmnes.uf2
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
- shuichitakano for the original pico-infones Pico port and NES gamepad PIO driver
- fhoedemakers for pico-infonesPlus and game metadata
- The libretro team for maintaining the QuickNES fork
- Nintendo for the original NES hardware
- The Raspberry Pi Foundation for the RP2350 and Pico SDK
- The Murmulator community for hardware designs and testing

## Author

Mikhail Matveev <<xtreme@rh1.tech>>

[https://rh1.tech](https://rh1.tech) | [GitHub](https://github.com/rh1tech/murmnes)

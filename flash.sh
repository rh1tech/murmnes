#!/bin/bash
FIRMWARE="${1:-./build/murmnes.elf}"
if [ ! -f "$FIRMWARE" ]; then
    FIRMWARE="${FIRMWARE%.elf}.uf2"
fi
picotool load -f "$FIRMWARE" && picotool reboot -f

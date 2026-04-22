# When built from root CMakeLists.txt, SDK is already initialized
if(NOT PICO_BUILD_FROM_ROOT)
    # == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
    if(WIN32)
        set(USERHOME $ENV{USERPROFILE})
    else()
        set(USERHOME $ENV{HOME})
    endif()
    set(sdkVersion 2.2.0)
    set(toolchainVersion 14_2_Rel1)
    set(picotoolVersion 2.2.0-a4)
    set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
    if (EXISTS ${picoVscode})
        include(${picoVscode})
    endif()
    # ====================================================================================
    cmake_minimum_required(VERSION 3.13)

    # Platform selection: m1, m2 (default), pc, dv, z0
    set(PLATFORM "m2" CACHE STRING "Target platform: m1, m2, pc, dv, z0")
    set_property(CACHE PLATFORM PROPERTY STRINGS m1 m2 pc dv z0)

    if(PLATFORM STREQUAL "z0")
        set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
        set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Board type" FORCE)
    else()
        set(PICO_BOARD "pico2" CACHE STRING "Board type" FORCE)
    endif()

    include(${CMAKE_CURRENT_LIST_DIR}/../../../pico_sdk_import.cmake)

    project(frank-nes C CXX ASM)

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    pico_sdk_init()
endif()

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(SRCDIR ${PROJECT_ROOT}/src)
set(DRIVERSDIR ${PROJECT_ROOT}/drivers)
set(COREDIR ${SRCDIR}/core_quicknes)
set(EMU2413DIR ${SRCDIR}/emu2413)
set(PICODIR ${SRCDIR}/platform/pico)

# Platform selection (may already be set from standalone block above)
if(NOT DEFINED PLATFORM)
    set(PLATFORM "m2" CACHE STRING "Target platform: m1, m2, pc, dv, z0")
endif()
message(STATUS "Platform: ${PLATFORM}")

# Per-platform compile definition and pin configuration
if(PLATFORM STREQUAL "m1")
    set(PLATFORM_DEF PLATFORM_M1)
    set(SD_SCK 2)
    set(SD_MOSI 3)
    set(SD_MISO 4)
    set(SD_CS 5)
    set(KBD_CLK 0)
    set(HDMI_PIN 6)
    set(VGA_PIN 6)
    set(TV_PIN 6)
elseif(PLATFORM STREQUAL "pc")
    set(PLATFORM_DEF PLATFORM_PC)
    set(SD_SCK 6)
    set(SD_MOSI 7)
    set(SD_MISO 4)
    set(SD_CS 22)
    set(KBD_CLK 0)
    set(HDMI_PIN 12)
elseif(PLATFORM STREQUAL "dv")
    set(PLATFORM_DEF PLATFORM_DV)
    set(SD_SCK 5)
    set(SD_MOSI 18)
    set(SD_MISO 19)
    set(SD_CS 22)
    set(KBD_CLK 0)
    set(HDMI_PIN 6)
    set(VGA_PIN 6)
elseif(PLATFORM STREQUAL "z0")
    set(PLATFORM_DEF PLATFORM_Z0)
    set(SD_SCK 30)
    set(SD_MOSI 31)
    set(SD_MISO 40)
    set(SD_CS 43)
    set(KBD_CLK 14)
    set(HDMI_PIN 32)
    set(VGA_PIN 32)
else()
    set(PLATFORM_DEF PLATFORM_M2)
    set(SD_SCK 6)
    set(SD_MOSI 7)
    set(SD_MISO 4)
    set(SD_CS 5)
    set(KBD_CLK 2)
    set(HDMI_PIN 12)
    set(VGA_PIN 12)
    set(TV_PIN 12)
endif()

# Video output selection: HSTX HDMI (default for m2/pc), Composite TV, or PIO HDMI
option(VIDEO_COMPOSITE "Build with composite TV output instead of HDMI" OFF)
option(HDMI_PIO "Build with PIO-based HDMI output" OFF)

# Auto-select video driver for non-HSTX platforms
if(NOT VIDEO_COMPOSITE AND NOT HDMI_PIO)
    if(PLATFORM STREQUAL "m1" OR PLATFORM STREQUAL "dv" OR PLATFORM STREQUAL "z0")
        set(HDMI_PIO ON)
    endif()
endif()

# Validate video mode vs platform
if(VIDEO_COMPOSITE)
    if(PLATFORM STREQUAL "pc" OR PLATFORM STREQUAL "dv" OR PLATFORM STREQUAL "z0")
        message(FATAL_ERROR "Platform '${PLATFORM}' does not support composite TV output")
    endif()
endif()
if(NOT HDMI_PIO AND NOT VIDEO_COMPOSITE)
    if(PLATFORM STREQUAL "m1" OR PLATFORM STREQUAL "dv" OR PLATFORM STREQUAL "z0")
        message(FATAL_ERROR "Platform '${PLATFORM}' requires HDMI_PIO (no HSTX support)")
    endif()
endif()

if(HDMI_PIO)
    add_subdirectory(${DRIVERSDIR}/hdmi_pio hdmi_pio)
    target_compile_definitions(hdmi_pio PUBLIC
        HDMI_BASE_PIN=${HDMI_PIN}
        VGA_BASE_PIN=${VGA_PIN}
    )
elseif(VIDEO_COMPOSITE)
    add_subdirectory(${DRIVERSDIR}/tv tv_driver)
    if(DEFINED TV_PIN)
        target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=${TV_PIN})
    endif()
else()
    # HSTX clocked from PLL_USB at 126MHz (reconfigured in main)
    add_compile_definitions(MODE_HSTX_CLK_DIV=1)
    add_subdirectory(${DRIVERSDIR}/pico_hdmi pico_hdmi)
endif()

# Peripheral drivers (always compiled)
set(PSRAM_MAX_FREQ_MHZ 133 CACHE STRING "PSRAM clock frequency in MHz")
add_compile_definitions(PSRAM_MAX_FREQ_MHZ=${PSRAM_MAX_FREQ_MHZ})

add_subdirectory(${SRCDIR}/lib/fatfs fatfs)
add_subdirectory(${DRIVERSDIR}/sdcard sdcard)
add_subdirectory(${DRIVERSDIR}/nespad nespad)
add_subdirectory(${DRIVERSDIR}/ps2kbd ps2kbd)

option(USB_HID_ENABLED "Enable USB HID host (disables USB serial)" OFF)
if(USB_HID_ENABLED)
    add_subdirectory(${DRIVERSDIR}/usbhid usbhid)
endif()

# QuickNES emulation core sources
set(QNES_SOURCES
    ${COREDIR}/nes_emu.cpp
    ${COREDIR}/nes_core.cpp
    ${COREDIR}/nes_cpu.cpp
    ${COREDIR}/nes_ppu.cpp
    ${COREDIR}/nes_ppu_impl.cpp
    ${COREDIR}/nes_ppu_rendering.cpp
    ${COREDIR}/nes_cart.cpp
    ${COREDIR}/nes_mapper.cpp
    ${COREDIR}/nes_apu.cpp
    ${COREDIR}/nes_oscs.cpp
    ${COREDIR}/nes_file.cpp
    ${COREDIR}/nes_state.cpp
    ${COREDIR}/nes_buffer.cpp
    ${COREDIR}/nes_effects_buffer.cpp
    ${COREDIR}/blip_buffer.cpp
    ${COREDIR}/multi_buffer.cpp
    ${COREDIR}/effects_buffer.cpp
    ${COREDIR}/data_reader.cpp
    ${COREDIR}/abstract_file.cpp
    ${COREDIR}/nes_data.cpp
    ${COREDIR}/nes_util.cpp
    ${COREDIR}/apu_state.cpp
    ${COREDIR}/nes_fme7_apu.cpp
    ${COREDIR}/nes_namco_apu.cpp
    ${COREDIR}/nes_vrc6_apu.cpp
    ${COREDIR}/nes_vrc7.cpp
    ${COREDIR}/quicknes.cpp
)

# emu2413 (YM2413/VRC7 emulator)
set(EMU2413_SOURCES
    ${EMU2413DIR}/emu2413.cpp
    ${EMU2413DIR}/emu2413_state.cpp
)

add_executable(frank-nes
    ${PICODIR}/main_pico.c
    ${PICODIR}/settings.c
    ${PICODIR}/rom_selector.c
    ${PICODIR}/uart_logging.c
    ${DRIVERSDIR}/psram/psram_init.c
    ${DRIVERSDIR}/psram/psram_allocator.c
    ${DRIVERSDIR}/i2s/i2s_audio.c
    ${QNES_SOURCES}
    ${EMU2413_SOURCES}
)

# Generate PIO header for I2S audio
pico_generate_pio_header(frank-nes ${DRIVERSDIR}/i2s/audio_i2s.pio)

target_include_directories(frank-nes PRIVATE
    ${PICODIR}
    ${COREDIR}
    ${EMU2413DIR}
    ${DRIVERSDIR}
    ${DRIVERSDIR}/psram
    ${DRIVERSDIR}/i2s
    ${SRCDIR}/lib/fatfs
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(frank-nes PRIVATE
    ${PLATFORM_DEF}
    PICO_CORE1_STACK_SIZE=0xC00
    PICO_STACK_SIZE=0x1000
    NDEBUG
    NO_UNALIGNED_ACCESS
    QNES_EXT_TILE_CACHE
    SDCARD_PIN_SPI0_SCK=${SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${SD_MISO}
    SDCARD_PIN_SPI0_CS=${SD_CS}
)
if(HDMI_PIO)
    target_compile_definitions(frank-nes PRIVATE HDMI_PIO)
elseif(VIDEO_COMPOSITE)
    target_compile_definitions(frank-nes PRIVATE VIDEO_COMPOSITE)
endif()

target_compile_definitions(frank-nes PRIVATE HAS_NESPAD=1)

if(PLATFORM STREQUAL "z0")
    target_compile_definitions(frank-nes PRIVATE SDCARD_SPI_BUS=spi1)
    target_link_libraries(frank-nes hardware_spi)
endif()

# C++ flags: no exceptions, no RTTI (saves flash/RAM)
# -mno-unaligned-access: QuickNES casts byte* to uint32_t* for tile cache,
# which can produce misaligned pointers. LDRD faults on unaligned on Cortex-M33.
target_compile_options(frank-nes PRIVATE
    -O2
    -mno-unaligned-access
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -Wno-sign-compare
    -Wno-unused-parameter
)

if(HDMI_PIO)
    set(VIDEO_LIB hdmi_pio)
elseif(VIDEO_COMPOSITE)
    set(VIDEO_LIB tv_driver)
else()
    set(VIDEO_LIB pico_hdmi)
endif()

target_link_libraries(frank-nes
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
    hardware_pio
    hardware_pll
    hardware_pwm
    hardware_xip_cache
    ${VIDEO_LIB}
    sdcard
    nespad
    ps2kbd
)

if(USB_HID_ENABLED)
    target_link_libraries(frank-nes usbhid)
    pico_enable_stdio_usb(frank-nes 0)
else()
    pico_enable_stdio_usb(frank-nes 1)
endif()
pico_enable_stdio_uart(frank-nes 0)
target_link_libraries(frank-nes hardware_uart)

# Optional: embed NES ROM into flash
if(DEFINED NES_ROM_PATH)
    set(ROM_COPY ${CMAKE_CURRENT_BINARY_DIR}/nes_rom.bin)
    set(ROM_OBJ ${CMAKE_CURRENT_BINARY_DIR}/nes_rom.o)

    add_custom_command(
        OUTPUT ${ROM_COPY}
        COMMAND ${CMAKE_COMMAND} -E copy ${NES_ROM_PATH} ${ROM_COPY}
        DEPENDS ${NES_ROM_PATH}
        COMMENT "Copying ROM to build dir"
    )

    add_custom_command(
        OUTPUT ${ROM_OBJ}
        COMMAND ${CMAKE_OBJCOPY}
            -I binary -O elf32-littlearm
            -B arm
            --rename-section .data=.rodata,alloc,load,readonly,data,contents
            nes_rom.bin nes_rom.o
        DEPENDS ${ROM_COPY}
        COMMENT "Embedding NES ROM into flash"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    target_sources(frank-nes PRIVATE ${ROM_OBJ})
    target_compile_definitions(frank-nes PRIVATE HAS_NES_ROM)

    target_link_options(frank-nes PRIVATE
        -Wl,--defsym=nes_rom_data=_binary_nes_rom_bin_start
        -Wl,--defsym=nes_rom_end=_binary_nes_rom_bin_end
    )
endif()

# Read version from version.txt (format: "MAJOR MINOR", e.g. "1 00" -> "1.00")
file(READ "${PROJECT_ROOT}/version.txt" FRANK_NES_VERSION_RAW)
string(STRIP "${FRANK_NES_VERSION_RAW}" FRANK_NES_VERSION_RAW)
string(REPLACE " " "." FRANK_NES_VERSION_STR "${FRANK_NES_VERSION_RAW}")
target_compile_definitions(frank-nes PRIVATE FRANK_NES_VERSION="${FRANK_NES_VERSION_STR}")

pico_add_extra_outputs(frank-nes)

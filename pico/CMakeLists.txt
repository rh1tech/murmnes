cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(murmnes C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(DRIVERSDIR ${PROJECT_ROOT}/drivers)
set(PICODIR ${PROJECT_ROOT}/src/pico)
set(QNESDIR ${PROJECT_ROOT}/nes_emu)

# sys_clk=252MHz, HSTX needs 126MHz -> divider=2
add_compile_definitions(MODE_HSTX_CLK_DIV=2)
add_subdirectory(${DRIVERSDIR}/pico_hdmi pico_hdmi)

# Peripheral drivers (always compiled)
set(PSRAM_MAX_FREQ_MHZ 133 CACHE STRING "PSRAM clock frequency in MHz")
add_compile_definitions(PSRAM_MAX_FREQ_MHZ=${PSRAM_MAX_FREQ_MHZ})

add_subdirectory(${PROJECT_ROOT}/lib/fatfs fatfs)
add_subdirectory(${DRIVERSDIR}/sdcard sdcard)
add_subdirectory(${DRIVERSDIR}/ps2kbd ps2kbd)

option(USB_HID_ENABLED "Enable USB HID host (disables USB serial)" OFF)
if(USB_HID_ENABLED)
    add_subdirectory(${DRIVERSDIR}/usbhid usbhid)
endif()

# QuickNES emulation core sources (excluding NTSC filter)
set(QNES_SOURCES
    ${QNESDIR}/Nes_Emu.cpp
    ${QNESDIR}/Nes_Core.cpp
    ${QNESDIR}/Nes_Cpu.cpp
    ${QNESDIR}/Nes_Ppu.cpp
    ${QNESDIR}/Nes_Ppu_Impl.cpp
    ${QNESDIR}/Nes_Ppu_Rendering.cpp
    ${QNESDIR}/Nes_Cart.cpp
    ${QNESDIR}/Nes_Mapper.cpp
    ${QNESDIR}/Nes_Apu.cpp
    ${QNESDIR}/Nes_Oscs.cpp
    ${QNESDIR}/Nes_File.cpp
    ${QNESDIR}/Nes_State.cpp
    ${QNESDIR}/Nes_Buffer.cpp
    ${QNESDIR}/Nes_Effects_Buffer.cpp
    ${QNESDIR}/Blip_Buffer.cpp
    ${QNESDIR}/Multi_Buffer.cpp
    ${QNESDIR}/Effects_Buffer.cpp
    ${QNESDIR}/Data_Reader.cpp
    ${QNESDIR}/abstract_file.cpp
    ${QNESDIR}/nes_data.cpp
    ${QNESDIR}/nes_util.cpp
    ${QNESDIR}/apu_state.cpp
    ${QNESDIR}/Nes_Fme7_Apu.cpp
    ${QNESDIR}/Nes_Namco_Apu.cpp
    ${QNESDIR}/Nes_Vrc6_Apu.cpp
    ${QNESDIR}/Nes_Vrc7.cpp
    ${QNESDIR}/emu2413.cpp
    ${QNESDIR}/emu2413_state.cpp
    ${QNESDIR}/quicknes.cpp
)

add_executable(murmnes
    ${PICODIR}/main_pico.c
    ${DRIVERSDIR}/psram_init.c
    ${DRIVERSDIR}/psram_allocator.c
    ${QNES_SOURCES}
)

target_include_directories(murmnes PRIVATE
    ${PICODIR}
    ${QNESDIR}
    ${DRIVERSDIR}
)

target_compile_definitions(murmnes PRIVATE
    PICO_CORE1_STACK_SIZE=0xC00
    PICO_STACK_SIZE=0x1000
    NDEBUG
    NO_UNALIGNED_ACCESS
)

# C++ flags: no exceptions, no RTTI (saves flash/RAM)
# -mno-unaligned-access: QuickNES casts byte* to uint32_t* for tile cache,
# which can produce misaligned pointers. LDRD faults on unaligned on Cortex-M33.
target_compile_options(murmnes PRIVATE
    -O2
    -mno-unaligned-access
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    -Wno-sign-compare
    -Wno-unused-parameter
)

target_link_libraries(murmnes
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
    pico_hdmi
    sdcard
    ps2kbd
)

if(USB_HID_ENABLED)
    target_link_libraries(murmnes usbhid)
    pico_enable_stdio_usb(murmnes 0)
else()
    pico_enable_stdio_usb(murmnes 1)
endif()
pico_enable_stdio_uart(murmnes 0)

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

    target_sources(murmnes PRIVATE ${ROM_OBJ})
    target_compile_definitions(murmnes PRIVATE HAS_NES_ROM)

    target_link_options(murmnes PRIVATE
        -Wl,--defsym=nes_rom_data=_binary_nes_rom_bin_start
        -Wl,--defsym=nes_rom_end=_binary_nes_rom_bin_end
    )
endif()

pico_add_extra_outputs(murmnes)

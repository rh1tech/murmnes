# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.24)

# Подключаем Pico SDK только если он доступен
set(picoSdkImport ${CMAKE_SOURCE_DIR}/pico_sdk_import.cmake)
if (EXISTS ${picoSdkImport})
    include(${picoSdkImport})
    set(PICO_BUILD TRUE)
endif()

project(cNES LANGUAGES C)
enable_testing()
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_VERBOSE_MAKEFILE OFF)
set(CMAKE_SIZE_UTIL size)

# Инициализируем Pico SDK если он есть
if (PICO_BUILD)
    pico_sdk_init()
endif()

# ─── Зависимости только для хостовой сборки ───────────────────────────────────
if (NOT PICO_BUILD)
    include(FetchContent)
    set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/deps)

    set(sdl2_version 2.30.9)
    set(sdl2_release_link "https://github.com/libsdl-org/SDL/releases/download/")
    string(APPEND sdl2_release_link "release-${sdl2_version}/SDL2-${sdl2_version}.tar.gz")

    FetchContent_Declare(
        SDL2
        URL ${sdl2_release_link}
        URL_HASH MD5=57393a5e1a46dd19ae40968a301c70a3
        DOWNLOAD_EXTRACT_TIMESTAMP True
        FIND_PACKAGE_ARGS
    )

    FetchContent_Declare(
        LIBCHECK
        GIT_REPOSITORY https://github.com/libcheck/check.git
        GIT_TAG 455005dc29dc6727de7ee36fee4b49a13b39f73f
        FIND_PACKAGE_ARGS NAMES check
    )

    FetchContent_MakeAvailable(SDL2 LIBCHECK)
endif()

# ─── Пути ─────────────────────────────────────────────────────────────────────
set(SRCTARGET cnes)
set(TSTTARGET test_all)

set(SRCDIR   ${CMAKE_SOURCE_DIR}/src)
set(COREDIR  ${SRCDIR}/core_quicknes)
set(UTILDIR  ${SRCDIR}/util)
set(INCDIR   ${SRCDIR})
set(INCCOREDIR ${SRCDIR}/core_quicknes)
set(INCUTILDIR ${SRCDIR}/util)
set(LIBDIR lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LIBDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LIBDIR})

# ─── Флаги компилятора ────────────────────────────────────────────────────────
set(COMMON_CFLAGS "-Wall;-Wextra")

if (DEFINED ASAN)
    unset(ASAN)
    set(ASAN_FLAGS -fsanitize=address,undefined)
endif()

if (DEFINED COVERAGE)
    set(COVERAGE_FLAGS "-coverage;-O0")
endif()

# ─── Исходники ────────────────────────────────────────────────────────────────
set(UTILSRCS "${UTILDIR}/bits_and_bytes.c")

set(CORESRCS
    "${COREDIR}/cart.c"
    "${COREDIR}/cpu.c"
    "${COREDIR}/emu.c"
    "${COREDIR}/gui.c"
    "${COREDIR}/mappers.c"
    "${COREDIR}/ppu.c"
    "${COREDIR}/cpu_ppu_interface.c"
    "${COREDIR}/cpu_mapper_interface.c"
)

set(TSTDIR  ${CMAKE_SOURCE_DIR}/tests)
set(TSTSRCS
    "${TSTDIR}/test_all.c"
    "${TSTDIR}/cpu_tests.c"
    "${TSTDIR}/mappers_tests.c"
    "${TSTDIR}/ppu_tests.c"
    "${TSTDIR}/cpu_ppu_interface_tests.c"
    "${TSTDIR}/util_tests.c"
)

# ─── Debug-вывод ──────────────────────────────────────────────────────────────
if (${DEBUG_CMAKE})
    message("PICO_BUILD: ${PICO_BUILD}")
    message("SRCDIR ${SRCDIR} | COREDIR ${COREDIR} | UTILDIR ${UTILDIR}")
    message("CORESRCS: ${CORESRCS}")
    message("UTILSRCS: ${UTILSRCS}")
    message("TSTSRCS:  ${TSTSRCS}")
endif()

# ─── Общий интерфейс флагов ───────────────────────────────────────────────────
add_library(common INTERFACE)
target_compile_options(common INTERFACE
    ${COMMON_CFLAGS}
    "$<$<CONFIG:DEBUG>:-g>"
    ${ASAN_FLAGS}
    ${COVERAGE_FLAGS}
)
target_compile_definitions(common INTERFACE "$<$<CONFIG:DEBUG>:__DEBUG__>")

# ─── Библиотеки исходников ────────────────────────────────────────────────────
add_library(utilsrcs ${UTILSRCS})
target_include_directories(utilsrcs PRIVATE ${INCUTILDIR})
target_link_libraries(utilsrcs PRIVATE common ${COVERAGE_FLAGS})

add_library(coresrcs ${CORESRCS})
target_include_directories(coresrcs PRIVATE
    ${INCCOREDIR}
    ${INCUTILDIR}
    "$<$<BOOL:${sdl2_BINARY_DIR}>:${sdl2_BINARY_DIR}/include>"
)
target_link_libraries(coresrcs PRIVATE common ${COVERAGE_FLAGS})

if (NOT PICO_BUILD)
    target_link_libraries(coresrcs PRIVATE SDL2::SDL2)
endif()

# ─── Основной исполняемый файл ────────────────────────────────────────────────
add_executable(${SRCTARGET} ${CORESRCS})
target_include_directories(${SRCTARGET} PRIVATE
    ${INCDIR}
    ${INCUTILDIR}
    ${INCCOREDIR}
    "$<$<BOOL:${sdl2_BINARY_DIR}>:${sdl2_BINARY_DIR}/include>"
    "$<$<BOOL:${sdl2_SOURCE_DIR}>:${sdl2_SOURCE_DIR}/include>"
)
target_link_libraries(${SRCTARGET} PRIVATE common utilsrcs coresrcs)
target_link_options(${SRCTARGET} PRIVATE ${ASAN_FLAGS})

if (PICO_BUILD)
    pico_add_extra_outputs(${SRCTARGET})
    pico_enable_stdio_uart(${SRCTARGET} 1)
    pico_enable_stdio_usb(${SRCTARGET} 0)
endif()

add_custom_command(TARGET ${SRCTARGET} POST_BUILD
    COMMAND ${CMAKE_SIZE_UTIL} "$<TARGET_FILE_NAME:${SRCTARGET}>"
    COMMENT "Executable size"
)

# ─── Тесты (только хостовая сборка) ──────────────────────────────────────────
if (NOT PICO_BUILD)
    add_executable(${TSTTARGET} ${TSTSRCS})
    add_test(NAME ${TSTTARGET} COMMAND ${TSTTARGET})

    target_compile_options(${TSTTARGET} PRIVATE
        ${COMMON_CFLAGS}
        "$<$<CONFIG:DEBUG>:-g>"
        ${ASAN_FLAGS}
        "$<$<BOOL:${TSTSRCCOVERAGE}>:${COVERAGE_FLAGS}>"
    )
    target_link_options(${TSTTARGET} PRIVATE
        ${ASAN_FLAGS}
        "$<$<BOOL:${TSTSRCCOVERAGE}>:${COVERAGE_FLAGS}>"
    )
    target_include_directories(${TSTTARGET} PRIVATE
        ${INCCOREDIR}
        ${INCUTILDIR}
        ${libcheck_BINARY_DIR}
    )
    target_link_libraries(${TSTTARGET} PRIVATE check coresrcs utilsrcs)

    add_custom_command(TARGET ${TSTTARGET} POST_BUILD
        COMMAND ${CMAKE_SIZE_UTIL} "$<TARGET_FILE_NAME:${TSTTARGET}>"
        COMMENT "Executable size (unit tests)"
    )

    add_custom_target(runtests
        DEPENDS ${TSTTARGET}
        COMMAND ctest --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
        COMMENT "Execute tests"
    )

    if (DEFINED COVERAGE)
        unset(COVERAGE)
        add_custom_target(runcoveragedata
            DEPENDS ${TSTTARGET}
            COMMAND ctest -T Coverage --test-dir "${CMAKE_BINARY_DIR}"
            COMMENT "Coverage of tests"
        )
    endif()
endif()

unset(DEBUG_CMAKE CACHE)

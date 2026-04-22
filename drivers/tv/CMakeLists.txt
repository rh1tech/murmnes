add_library(tv_driver STATIC
    ${CMAKE_CURRENT_LIST_DIR}/tv-software.c
    ${CMAKE_CURRENT_LIST_DIR}/graphics.c
)

target_include_directories(tv_driver PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_options(tv_driver PRIVATE
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-all
)

target_link_libraries(tv_driver
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_clocks
)

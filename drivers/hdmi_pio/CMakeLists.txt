add_library(hdmi_pio STATIC
    ${CMAKE_CURRENT_LIST_DIR}/hdmi.c
    ${CMAKE_CURRENT_LIST_DIR}/hdmi_scanline.S
    ${CMAKE_CURRENT_LIST_DIR}/vga.c
    ${CMAKE_CURRENT_LIST_DIR}/test_pins.c
)
target_include_directories(hdmi_pio PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(hdmi_pio PUBLIC HDMI_PIO=1)
target_compile_options(hdmi_pio PRIVATE -Wno-error -Wno-all)
target_link_libraries(hdmi_pio pico_stdlib hardware_pio hardware_clocks hardware_dma)

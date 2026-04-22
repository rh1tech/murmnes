add_library(ps2kbd STATIC)

target_sources(ps2kbd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.h
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.h
)

target_link_libraries(ps2kbd PRIVATE hardware_pio hardware_clocks)

target_compile_definitions(ps2kbd PRIVATE
    KBD_CLOCK_PIN=${KBD_CLK}
    ${PLATFORM_DEF}
)
if(HDMI_PIO)
    target_compile_definitions(ps2kbd PRIVATE HDMI_PIO=1)
elseif(VIDEO_COMPOSITE)
    target_compile_definitions(ps2kbd PRIVATE VIDEO_COMPOSITE=1)
endif()

target_include_directories(ps2kbd PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_include_directories(ps2kbd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../platform/pico
)

pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.pio
)

pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr2.pio
)

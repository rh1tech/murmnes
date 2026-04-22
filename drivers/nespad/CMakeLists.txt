add_library(nespad INTERFACE)

target_sources(nespad INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/nespad.c
)

target_include_directories(nespad INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(nespad INTERFACE
    hardware_pio
    hardware_gpio
)

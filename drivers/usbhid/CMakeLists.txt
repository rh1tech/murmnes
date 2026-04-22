add_library(usbhid INTERFACE)

target_sources(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
)

target_include_directories(usbhid INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usbhid INTERFACE
    USB_HID_ENABLED=1
)

target_link_libraries(usbhid INTERFACE
    tinyusb_host
    tinyusb_board
)

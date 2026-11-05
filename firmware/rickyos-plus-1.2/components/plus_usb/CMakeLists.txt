set(USB_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../../../.local/usb-dependencies")
set(ESP_USB "${USB_DEPS}/esp-usb/device/esp_tinyusb")
# Use Espressif's S31 PHY/task/descriptor wrapper, with app-owned raw MSC callbacks.
# Its FatFS MSC backend is deliberately not linked: the application uses SdFat.
idf_component_register(
    SRCS "${USB_DEPS}/tinyusb.raw.c" "${ESP_USB}/descriptors_control.c"
         "${ESP_USB}/usb_descriptors.c" "${ESP_USB}/tinyusb_task.c"
         "${ESP_USB}/tinyusb_vbus_monitor.c"
    INCLUDE_DIRS "${ESP_USB}/include"
    PRIV_INCLUDE_DIRS "${ESP_USB}/include_private"
    REQUIRES tinyusb esp_hw_support esp_pm esp_driver_gpio
)
idf_component_get_property(TUSB_LIB tinyusb COMPONENT_LIB)
target_include_directories(${TUSB_LIB} PRIVATE "${ESP_USB}/include")
# Size policy is scoped to the USB stack; retain warnings/errors and assertions.
target_compile_options(${COMPONENT_LIB} PRIVATE -Oz -DLOG_LOCAL_LEVEL=1)
target_compile_options(${TUSB_LIB} PRIVATE -Oz)
target_compile_options(${COMPONENT_LIB} PRIVATE -flto=auto)
target_compile_options(${TUSB_LIB} PRIVATE -flto=auto)

cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.2.10")
if(NOT "$ENV{RICKYOS_PLUS_VERSION}" STREQUAL "")
  set(PROJECT_VER "$ENV{RICKYOS_PLUS_VERSION}")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../.local/arduino-esp32-4.0.0-RC1"
    "${CMAKE_CURRENT_LIST_DIR}/../../.local/dependencies/mdns"
    "${CMAKE_CURRENT_LIST_DIR}/../../.local/usb-dependencies/tinyusb")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(RickyOS-Plus-1.2)

# Apply Plus security policy to every Arduino STA connection, without modifying
# the shared/pinned Arduino dependency or changing SoftAP configuration.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
    "-Wl,--wrap=esp_wifi_set_config,-u,__wrap_esp_wifi_set_config")

# IDF's S31 HAL zero-initializes register unions; GCC C++ warns about the
# remaining fields. Match the vendor build for this one warning, scoped to
# Arduino rather than disabling errors for the application.
idf_component_get_property(ARDUINO_LIB arduino-esp32-4.0.0-RC1 COMPONENT_LIB)
target_compile_options(${ARDUINO_LIB} PRIVATE -Wno-missing-field-initializers)

idf_component_get_property(MDNS_LIB mdns COMPONENT_LIB)
target_link_libraries(${ARDUINO_LIB} PUBLIC ${MDNS_LIB})

# Preserve the original on-device panic ring capture.
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=panic_abort,--wrap=panic_print_backtrace" APPEND)

# IDF adds -fno-lto even when an application opts in. GCC's driver uses
# the presence of that flag to suppress its linker plugin, so remove it
# after project setup while retaining every other IDF link option.
idf_build_get_property(PLUS_LINK_OPTIONS LINK_OPTIONS)
list(REMOVE_ITEM PLUS_LINK_OPTIONS "-fno-lto")
idf_build_set_property(LINK_OPTIONS "${PLUS_LINK_OPTIONS}")
get_target_property(PLUS_ELF_LINK_OPTIONS ${CMAKE_PROJECT_NAME}.elf LINK_OPTIONS)
list(REMOVE_ITEM PLUS_ELF_LINK_OPTIONS "-fno-lto")
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf PROPERTY LINK_OPTIONS "${PLUS_ELF_LINK_OPTIONS}")

# Plus uses Espressif's S31 TinyUSB device wrapper, not Arduino's separate MSC owner.
get_target_property(PLUS_ARDUINO_SOURCES ${ARDUINO_LIB} SOURCES)
list(FILTER PLUS_ARDUINO_SOURCES EXCLUDE REGEX "/cores/esp32/USBMSC\\.cpp$")
set_property(TARGET ${ARDUINO_LIB} PROPERTY SOURCES "${PLUS_ARDUINO_SOURCES}")

# External pinned dependencies live outside PROJECT_DIR. Keep assertion paths
# relative too, preserving source/line diagnostics without workstation prefixes.
get_filename_component(PLUS_WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${PLUS_WORKSPACE_ROOT}/=" APPEND)
# LTO optimization is chosen again at final link; keep the application size policy.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -Oz)

# Perf build (RICKYOS_PERF=1): one [PERF] line per measured event (frame, page turn,
# panel push, chapter load, download), read by tools/rickyos-perf/perf.py. Never shipped.
if("$ENV{RICKYOS_PERF}" STREQUAL "1")
    idf_build_set_property(COMPILE_DEFINITIONS "RICKYOS_PERF=1" APPEND)
endif()
# Paint profile (RICKYOS_PAINT_PROFILE=1): per reader page, text/fill/image paint time
# and font cache statistics. Diagnostic only.
if("$ENV{RICKYOS_PAINT_PROFILE}" STREQUAL "1")
    idf_build_set_property(COMPILE_DEFINITIONS "RICKYOS_PAINT_PROFILE=1" APPEND)
endif()

# Explicit packet-header diagnostics; normal builds have no hooks or log code.
if("$ENV{RICKYOS_PLUS_NET_ALLOC_TRACE}" STREQUAL "1")
    idf_component_get_property(PLUS_NET_ALLOC_APP_LIB rickyos_application COMPONENT_LIB)
    target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_ALLOC_TRACE=1)
    target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--wrap=pbuf_alloc,--wrap=esp_netif_transmit_wrap,--wrap=esp_wifi_internal_tx,--wrap=esp_wifi_internal_tx_by_ref"
        "-Wl,-u,__wrap_pbuf_alloc,-u,__wrap_esp_netif_transmit_wrap,-u,__wrap_esp_wifi_internal_tx,-u,__wrap_esp_wifi_internal_tx_by_ref"
        "-Wl,--wrap=heap_caps_malloc,--wrap=heap_caps_calloc,--wrap=heap_caps_realloc"
        "-Wl,-u,__wrap_heap_caps_malloc,-u,__wrap_heap_caps_calloc,-u,__wrap_heap_caps_realloc")
endif()

# A single-factor experiment: use the SDK's copying API instead of retaining
# a pbuf reference. Keep every radio/configuration setting and deadline intact.
if("$ENV{RICKYOS_PLUS_NET_COPY_TX}" STREQUAL "1")
    if(NOT "$ENV{RICKYOS_PLUS_NET_ALLOC_TRACE}" STREQUAL "1")
        message(FATAL_ERROR "Copy-TX comparison requires allocation diagnostics")
    endif()
    target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_COPY_TX=1)
endif()

if("$ENV{RICKYOS_PLUS_NET_AUTORUN}" STREQUAL "1")
    if(NOT "$ENV{RICKYOS_PLUS_NET_ALLOC_TRACE}" STREQUAL "1")
        message(FATAL_ERROR "Automatic font-network comparison requires allocation diagnostics")
    endif()
    if(NOT "$ENV{RICKYOS_PLUS_NET_COPY_TX}" STREQUAL "1")
        message(FATAL_ERROR "Automatic A/B/A requires explicit copy-TX comparison support")
    endif()
    target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE
        RICKYOS_PLUS_NET_AUTORUN=1 RICKYOS_PLUS_RADIO_AUTOSTART=1)
    if("$ENV{RICKYOS_PLUS_NET_AX_COMPARE}" STREQUAL "1")
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_AX_COMPARE=1)
    endif()
    if("$ENV{RICKYOS_PLUS_NET_SECURITY_CHECK}" STREQUAL "1")
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_SECURITY_CHECK=1)
    endif()
    # Test-only credentials stay in an ignored local header, never the port or
    # saved-network store. Ordinary builds cannot include this profile.
    if("$ENV{RICKYOS_PLUS_NET_PRIVATE_PROFILE}" STREQUAL "1")
        if(NOT "$ENV{RICKYOS_PLUS_NET_SECURITY_CHECK}" STREQUAL "1")
            message(FATAL_ERROR "Private Wi-Fi test profile requires the explicit security diagnostic")
        endif()
        set(PLUS_PRIVATE_WIFI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.local/plus-private-wifi-test")
        if(NOT EXISTS "${PLUS_PRIVATE_WIFI_DIR}/PlusPrivateNetworkProfile.h")
            message(FATAL_ERROR "Local Wi-Fi test profile header is missing")
        endif()
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_PRIVATE_PROFILE=1)
        target_include_directories(${PLUS_NET_ALLOC_APP_LIB} PRIVATE "${PLUS_PRIVATE_WIFI_DIR}")
    endif()
    if("$ENV{RICKYOS_PLUS_NET_UI_AUTORUN}" STREQUAL "1")
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_UI_AUTORUN=1)
        if("$ENV{RICKYOS_PLUS_NET_AUTORUN_FROM_READER}" STREQUAL "1")
            target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_AUTORUN_FROM_READER=1)
        endif()
    endif()
    if("$ENV{RICKYOS_PLUS_NET_HEAP_POLICY}" STREQUAL "1")
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_HEAP_POLICY=1)
    endif()
    if("$ENV{RICKYOS_PLUS_NET_BODY_AUTORUN}" STREQUAL "1")
        if(NOT "$ENV{RICKYOS_PLUS_NET_UI_AUTORUN}" STREQUAL "1" OR
           NOT "$ENV{RICKYOS_PLUS_NETWORK_ACCEPTANCE}" STREQUAL "1")
            message(FATAL_ERROR "Font body comparison requires UI autorun and the existing network acceptance helper")
        endif()
        target_compile_definitions(${PLUS_NET_ALLOC_APP_LIB} PRIVATE RICKYOS_PLUS_NET_BODY_AUTORUN=1)
    endif()
endif()

if("$ENV{RICKYOS_PLUS_TCP_TRACE}" STREQUAL "1")
    idf_component_get_property(PLUS_APP_LIB rickyos_application COMPONENT_LIB)
    target_compile_definitions(${PLUS_APP_LIB} PRIVATE RICKYOS_PLUS_TCP_TRACE=1)
    target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--wrap=ip4_output_if,--wrap=ethernet_output,--wrap=ethernet_input"
        "-Wl,-u,__wrap_ip4_output_if,-u,__wrap_ethernet_output,-u,__wrap_ethernet_input")
endif()

if("$ENV{RICKYOS_PLUS_POWER_LOG}" STREQUAL "1")
    # Diagnostic only: append battery-gauge samples to /rickyos-power.csv on the SD card.
    idf_component_get_property(PLUS_POWER_APP_LIB rickyos_application COMPONENT_LIB)
    target_compile_definitions(${PLUS_POWER_APP_LIB} PRIVATE RICKYOS_PLUS_POWER_LOG=1)
endif()

if("$ENV{RICKYOS_PLUS_RADIO_COMPARE}" STREQUAL "1")
    if(NOT "$ENV{RICKYOS_PLUS_TCP_TRACE}" STREQUAL "1")
        message(FATAL_ERROR "Radio comparison requires RICKYOS_PLUS_TCP_TRACE=1")
    endif()
    idf_component_get_property(PLUS_RADIO_APP_LIB rickyos_application COMPONENT_LIB)
    target_compile_definitions(${PLUS_RADIO_APP_LIB} PRIVATE RICKYOS_PLUS_RADIO_COMPARE=1)
    if("$ENV{RICKYOS_PLUS_RADIO_TIMING}" STREQUAL "1")
        target_compile_definitions(${PLUS_RADIO_APP_LIB} PRIVATE RICKYOS_PLUS_RADIO_TIMING=1)
    endif()
    if("$ENV{RICKYOS_PLUS_RADIO_AUTOSTART}" STREQUAL "1")
        target_compile_definitions(${PLUS_RADIO_APP_LIB} PRIVATE RICKYOS_PLUS_RADIO_AUTOSTART=1)
    endif()
    if("$ENV{RICKYOS_PLUS_RADIO_RATE}" STREQUAL "1")
        target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
            "-Wl,--wrap=esp_wifi_init" "-Wl,-u,__wrap_esp_wifi_init")
    endif()
endif()

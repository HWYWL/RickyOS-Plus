idf_component_register(
    SRCS "src/FreeInkDisplay.cpp" "src/bus/EpdBus.cpp" "src/driver/MetalioPlusDriver.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../upstream/lib/hal"
    REQUIRES rickyos_platform metalio_board
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    FREEINK_FB_PSRAM=1 EINK_DISPLAY_SINGLE_BUFFER_MODE=1 BOARD_HAS_PSRAM=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# Diagnostic screen capture over the console (marketing screenshots, or alone with
# RICKYOS_PLUS_SCREEN_DUMP=1 to inspect what a page sent the panel); off in releases.
if(NOT "$ENV{RICKYOS_PLUS_SCREEN_TOUR}" STREQUAL "" OR "$ENV{RICKYOS_PLUS_SCREEN_DUMP}" STREQUAL "1")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE RICKYOS_PLUS_SCREEN_DUMP=1)
endif()
if("$ENV{RICKYOS_PLUS_GRAY_CALIBRATION}" STREQUAL "1")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE RICKYOS_PLUS_GRAY_CALIBRATION=1)
endif()

# Bounded optical/feeder exercise before application startup; never a release default.
if("$ENV{RICKYOS_PLUS_TURN_SELFTEST}" STREQUAL "1")
 target_compile_definitions(${COMPONENT_LIB} PRIVATE RICKYOS_PLUS_TURN_SELFTEST=1)
endif()

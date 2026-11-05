# Original application entry lives in the application component, including its text helpers.
idf_component_register(SRCS "runtime.cc" INCLUDE_DIRS "."
    REQUIRES arduino-esp32-4.0.0-RC1 rickyos_application)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

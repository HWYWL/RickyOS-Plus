add_executable(ZipIndexTest
  ZipIndexTest.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${crosspoint_simulator_SOURCE_DIR}/src/HalStorage.cpp
  ${crosspoint_simulator_SOURCE_DIR}/src/HalMemory.cpp
  ${crosspoint_simulator_SOURCE_DIR}/src/ESP.cpp
)

target_include_directories(ZipIndexTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${crosspoint_simulator_SOURCE_DIR}/src
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/miniz/src
)

target_compile_definitions(ZipIndexTest PRIVATE
  CROSSPOINT_EMULATED=1
  DESTRUCTOR_CLOSES_FILE=1
)

target_link_libraries(ZipIndexTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ZipIndexTest PROPERTIES RUN_SERIAL TRUE)

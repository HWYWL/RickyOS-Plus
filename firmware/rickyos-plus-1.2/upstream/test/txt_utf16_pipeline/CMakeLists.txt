add_executable(TxtUtf16PipelineTest
  "${REPO_ROOT}/test/txt_utf16_pipeline/TxtUtf16PipelineTest.cpp"
  "${REPO_ROOT}/lib/Txt/Txt.cpp" "${REPO_ROOT}/lib/Txt/TxtCache.cpp"
  "${REPO_ROOT}/lib/Txt/TxtToHtml.cpp" "${REPO_ROOT}/lib/Txt/MarkdownFormatter.cpp"
  "${REPO_ROOT}/lib/Txt/TxtChapterIndex.cpp" "${REPO_ROOT}/lib/Txt/TxtEncoding.cpp"
  "${REPO_ROOT}/lib/Txt/TxtProgress.cpp" "${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp"
  "${REPO_ROOT}/lib/Utf8/Utf8.cpp" "${crosspoint_simulator_SOURCE_DIR}/src/HalStorage.cpp")
target_include_directories(TxtUtf16PipelineTest BEFORE PRIVATE
  "${crosspoint_simulator_SOURCE_DIR}/src" "${REPO_ROOT}/lib/Txt" "${REPO_ROOT}/lib/FsHelpers"
  "${REPO_ROOT}/lib/Epub" "${REPO_ROOT}/lib/Serialization" "${REPO_ROOT}/lib/Utf8"
  "${REPO_ROOT}/lib/JpegToBmpConverter" "${REPO_ROOT}/lib/PngToBmpConverter"
  "${REPO_ROOT}/test/chapter_html_slim_parser/stubs")
target_compile_options(TxtUtf16PipelineTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
 target_link_options(TxtUtf16PipelineTest PRIVATE -Wl,-dead_strip)
else()
 target_link_options(TxtUtf16PipelineTest PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(TxtUtf16PipelineTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TxtUtf16PipelineTest PROPERTIES RUN_SERIAL TRUE)

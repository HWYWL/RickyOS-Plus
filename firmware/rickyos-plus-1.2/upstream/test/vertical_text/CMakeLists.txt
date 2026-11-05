add_executable(vertical_text_test VerticalTextTest.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(vertical_text_test PRIVATE ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Epub)
target_link_libraries(vertical_text_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(vertical_text_test)

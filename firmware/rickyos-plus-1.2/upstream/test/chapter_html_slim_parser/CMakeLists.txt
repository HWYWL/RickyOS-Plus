add_executable(ChapterHtmlSlimParserTest
  ChapterHtmlSlimParserTest.cpp
  ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookFonts.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# The section cache is a binary format: the test normalizes it back to a pinned
# v70 digest and must also assert the live version byte. Read that byte from the
# production declaration instead of restating it, so bumping SECTION_FILE_VERSION
# updates the test automatically rather than leaving a stale magic number.
file(READ ${REPO_ROOT}/lib/Epub/Epub/Section.cpp SECTION_CPP_SOURCE)
string(REGEX MATCHALL "constexpr uint8_t SECTION_FILE_VERSION = [0-9]+" _section_versions "${SECTION_CPP_SOURCE}")
if(RICKYOS_TEST_DEVICE STREQUAL "plus")
  list(GET _section_versions 2 _section_declaration)
elseif(RICKYOS_TEST_DEVICE STREQUAL "readpico")
  list(GET _section_versions 1 _section_declaration)
else()
  list(GET _section_versions 0 _section_declaration)
endif()
string(REGEX MATCH "= ([0-9]+)$" _section_version_match "${_section_declaration}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "Could not read SECTION_FILE_VERSION from lib/Epub/Epub/Section.cpp")
endif()
target_compile_definitions(ChapterHtmlSlimParserTest PRIVATE
  SECTION_VERSION_UNDER_TEST=${CMAKE_MATCH_1}
)

target_include_directories(ChapterHtmlSlimParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(ChapterHtmlSlimParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(ChapterHtmlSlimParserTest)

add_executable(UpstreamParserTest
  UpstreamParserTest.cpp
  UpstreamParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookFonts.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(UpstreamParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/upstream_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(UpstreamParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(UpstreamParserTest TEST_PREFIX "upstream.")

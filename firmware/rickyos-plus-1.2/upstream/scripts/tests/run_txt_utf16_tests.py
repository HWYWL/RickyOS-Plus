#!/usr/bin/env python3
"""Focused real TXT pipeline tests; reuse explicitly supplied host dependencies.

No device access/downloads. Optional local sample paths are supplied through
RICKYOS_UTF16_SAMPLE and RICKYOS_UTF8_REFERENCE, never stored in the repository.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--app-root',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--gtest-source',type=Path,required=True)
p.add_argument('--simulator-source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--cmake',default=shutil.which('cmake'))
p.add_argument('--ctest',default=shutil.which('ctest'))
a=p.parse_args()
if not a.cmake or not a.ctest:p.error('cmake and ctest paths are required')
for folder in [a.app_root,a.gtest_source,a.simulator_source]:
 if not folder.is_dir():p.error(f'missing source folder: {folder}')
def quote(path):return str(path.resolve()).replace('\\','/').replace('"','\\"')
source=a.build_dir.resolve()/'driver';build=a.build_dir.resolve()/'build';source.mkdir(parents=True,exist_ok=True)
(source/'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(txt_utf16_regressions C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO_ROOT "{quote(a.app_root)}")
set(crosspoint_simulator_SOURCE_DIR "{quote(a.simulator_source)}")
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("{quote(a.gtest_source)}" googletest)
add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE "${{REPO_ROOT}}" "${{REPO_ROOT}}/lib" "${{REPO_ROOT}}/lib/Memory")
target_compile_definitions(crosspoint_test_common INTERFACE RICKYOS_PRODUCT=1 ENABLE_CHINESE_VERSION=1 CROSSPOINT_EMULATED=1)
target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
enable_testing()
include(GoogleTest)
add_subdirectory("${{REPO_ROOT}}/test/txt_encoding" encoding)
add_subdirectory("${{REPO_ROOT}}/test/txt_to_html" html)
add_subdirectory("${{REPO_ROOT}}/test/txt_chapter_index" chapters)
add_subdirectory("${{REPO_ROOT}}/test/txt_utf16_pipeline" pipeline)
''')
subprocess.run([a.cmake,'-S',str(source),'-B',str(build),'-DCMAKE_BUILD_TYPE=Debug',
 '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer',
 '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'],check=True)
subprocess.run([a.cmake,'--build',str(build),'-j','4'],check=True)
subprocess.run([a.ctest,'--test-dir',str(build),'--output-on-failure'],check=True)

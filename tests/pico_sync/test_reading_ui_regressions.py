"""Run production home geometry, headers, resource errors and sync refresh with small seams."""
from pathlib import Path
import importlib.util
import json
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WEREAD = ROOT / 'src/activities/apps/weread/webapi'


def method(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# The sources are RickyOS-only (Read Pico, Chinese, high-density UI): every check builds
# with the same product switches the firmware does, whatever the test asks for besides.
PRODUCT_DEFINES = ('RICKYOS_PRODUCT=1', 'ENABLE_CHINESE_VERSION=1', 'CROSSMUX_UI_PROFILE_HIGH_DPI=1')


def run_cpp(program, include_dirs=(), defines=()):
    defines = tuple(d for d in PRODUCT_DEFINES
                    if d.split('=')[0] not in {x.split('=')[0] for x in defines}) + tuple(defines)
    with tempfile.TemporaryDirectory(prefix='reading-ui-') as directory:
        cpp = Path(directory) / 'check.cpp'
        exe = Path(directory) / 'check'
        cpp.write_text(f'#include "{ROOT}/src/components/UiHighDpiProfile.h"\n'
                       f'#include "{ROOT}/src/components/RickyDesignTokens.h"\n' + program)
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        *('-I'+str(path) for path in include_dirs), *('-D'+define for define in defines), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

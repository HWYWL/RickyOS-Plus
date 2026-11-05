"""Compile the real app catalog in product/stock profiles and preserve stable IDs."""
import configparser
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DIRECTORIES = ('sudoku', 'sokoban', 'chinese-chess', 'minesweeper', '2048', 'avatar', 'buddy', 'pixel-switch', 'woodfish',
               'airpage')


class RickyAppProfileTest(unittest.TestCase):
    def test_catalog_profiles_and_visibility_masks(self):
        source = (ROOT / 'src/activities/apps/AppsMenuActivity.cpp').read_text()
        catalog = source.split('using appVisibility::appBit;', 1)[1].split('}  // namespace', 1)[0]
        catalog = 'using appVisibility::appBit;' + catalog
        routes = sorted(set(re.findall(r'&ActivityManager::(\w+)', catalog)))
        titles = sorted(set(re.findall(r'StrId::(\w+)', catalog)))
        icons = sorted(set(re.findall(r'UIIcon::(\w+)', catalog)))
        program = '''
#include "AppVisibility.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
enum class StrId { TITLES };
enum class UIIcon { ICONS };
struct ActivityManager { ROUTES };
CATALOG
int main() {
  using appVisibility::AppId;
  using appVisibility::appBit;
  constexpr AppId removed[] = {AppId::Sudoku, AppId::Sokoban, AppId::ChineseChess,
                             AppId::Minesweeper, AppId::Game2048, AppId::UglyAvatar,
                             AppId::Buddy, AppId::PixelSwitch, AppId::Woodfish, AppId::AirPage,
                             AppId::OpdsBrowser};  // OPDS left in 1.2
  constexpr AppId tools[] = {AppId::Gomoku, AppId::Calculator,
                             AppId::FileTransfer, AppId::ReadingStats,
                             AppId::Standby};
  for (auto id : tools) {
    bool found=false;
    for (auto entry : kAppEntries) found |= entry.id == id;
    assert(found);
  }
  for (auto id : removed) {
    bool found=false;
    for (auto entry : kAppEntries) found |= entry.id == id;
#ifdef RICKYOS_PRODUCT
    assert(!found);
#else
#ifdef ENABLE_CHINESE_VERSION
    assert(found);
#else
    assert(found == (id != AppId::ChineseChess));
#endif
#endif
  }
#ifdef RICKYOS_PRODUCT
#ifdef ENABLE_CHINESE_VERSION
  static_assert(kAppCount == (FREEINK_DEVICE_METALIO_PLUS ? 7 : 6));  // Plus also has Recorder
#else
  static_assert(kAppCount == 6);
#endif
#else
#ifdef ENABLE_CHINESE_VERSION
  static_assert(kAppCount == 17);
#else
  static_assert(kAppCount == 15);
#endif
#endif
  constexpr AppId expected[] = {AppId::FileTransfer,
#ifdef ENABLE_CHINESE_VERSION
    AppId::WeRead,
#endif
#ifndef RICKYOS_PRODUCT
    AppId::AirPage,
#endif
    AppId::ReadingStats,
#ifndef RICKYOS_PRODUCT
    AppId::Sudoku,
#endif
    AppId::Gomoku,
#ifndef RICKYOS_PRODUCT
    AppId::Sokoban,
#ifdef ENABLE_CHINESE_VERSION
    AppId::ChineseChess,
#endif
    AppId::Minesweeper, AppId::Game2048, AppId::UglyAvatar, AppId::Buddy, AppId::PixelSwitch,
#endif
    AppId::Calculator,
#if FREEINK_DEVICE_METALIO_PLUS
    AppId::Recorder,
#endif
#ifndef RICKYOS_PRODUCT
    AppId::Woodfish,
#endif
    AppId::Standby};
  static_assert(sizeof(expected) / sizeof(expected[0]) == kAppCount);
  for (int i=0; i<kAppCount; ++i) assert(kAppEntries[i].id == expected[i]);
  // Every persisted visibility combination still maps to the correct sparse IDs.
  for (uint32_t mask=0; mask<(1u<<18); ++mask) {
    int visible=0;
    for (int i=0;i<kAppCount;++i) {
      if (mask & appBit(kAppEntries[i].id)) continue;
      assert(appIndexForVisibleIndex(mask,visible++) == i);
    }
    assert(visibleAppCount(mask)==visible);
    assert(appIndexForVisibleIndex(mask,visible)==-1);
  }
  static_assert(static_cast<uint8_t>(AppId::Gomoku)==3);
  static_assert(static_cast<uint8_t>(AppId::Sokoban)==11);
  static_assert(static_cast<uint8_t>(AppId::Recorder)==17);
  static_assert(static_cast<uint8_t>(AppId::Count)==18);
}
'''
        program = program.replace('TITLES', ','.join(titles)).replace('ICONS', ','.join(icons))
        program = program.replace('ROUTES', ' '.join(f'void {route}() {{}}' for route in routes))
        program = program.replace('CATALOG', catalog)
        with tempfile.TemporaryDirectory(prefix='ricky-app-profile-') as directory:
            cpp = Path(directory) / 'catalog.cpp'
            cpp.write_text(program)
            for product in (True,):  # RickyOS is the only product: Chinese, its own catalog
                for chinese in (True,):
                    for plus in (False, True):
                        with self.subTest(product=product, chinese=chinese, plus=plus):
                            binary = Path(directory) / f'catalog-{product}-{chinese}-{plus}'
                            flags = (['-DRICKYOS_PRODUCT=1'] if product else [])
                            flags += ['-DENABLE_CHINESE_VERSION=1'] if chinese else []
                            flags += [f'-DFREEINK_DEVICE_METALIO_PLUS={int(plus)}']
                            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                            '-I' + str(ROOT / 'src'), *flags, str(cpp), '-o', str(binary)], check=True)
                            subprocess.run([str(binary)], check=True)

    def test_only_product_builds_exclude_game_sources(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / 'platformio.ini')
        excludes = config['rickyos_app_profile']['build_src_filter']
        for directory in DIRECTORIES:
            self.assertIn(f'-<activities/apps/{directory}/>', excludes)
        self.assertNotIn('gomoku', excludes)
        for environment in ('env:rickyos_readpico', 'env:simulator_rickyos'):
            self.assertIn('${rickyos_app_profile.build_src_filter}', config[environment]['build_src_filter'])
            self.assertIn('-DRICKYOS_PRODUCT=1', config[environment]['build_flags'])
        self.assertIn('${env:simulator.build_src_filter}', config['env:simulator_rickyos']['build_src_filter'])
        for environment in ('env:readpico', 'env:simulator_readpico', 'base', 'env:simulator'):
            self.assertNotIn('rickyos_app_profile', str(dict(config[environment])))


if __name__ == '__main__':
    unittest.main()

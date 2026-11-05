"""Owned reader-resource routing; offline, without changing device data."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_reading_ui_regressions import method as body

ROOT = Path(__file__).resolve().parents[2]


class ReaderResourcesTest(unittest.TestCase):
    def test_production_routes_and_origin_guard(self):
        program = r'''
#include "RickyReaderResources.h"
#include "DictionaryResource.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace RickyReaderResources;
int main() {
  assert(!isOwnedUrl(nullptr) && !isOwnedUrl(""));
#ifdef TEST_DISABLED
  static_assert(!ENABLED);
  assert(!isOwnedUrl("/dictionaries/m1/"));
#else
  static_assert(ENABLED);
  const std::string base=RICKYOS_COS_BASE;
  assert(isOwnedUrl((base+"/dictionaries/m1/v1.0.0/").c_str()));
  for (size_t i=0;i<=base.size();++i) assert(!isOwnedUrl(base.substr(0,i).c_str()));
  for (const char* suffix : {".evil.example/", "@evil.example/", ":443/", "?x=/", "#x=/"})
    assert(!isOwnedUrl((base+suffix).c_str()));
  assert(!isOwnedUrl("https://crossmux.cn/api/assets/dictionaries/manifest"));
  assert(!isOwnedUrl("https://evil.example/"));
  assert(!isOwnedUrl("http://rickyos-1309217881.cos.ap-guangzhou.myqcloud.com/"));
  char font[160], dictionary[192];
  const int a=snprintf(font,sizeof(font),FONT_MANIFEST_FORMAT,"1","4");
  const int b=snprintf(dictionary,sizeof(dictionary),DICTIONARY_MANIFEST_FORMAT,
                       unsigned(DictionaryResource::MANIFEST_VERSION));
  assert(a>0 && a<int(sizeof(font)) && b>0 && b<int(sizeof(dictionary)));
  assert(std::string(font)==base+"/font-fallback/m1-b4/cos.json");
  assert(std::string(dictionary)==base+"/dictionaries/m1/cos.json");
#endif
}
'''
        with tempfile.TemporaryDirectory(prefix='reader-resources-') as directory:
            cpp = Path(directory) / 'check.cpp'
            exe = Path(directory) / 'check'
            cpp.write_text(program)
            for flags in ([], ['-DRICKYOS_COS_BASE=""', '-DTEST_DISABLED=1'],
                          ['-DRICKYOS_COS_BASE="https://resources.example.test"']):
                subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                *flags, '-I'+str(ROOT/'src/network'), '-I'+str(ROOT/'src/util'),
                                str(cpp), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)

    def test_downloader_wiring_is_owned_and_bounded(self):
        font = (ROOT/'src/activities/settings/FontDownloadActivity.cpp').read_text()
        dictionary = (ROOT/'src/activities/settings/DictionaryDownloadActivity.cpp').read_text()
        for source in (font, dictionary):
            self.assertNotIn('CrossMuxEndpoints', source)
            self.assertIn('RickyReaderResources::ENABLED', source)
            self.assertIn('RickyReaderResources::isOwnedUrl(baseUrl_.c_str())', source)
            self.assertIn('manifestUrlLength', source)
        self.assertIn('RickyReaderResources::FONT_MANIFEST_FORMAT', font)
        self.assertIn('purpose_ == Purpose::ReaderAutoInstall && !RickyReaderResources::isOwnedUrl(url)', font)
        self.assertIn('if (purpose_ != Purpose::ReaderAutoInstall)', font)
        self.assertIn('RickyFontCatalog::manifestFor(attempt)', font)
        self.assertIn('HttpDownloader::downloadIndex(manifestUrl, MANIFEST_TMP)', dictionary)
        self.assertIn('static_cast<unsigned>(DictionaryResource::MANIFEST_VERSION)', dictionary)
        self.assertIn('baseUrl_.size() > 256', dictionary)
        self.assertIn('dictionaries.size() > DictionaryResource::MAX_ITEMS', dictionary)
        self.assertIn('manifestFile.fileSize() > MAX_MANIFEST_BYTES', dictionary)
        self.assertIn('I18N.getLanguage() == Language::ZH_CN', dictionary)
        self.assertIn('object["nameZh"]', dictionary)
        self.assertIn('localizedName[0] ? localizedName : (object["name"] | "")', dictionary)

    def test_existing_installation_and_manual_conflicts_remain_compatible(self):
        source = (ROOT/'src/activities/settings/DictionaryDownloadActivity.cpp').read_text()
        self.assertIn('.crossmux-resource', source)
        self.assertIn('DictionaryResource::classify', source)
        self.assertIn('DictionaryResource::isValidFileName', source)
        self.assertIn('DictionaryResource::isValidId', source)
        fetch = body(source, 'bool DictionaryDownloadActivity::fetchAndParseManifest()')
        self.assertIn('recoverInstall(item.id.c_str());', fetch)
        self.assertIn('refreshLocalState(item);', fetch)
        self.assertIn('fileObject["crc32"].is<uint32_t>()', fetch)
        self.assertNotIn('removeDictionary(', fetch)
        # Only normal catalog loading changed; no new automatic installation path.
        self.assertNotIn('downloadDictionary(', fetch)


if __name__ == '__main__':
    unittest.main()

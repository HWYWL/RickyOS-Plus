"""RickyOS flash trim: Chinese + English UI strings and English-only hyphenation."""
import configparser
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRODUCT_ENVIRONMENTS = ('env:rickyos_readpico', 'env:simulator_rickyos')
UPSTREAM_SECTIONS = ('env:readpico', 'env:simulator_readpico', 'base', 'env:simulator')


class RickyFlashTrimConfigTest(unittest.TestCase):
    def test_only_product_environments_use_the_trim(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / 'platformio.ini')
        trim = config['rickyos_flash_trim']
        self.assertEqual(trim['custom_i18n_builtin_langs'].strip(), 'zh_cn')
        self.assertIn('-DHYPHENATION_ENGLISH_ONLY', trim['build_flags'])
        for environment in PRODUCT_ENVIRONMENTS:
            with self.subTest(environment=environment):
                section = config[environment]
                self.assertIn('${rickyos_flash_trim.build_flags}', section['build_flags'])
                self.assertEqual(section['custom_i18n_builtin_langs'].strip(),
                                 '${rickyos_flash_trim.custom_i18n_builtin_langs}')
        self.assertEqual(config['base']['custom_i18n_builtin_langs'].strip(), 'all')
        for environment in UPSTREAM_SECTIONS:
            with self.subTest(environment=environment):
                self.assertNotIn('rickyos_flash_trim', str(dict(config[environment])))
                self.assertNotIn('HYPHENATION_ENGLISH_ONLY', str(dict(config[environment])))


class RickyFlashTrimI18nTest(unittest.TestCase):
    def test_generator_keeps_enum_and_compiles_only_chinese_and_english(self):
        with tempfile.TemporaryDirectory(prefix='ricky-i18n-') as directory:
            subprocess.run([sys.executable, str(ROOT / 'scripts/gen_i18n.py'), str(ROOT / 'lib/I18n/translations'),
                            directory, '--strip-unused', '--src-dirs', str(ROOT / 'src'), str(ROOT / 'lib'),
                            '--builtin-langs', 'zh_cn'],
                           check=True, capture_output=True, cwd=ROOT)
            strings = (Path(directory) / 'I18nStrings.cpp').read_text(encoding='utf-8')
            keys = (Path(directory) / 'I18nKeys.h').read_text(encoding='utf-8')
        self.assertIn('STRINGS_EN_DATA[] =', strings)
        self.assertIn('STRINGS_ZH_CN_DATA[] =', strings)
        for dropped in ('DE', 'FR', 'RU', 'ES', 'AR'):
            self.assertNotIn(f'STRINGS_{dropped}_DATA[] =', strings)
            # Saved settings and keyboard layouts still name every language.
            self.assertRegex(keys, rf'\n  {dropped}\s+= \d+,')
        picker = keys.split('SORTED_LANGUAGE_INDICES[] = {', 1)[1].split('}', 1)[0]
        self.assertEqual(len(picker.split(',')), 2)


HYPHENATION_PROGRAM = r'''
#include <cassert>
#include <string>
#include "Epub/hyphenation/Hyphenator.h"
#include "Epub/hyphenation/LanguageRegistry.h"

static bool hasPatternBreak(const char* word) {
  for (const auto& info : Hyphenator::breakOffsets(word, false)) {
    if (info.requiresInsertedHyphen) return true;
  }
  return false;
}

int main() {
  assert(getLanguageHyphenatorForPrimaryTag("en") != nullptr);
  Hyphenator::setPreferredLanguage("en-US");
  assert(hasPatternBreak("hyphenation"));
  Hyphenator::setPreferredLanguage("deu");
  assert(Hyphenator::breakOffsets("US-Satellitensystems", false).size() >= 1);
#ifdef HYPHENATION_ENGLISH_ONLY
  assert(getLanguageEntries().size == 1);
  assert(getLanguageHyphenatorForPrimaryTag("de") == nullptr);
  assert(getLanguageHyphenatorForPrimaryTag("ru") == nullptr);
  assert(!hasPatternBreak("Quadratkilometer"));
#else
  assert(getLanguageEntries().size == 11);
  assert(getLanguageHyphenatorForPrimaryTag("de") != nullptr);
  assert(hasPatternBreak("Quadratkilometer"));
#endif
}
'''


class RickyFlashTrimHyphenationTest(unittest.TestCase):
    def test_english_only_registry_falls_back_to_explicit_breaks(self):
        hyphenation = ROOT / 'lib/Epub/Epub/hyphenation'
        sources = [hyphenation / name for name in
                   ('Hyphenator.cpp', 'LanguageRegistry.cpp', 'LiangHyphenation.cpp', 'HyphenationCommon.cpp')]
        sources.append(ROOT / 'lib/Utf8/Utf8.cpp')
        with tempfile.TemporaryDirectory(prefix='ricky-hyphenation-') as directory:
            program = Path(directory) / 'registry.cpp'
            program.write_text(HYPHENATION_PROGRAM)
            for english_only in (True,):  # RickyOS builds English-only Liang hyphenation
                with self.subTest(english_only=english_only):
                    binary = Path(directory) / f'registry-{english_only}'
                    flags = ['-DHYPHENATION_ENGLISH_ONLY'] if english_only else []
                    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                    '-I' + str(ROOT / 'lib/Epub'), '-I' + str(ROOT / 'lib/Utf8'),
                                    *flags, str(program), *map(str, sources), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

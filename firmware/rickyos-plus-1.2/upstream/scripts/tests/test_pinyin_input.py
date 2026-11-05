"""Compile the actual decoder with sanitizer and verify shipped glyph coverage."""
import pathlib
import re
import subprocess
import tempfile
import unittest
import importlib.util

ROOT = pathlib.Path(__file__).resolve().parents[2]


class PinyinTests(unittest.TestCase):
    def test_activity_commit_byte_limit(self):
        # Compile actual activity methods, not an independently reimplemented
        # commit model; collaborators omitted here are only layout publication.
        source = (ROOT / "src/activities/util/KeyboardEntryActivity.cpp").read_text()
        methods = []
        for name in ('insertUtf8', 'commitIme'):
            methods.append(re.search(r'^[^\n]*KeyboardEntryActivity::' + name + r'\(.*?\n}', source, re.M | re.S).group())
        code = '''
#include <cassert>
#include <cstring>
#include <string>
#include "PinyinInput.h"
class KeyboardEntryActivity {
 public:
  PinyinInput ime;
  std::string text;
  size_t maxLength = 5, cursorPos = 0;
  void insertUtf8(const char*);
  bool commitIme(size_t);
  void rebuildImeLayout() {}
  void clampSelection() {}
};
''' + '\n'.join(methods) + '''
int main() {
  KeyboardEntryActivity activity;
  for (char c : std::string("nihao")) assert(activity.ime.append(c));
  assert(!activity.commitIme(0));
  assert(activity.text.empty() && !strcmp(activity.ime.raw(), "nihao"));
  activity.maxLength = 6;
  assert(activity.commitIme(0));
  assert(activity.text == "你好" && activity.ime.empty() && activity.cursorPos == 6);
  assert(!activity.commitIme(99));
}
'''
        with tempfile.TemporaryDirectory(prefix="ricky-ime-activity-test-") as folder:
            cpp, exe = pathlib.Path(folder) / 'test.cpp', pathlib.Path(folder) / 'test'
            cpp.write_text(code)
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(ROOT / "src/util"), str(ROOT / "src/util/PinyinInput.cpp"),
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_umlaut_normalization(self):
        spec = importlib.util.spec_from_file_location("pinyin_recipe", ROOT / "scripts/build_pinyin_dictionary.py")
        recipe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recipe)
        for value, expected in [('lǜ', 'lv'), ('nǚ', 'nv'), ('lüè', 'lve'), ('luò', 'luo')]:
            self.assertEqual(recipe.normalize(value), expected)

    def test_decoder(self):
        with tempfile.TemporaryDirectory(prefix="ricky-pinyin-test-") as folder:
            exe = pathlib.Path(folder) / "test"
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(ROOT / "src/util"), str(ROOT / "src/util/PinyinInput.cpp"),
                            str(ROOT / "test/pinyin/PinyinInputTest.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_candidates_have_shipped_glyphs(self):
        supported = set((ROOT / "lib/EpdFont/scripts/cn_rickyos_chars.txt").read_text())
        header = (ROOT / "src/util/PinyinDictionary.h").read_text()
        values = re.search(r"characters\[\] = \{(.*?)\};", header, re.S).group(1)
        points = [int(p) for p in re.findall(r"\d+", values)]
        self.assertGreater(len(set(points)), 3500)
        self.assertTrue(all(chr(p) in supported for p in points))
        phrases = (ROOT / "src/util/PinyinInput.cpp").read_text().split("const Syllable* find")[0]
        for value in re.findall(r'\{"[a-z]+", "([^"]+)"\}', phrases):
            self.assertTrue(all(c in supported for c in value), value)


if __name__ == "__main__":
    unittest.main()

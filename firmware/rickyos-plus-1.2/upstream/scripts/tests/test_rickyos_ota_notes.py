"""RickyOS update notes: a separate ota-notes.txt that old devices never see."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import run_cpp

ROOT = Path(__file__).resolve().parents[2]


class RickyOtaNotesTest(unittest.TestCase):
    def test_notes_parse_and_fail_closed(self):
        program = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "network/RickyOtaNotes.h"
using Notes = std::array<std::array<char, 97>, 8>;
size_t parse(Notes& notes, const std::string& text, const char* version, size_t chunk = 7) {
  RickyOtaNotes<97> reader(notes, version);
  for (size_t i = 0; i < text.size(); i += chunk) {
    const std::string part = text.substr(i, chunk);
    if (!reader.feed(reinterpret_cast<const uint8_t*>(part.data()), part.size())) break;
  }
  return reader.count();
}
int main() {
  Notes notes{};
  assert(parse(notes, "1.1.3\n待机新增日历时钟\r\n\n短按侧键待机，长按关机\n", "1.1.3") == 2);
  assert(!strcmp(notes[0].data(), "待机新增日历时钟") && !strcmp(notes[1].data(), "短按侧键待机，长按关机"));
  assert(parse(notes, "1.1.3\n", "1.1.3") == 0);
  assert(parse(notes, "1.1.2\nold notes\n", "1.1.3") == 0);       // another version's notes
  assert(parse(notes, "1.1.3\nunterminated", "1.1.3") == 0);      // cut off mid-line
  assert(parse(notes, "1.1.3\n" + std::string(97, 'a') + "\n", "1.1.3") == 0);  // longer than a note
  assert(parse(notes, "1.1.3\n" + std::string(96, 'a') + "\n", "1.1.3") == 1);
  assert(parse(notes, std::string("1.1.3\nbad\x01note\n"), "1.1.3") == 0);
  std::string many = "1.1.3\n";
  for (int i = 0; i < 12; ++i) many += "note\n";
  assert(parse(notes, many, "1.1.3") == 8);
  assert(parse(notes, "1.1.3\n" + std::string(3000, 'a'), "1.1.3", 1024) == 0);  // bounded
}
'''
        run_cpp(program, include_dirs=(ROOT / 'src',))

    def test_offer_fetches_notes_without_depending_on_them(self):
        source = (ROOT / 'src/network/OtaUpdater.cpp').read_text()
        product = source[source.index('  (void)requestedChannel;'):]
        product = product[:product.index('\n}\n')]
        # Notes come from the route that made the offer, only after the offer stands.
        self.assertLess(product.index('updateAvailable = true;'), product.index('route->notes'))
        self.assertIn('releaseNoteCount = notes.count();', product)
        manifest = (ROOT / 'src/network/RickyOtaManifest.h').read_text()
        self.assertNotIn('"notes"', manifest)


if __name__ == '__main__':
    unittest.main()

"""Compile the production partial-cache guard; native QA covers real paging."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import run_cpp

ROOT = Path(__file__).resolve().parents[2]


class MarkdownPartialNavigationTest(unittest.TestCase):
    def test_partial_cache_checks_the_requested_offset_not_page_zero(self):
        source = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        start = source.index('if (section->isPartial() &&')
        guard = source[start:source.index('{', start)].strip()
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
struct Section {
  bool partial=true;
  unsigned pageCount=9;
  mutable int offsetCalls=0;
  bool isPartial() const {return partial;}
  std::optional<int> getPageForVisibleTextOffset(uint32_t offset) const {
    ++offsetCalls;
    return offset<=100 ? std::optional<int>(3) : std::nullopt;
  }
  std::optional<int> getPageForAnchor(const std::string& anchor) const {
    return anchor=="cached" ? std::optional<int>(2) : std::nullopt;
  }
};
bool covers(Section* section, std::optional<uint32_t> offsetJump={},
            bool anchorJump=false, std::string pendingAnchor="", int target=0) {
  constexpr int kOpenMargin=0;
''' + guard + r''' {return true;}
  return false;
}
int main() {
  Section section;
  // A chapter beyond the known prefix must extend, even with nextPageNumber=0.
  assert(!covers(&section, 101));
  assert(covers(&section, 100));
  assert(!covers(&section, 101, true, "cached")); // offset wins over an anchor/page
  assert(covers(&section));
  assert(!covers(&section, {}, false, "", 9));
  assert(covers(&section, {}, true, "cached"));
  assert(!covers(&section, {}, true, "missing"));
  section.partial=false;
  const int calls=section.offsetCalls;
  assert(!covers(&section, 50));
  assert(section.offsetCalls==calls); // complete sections do not query this guard
}
''')


if __name__ == '__main__':
    unittest.main()

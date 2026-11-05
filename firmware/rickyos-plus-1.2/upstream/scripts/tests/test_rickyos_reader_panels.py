"""RickyOS reader panels: bookmarks beside the contents, stroke weight in the Text panel."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]
READER = ROOT / 'src/activities/reader/EpubReaderActivity.cpp'
TOOLBAR = ROOT / 'src/activities/reader/ReaderToolbarUi.cpp'


class RickyReaderPanelsTest(unittest.TestCase):
    def test_contents_and_bookmarks_are_two_segments_of_one_panel(self):
        reader = READER.read_text()
        render = method(reader, 'void EpubReaderActivity::renderOverlay()')
        segments = render[render.index('model.segmentLabels[0] = tr(STR_TOOL_CONTENTS);'):]
        self.assertIn('model.segmentLabels[1] = tr(STR_BOOKMARKS);', segments)
        # Both segments size the sheet for the longer list, so switching does not move it.
        self.assertIn('model.minRows = std::max(epub->getTocItemsCount(), bookmarkRowCount());', segments)
        self.assertIn('case ReaderToolbarUi::Event::Segment:', reader)
        # The toolbar app holds six handlers, all taken: segments ride the tool action.
        toolbar = TOOLBAR.read_text()
        self.assertIn('constexpr int16_t kSegmentToolBase = 16;', toolbar)
        self.assertIn('ACTION_TOOL, static_cast<int16_t>(kSegmentToolBase + i)', toolbar)
        self.assertIn('for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_ROW; ++id)', toolbar)

    def test_bookmark_rows_toggle_jump_and_manage(self):
        reader = READER.read_text()
        self.assertIn('return 1 + saved + (saved > 0 ? 1 : 0);', method(reader, 'int EpubReaderActivity::bookmarkRowCount()'))
        activate = method(reader, 'void EpubReaderActivity::activateBookmarkRow(')
        self.assertLess(activate.index('addBookmark();'), activate.index('applyProgressChange(jump);'))
        self.assertIn('startActivityForResultWith<EpubReaderBookmarksActivity>', activate)
        # The More panel no longer carries the two bookmark rows on RickyOS.
        more = method(reader, 'void EpubReaderActivity::buildMoreActions()')
        self.assertIn('item.action == MA::BOOKMARKS || item.action == MA::TOGGLE_BOOKMARK', more)
        # The list-menu result and the panel share one jump.
        self.assertIn('applyProgressChange(std::get<ProgressChangeResult>(result.data));',
                      method(reader, 'void EpubReaderActivity::onReaderMenuConfirm('))

    def test_weight_row_sits_under_the_size_and_redraws_without_repaginating(self):
        reader = READER.read_text()
        self.assertIn('constexpr int kTextRowWeight = 2;', reader)
        names = reader[reader.index('constexpr StrId kTextRowNames[] = {'):]
        names = names[:names.index('};')]
        self.assertLess(names.index('STR_FONT_SIZE'), names.index('STR_RICKY_WEIGHT'))
        self.assertLess(names.index('STR_RICKY_WEIGHT'), names.index('STR_LINE_SPACING'))
        popup = method(reader, 'void EpubReaderActivity::showTextRowPopup(')
        weight = popup[popup.index('case kTextRowWeight:'):]
        weight = weight[:weight.index('break;')]
        self.assertIn('SETTINGS.rickyTextWeight = static_cast<uint8_t>(idx);', weight)
        self.assertNotIn('applyTextSettingLive', weight)
        # fakeBold only fakes bold-styled runs; the weight scope reaches every glyph of the page,
        # and the page cache keys on it so a change redraws.
        self.assertEqual(reader.count('GfxRenderer::TextWeightScope textWeight(renderer, readerTextWeightCurve('), 3)
        self.assertEqual(reader.count('readerTextWeightSpread(key.textWeight)'), 2)
        self.assertEqual(reader.count('readerTextWeightSpread(readerTextWeight())'), 1)
        self.assertIn('.textWeight = readerTextWeight(),', reader)
        self.assertIn('uint8_t textWeight = 2;', (ROOT / 'src/activities/reader/ReaderPageCache.h').read_text())
        for key, text in (('STR_RICKY_WEIGHT', '字重'), ('STR_RICKY_WEIGHT_THINNEST', '最细'),
                          ('STR_RICKY_BOOKMARK_ADD', '添加书签'), ('STR_RICKY_BOOKMARK_MANAGE', '管理书签')):
            self.assertIn(f'{key}: "{text}"', (ROOT / 'lib/I18n/translations/chinese.yaml').read_text())

    def test_weight_curves_keep_paper_and_ink_and_order_the_steps(self):
        reader = READER.read_text()
        curves = method(reader, 'const uint8_t* readerTextWeightCurve(') + '\n' + method(
            reader, 'uint8_t readerTextWeightSpread(')
        renderer = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        weighted = method(renderer, 'static uint8_t weighted2BitCoverage(')
        program = r'''
#include <cassert>
#include <cstdint>
#include <iterator>
static uint8_t get4BitCoverage(const uint8_t* bitmap, const int p) { return (bitmap[p >> 1] >> ((1 - (p & 1)) * 4)) & 0xF; }
static uint8_t get2BitCoverage(const uint8_t* bitmap, const int p, const bool fourBit = false) {
  if (fourBit) return get4BitCoverage(bitmap, p) >> 2;
  return (bitmap[p >> 2] >> ((3 - (p & 3)) * 2)) & 0x3;
}
''' + weighted + '\n' + curves + r'''
int main() {
  assert(readerTextWeightCurve(2) == nullptr);  // the font as drawn costs nothing
  assert(readerTextWeightCurve(5) == nullptr);
  for (int w = 0; w < 5; ++w) {
    const uint8_t* c = w == 2 ? nullptr : readerTextWeightCurve(w);
    if (!c) continue;
    assert(c[0] == 0 && c[15] == 15);
    for (int i = 1; i < 16; ++i) assert(c[i] >= c[i - 1]);
  }
  for (int i = 1; i < 15; ++i) {
    assert(readerTextWeightCurve(0)[i] <= readerTextWeightCurve(1)[i] && readerTextWeightCurve(1)[i] <= i);
    assert(readerTextWeightCurve(3)[i] >= i && readerTextWeightCurve(4)[i] >= i);
  }
  // Only the heaviest step spreads strokes; the others shape edges alone.
  for (int w = 0; w < 4; ++w) assert(readerTextWeightSpread(w) == 0);
  assert(readerTextWeightSpread(4) == 2);
  // A 4-bit edge pixel of coverage 6 reads as 2-bit 1 as drawn, 2 when heavier, 0 when thinnest.
  const uint8_t edge[] = {0x60};
  assert(weighted2BitCoverage(nullptr, edge, 0, true) == 1);
  assert(weighted2BitCoverage(readerTextWeightCurve(3), edge, 0, true) == 2);
  assert(weighted2BitCoverage(readerTextWeightCurve(0), edge, 0, true) == 0);
  const uint8_t two[] = {0x40};  // 2-bit pixel 1 (gray)
  assert(weighted2BitCoverage(readerTextWeightCurve(3), two, 0, false) == 2);
}
'''
        run_cpp(program)

    def test_cjk_bookmark_summary_drops_word_spaces_and_cuts_on_a_character(self):
        source = (ROOT / 'src/util/BookmarkUtil.cpp').read_text()
        sanitize = method(source, 'std::string BookmarkUtil::sanitizeBookmarkSummary(')
        program = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>
struct BookmarkUtil { static std::string sanitizeBookmarkSummary(std::string summary); };
''' + sanitize + r'''
int main() {
  assert(BookmarkUtil::sanitizeBookmarkSummary("第 一 章 · 经 济 篇") == "第一章·经济篇");
  assert(BookmarkUtil::sanitizeBookmarkSummary("Walden by Thoreau") == "Walden by Thoreau");
  assert(BookmarkUtil::sanitizeBookmarkSummary("这 是 RickyOS 界 面") == "这是 RickyOS 界面");
  std::string longText;
  for (int i = 0; i < 40; ++i) longText += "字 ";
  const std::string cut = BookmarkUtil::sanitizeBookmarkSummary(longText);
  assert(cut.size() <= 72 && cut.size() % 3 == 0);  // whole three-byte characters only
  assert(BookmarkUtil::sanitizeBookmarkSummary(cut) == cut);
}
'''
        run_cpp(program)


if __name__ == '__main__':
    unittest.main()

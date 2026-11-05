#pragma once

#include "ReadingMode.h"

class GfxRenderer;
class Page;

// Draws what a reading mode puts around the text: the puppy frame and dog, or the
// ancient-book rules and 版心. Called with the status bar in every render pass
// (B/W base, 16-level page, gray planes), so it draws with the plain B/W API and
// only adds gray where the 16-level frame is open. No heap; Flash art only.
namespace readerChrome {

// Distances relative to the oriented safe area. Layout and drawing must use
// this same geometry: a nominal 96 px footer wasted space, while shrinking
// only the reservation would let the last body line collide with its labels.
// Pass the tallest caption/fallback/numeric font so real CJK ink fits too.
struct CompactLayout {
  int rowHeight;
  int headerTextY;
  int headerReserve;
  int footerTextBottom;
  int progressBottom;
  int footerReserve;
};

// textHeight is the chrome's own small type (caption, status numerals), not body text.
// The footer is one row close to the panel edge: page count left, percent right and
// the progress bar between them, centred on the row. Without the text lane the bar
// alone sits at the bottom. Every pixel the chrome does not take goes to the text.
constexpr CompactLayout compactLayout(const int textHeight, const bool headerLane, const bool footerLane,
                                      const bool progress, const int progressHeight) {
  constexpr int headerPadding = 4;
  constexpr int bottomPadding = 4;
  constexpr int contentGap = 8;
  const int row = textHeight < 28 ? 28 : textHeight;  // 24 px icons + their 4 px inset
  const int textBottom = bottomPadding + row;
  const int barHeight = progressHeight < 1 ? 1 : progressHeight;
  const int barBottom = footerLane ? bottomPadding + (row - barHeight) / 2 : bottomPadding;
  const int occupied = footerLane ? textBottom : progress ? barBottom + barHeight : 0;
  return {row, headerPadding, headerLane ? headerPadding * 2 + row + contentGap : 0,
          textBottom, barBottom, occupied ? occupied + contentGap : 0};
}

struct PageInfo {
  const char* bookTitle = nullptr;
  // 古籍 版心 label: the chapter when the table of contents names one, else the book.
  const char* chapterTitle = nullptr;
  int pageInChapter = 1;  // 1-based
};

void draw(const GfxRenderer& renderer, readingMode::Mode mode, const readingMode::Box& text, const PageInfo& page);

// 古籍: 界行 between the columns of a vertically set page whose columns are `pitch`
// wide (the line height), right to left from the text box's right edge. Drawn with
// the page text, so the page cache keeps them; empty columns continue the grid.
void drawColumnRules(const GfxRenderer& renderer, const Page& page, const readingMode::Box& text, int pitch);

}  // namespace readerChrome

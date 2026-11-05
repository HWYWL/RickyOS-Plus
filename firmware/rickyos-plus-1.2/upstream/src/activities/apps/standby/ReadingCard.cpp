#include "ReadingCard.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "SdCardFontSystem.h"
#include "components/RickyDesignUi.h"
#include "fontIds.h"
#include "util/ReadingExcerpt.h"
#if FREEINK_DEVICE_METALIO_PLUS
#include <PlusProgressiveJpeg.h>
#endif

namespace {
#if FREEINK_DEVICE_METALIO_PLUS
constexpr char kCardCoverJpeg[] = "/.crosspoint/standby-card-cover.jpg";
#endif
constexpr int kTimeFont = NOTOSANS_18_FONT_ID;

struct Canvas {
  GfxRenderer& renderer;
  bool gray;
  int vector[4] = {};  // display faces: their 4-bit glyphs go straight into the gray frame
  bool direct(int font) const {
    return gray && font != 0 && std::find(std::begin(vector), std::end(vector), font) != std::end(vector);
  }
  // B/W text drawn for a 16-gray frame is copied in rectangle by rectangle, so the
  // cover underneath is never overwritten by the B/W buffer's white.
  void copy(int x, int y, int width, int height) const {
    if (gray && width > 0 && height > 0) renderer.copyBwToGrayscale16(x, y, width, height, 0);
  }
  int line(int font) const { return renderer.getLineHeight(font); }
  int width(int font, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return renderer.getTextWidth(font, text, style);
  }
  void text(int font, int x, int y, const char* value, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    renderer.drawText(font, x, y, value, true, style);
    if (!direct(font)) copy(x, y, width(font, value, style), line(font));
  }
  void rule(int x, int y, int width, int thickness = 2) const {
    renderer.fillRect(x, y, width, thickness, true);
    copy(x, y, width, thickness);
  }
};

// Wrap `text` to `width` (CJK per character, Latin per word), at most `maxLines` lines;
// a cut last line ends with an ellipsis.
std::vector<std::string> wrap(const Canvas& c, int font, const std::string& text, int width, int maxLines) {
  std::vector<std::string> tokens;
  const auto* cursor = reinterpret_cast<const uint8_t*>(text.c_str());
  std::string word;
  while (*cursor) {
    const auto* start = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    const std::string glyph(reinterpret_cast<const char*>(start), cursor - start);
    const bool latin = cp < 0x80 && cp != ' ';
    if (latin) {
      word += glyph;
      continue;
    }
    if (!word.empty()) tokens.push_back(std::move(word)), word.clear();
    tokens.push_back(glyph);
  }
  if (!word.empty()) tokens.push_back(word);
  std::vector<std::string> lines;
  std::string current;
  for (size_t i = 0; i < tokens.size(); ++i) {
    std::string next = current + tokens[i];
    if (!current.empty() && c.width(font, next.c_str()) > width) {
      lines.push_back(current);
      if (static_cast<int>(lines.size()) == maxLines) {
        std::string& last = lines.back();
        while (!last.empty() && c.width(font, (last + "……").c_str()) > width) {
          const auto chars = reinterpret_cast<const uint8_t*>(last.c_str());
          size_t cut = last.size() - 1;
          while (cut > 0 && (chars[cut] & 0xC0) == 0x80) --cut;
          last.erase(cut);
        }
        last += "……";
        return lines;
      }
      current = tokens[i] == " " ? std::string() : tokens[i];
    } else {
      current = std::move(next);
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

// Draws the cover bitmap scaled into `box`, fitted (or filling it, cropped when `fill`).
bool drawCover(const Canvas& c, const std::string& path, int x, int y, int width, int height, bool fill) {
  if (path.empty()) return false;
  HalFile file;
  if (!Storage.openFileForRead("STANDBY", path, file)) return false;
  Bitmap bitmap(file, !c.gray);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  const float bw = static_cast<float>(bitmap.getWidth()), bh = static_cast<float>(bitmap.getHeight());
  float cropX = 0, cropY = 0;
  if (fill) {
    const float box = static_cast<float>(width) / static_cast<float>(height);
    if (bw / bh > box)
      cropX = 1.0f - box * bh / bw;
    else
      cropY = 1.0f - bw / (box * bh);
    cropX = std::clamp(cropX, 0.0f, 0.95f);
    cropY = std::clamp(cropY, 0.0f, 0.95f);
  }
  // Center what is drawn: the renderer scales down to fit, never up.
  const float scale = std::min({1.0f, width / ((1 - cropX) * bw), height / ((1 - cropY) * bh)});
  const int drawnWidth = static_cast<int>((1 - cropX) * bw * scale);
  const int drawnHeight = static_cast<int>((1 - cropY) * bh * scale);
  const int at = x + (width - drawnWidth) / 2, top = y + (height - drawnHeight) / 2;
  if (c.gray) return c.renderer.drawBitmapGrayscale16(bitmap, at, top, width, height, cropX, cropY);
  return c.renderer.drawBitmap(bitmap, at, top, width, height, cropX, cropY);
}

// Point size whose em is `px` pixels (vector faces speak points at 150 DPI).
uint8_t pointsFor(const int px) { return static_cast<uint8_t>(std::clamp(px * 72 / 150, 10, 48)); }

// A book without a cover gets a typeset one in its place: a tinted panel with a framed
// title and the author, so every layout keeps its shape. The tint needs text that goes
// straight into the gray frame (a display face); copied B/W text would paint white boxes.
void drawTypesetCover(const Canvas& c, int x, int y, int width, int height, const std::string& title,
                      const std::string& author, int titleFont, int metaFont) {
  const bool tint = c.gray && c.direct(titleFont);
  if (tint) {
    c.renderer.fillGrayscale16Rect(x, y, width, height, RickyDesignUi::ds::gray::kTint);
  } else {
    c.renderer.drawRect(x, y, width, height, 2, true);
    c.copy(x, y, width, height);
  }
  const int inset = std::max(6, width / 22);
  if (tint) {
    // Hairline inner frame, drawn straight into the gray frame.
    const int fx = x + inset, fy = y + inset, fw = width - 2 * inset, fh = height - 2 * inset;
    c.renderer.fillGrayscale16Rect(fx, fy, fw, 1, RickyDesignUi::ds::gray::kInk);
    c.renderer.fillGrayscale16Rect(fx, fy + fh - 1, fw, 1, RickyDesignUi::ds::gray::kInk);
    c.renderer.fillGrayscale16Rect(fx, fy, 1, fh, RickyDesignUi::ds::gray::kInk);
    c.renderer.fillGrayscale16Rect(fx + fw - 1, fy, 1, fh, RickyDesignUi::ds::gray::kInk);
  }
  const int textWidth = width - 4 * inset;
  // Small boxes (the editorial thumbnail) set the title in the caption size.
  const int font = c.width(titleFont, "国") * 5 > textWidth ? metaFont : titleFont;
  const auto lines = wrap(c, font, title, textWidth, 4);
  const int step = c.line(font) * 11 / 10;
  int ty = y + height * 2 / 5 - static_cast<int>(lines.size()) * step / 2;
  for (const auto& line : lines) {
    const int w = c.width(font, line.c_str());
    c.text(font, x + (width - w) / 2, ty, line.c_str());
    ty += step;
  }
  if (author.empty()) return;
  ty += step / 2;
  const int ruleWidth = std::min(width / 6, 48);
  if (tint)
    c.renderer.fillGrayscale16Rect(x + (width - ruleWidth) / 2, ty, ruleWidth, 2, RickyDesignUi::ds::gray::kInk);
  else
    c.rule(x + (width - ruleWidth) / 2, ty, ruleWidth);
  ty += step / 2;
  for (const auto& line : wrap(c, metaFont, author, textWidth, 1)) {
    const int w = c.width(metaFont, line.c_str());
    c.text(metaFont, x + (width - w) / 2, ty, line.c_str());
  }
}

// "10:24", "10 月 28 日 · 周二", or "" (the time/date choice is the standby info setting).
void clockStrings(char (&time)[8], char (&date)[48]) {
  time[0] = date[0] = '\0';
  if (SETTINGS.standbyOverlay == CrossPointSettings::STANDBY_OVERLAY_NONE) return;
  std::tm local{};
  if (!halClock.localTime(local)) return;
  static constexpr StrId weekdays[] = {
      StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
      StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
  snprintf(date, sizeof(date), tr(STR_RICKY_HOME_DATE), static_cast<unsigned>(local.tm_mon + 1),
           static_cast<unsigned>(local.tm_mday), I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]));
  if (SETTINGS.standbyOverlay == CrossPointSettings::STANDBY_OVERLAY_TIME)
    snprintf(time, sizeof(time), "%02d:%02d", local.tm_hour, local.tm_min);
}
}  // namespace

bool ReadingCard::load() {
  const std::string& book = APP_STATE.openEpubPath;
  if (book.empty()) return false;
  coverPath_.clear();
  if (FsHelpers::hasReflowableBookExtension(book)) {
    Epub epub(book, "/.crosspoint");
    if (epub.load(true, true)) {
      title_ = epub.getTitle();
      author_ = epub.getAuthor();
      if (epub.generateCoverBmp(false)) coverPath_ = epub.getCoverBmpPath(false);
#if FREEINK_DEVICE_METALIO_PLUS
      // A cover the reader chose (override) keeps the BMP path; the book's own JPEG cover is
      // extracted for the full-resolution decode.
      sourceJpeg_.clear();
      if (!epub.hasCoverOverride() && FsHelpers::hasJpgExtension(epub.getCoverItemHref()) &&
          epub.extractItemToFile(epub.getCoverItemHref(), kCardCoverJpeg))
        sourceJpeg_ = kCardCoverJpeg;
#endif
    }
  }
  if (const auto* stats = READING_STATS.findBook(book)) {
    if (title_.empty()) title_ = stats->title;
    if (author_.empty()) author_ = stats->author;
    chapter_ = stats->chapterTitle;
    progress_ = std::min<int>(stats->lastProgressPercent, 100);
  }
  if (title_.empty()) title_ = FsHelpers::getFileNameWithoutExtension(book);
  todayMinutes_ = static_cast<unsigned>(READING_STATS.getTodayReadingMs() / 60000);
  excerpt_ = SETTINGS.standbyShowExcerpt ? reading_excerpt::load(book) : std::string();
  return true;
}

void ReadingCard::release() {
  if (fontsOwner_) sdFontSystem.releaseDisplayFaces(*fontsOwner_);
  fontsOwner_ = nullptr;
#if FREEINK_DEVICE_METALIO_PLUS
  sharp_.reset();
  sharpBoxWidth_ = sharpBoxHeight_ = 0;
  sharpFailed_ = false;
#endif
}

bool ReadingCard::drawSharpCover(GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                                 const bool fill) const {
#if FREEINK_DEVICE_METALIO_PLUS
  if (sourceJpeg_.empty() || !renderer.isGrayscale16Active()) return false;
  if (!sharp_ || sharpBoxWidth_ != width || sharpBoxHeight_ != height || sharpFill_ != fill) {
    if (sharpFailed_) return false;
    sharp_ = decodePlusJpegGrayForBox(sourceJpeg_, width, height, fill, sharpWidth_, sharpHeight_);
    sharpBoxWidth_ = width;
    sharpBoxHeight_ = height;
    sharpFill_ = fill;
    sharpFailed_ = !sharp_;  // too large for PSRAM: the BMP for the rest of this face
    if (!sharp_) return false;
  }
  const int at = x + (width - sharpWidth_) / 2, top = y + (height - sharpHeight_) / 2;
  for (int row = 0; row < sharpHeight_; ++row) {
    const uint8_t* src = sharp_.get() + static_cast<size_t>(row) * sharpWidth_;
    for (int col = 0; col < sharpWidth_; ++col) renderer.drawGrayscale16Pixel(at + col, top + row, src[col]);
  }
  return true;
#else
  (void)renderer;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  (void)fill;
  return false;
#endif
}

bool ReadingCard::drawGray(GfxRenderer& renderer, const Rect& viewport) const { return draw(renderer, viewport, true); }

void ReadingCard::drawBw(GfxRenderer& renderer, const Rect& viewport) const { draw(renderer, viewport, false); }

bool ReadingCard::draw(GfxRenderer& renderer, const Rect& viewport, const bool gray) const {
  if (viewport.width <= 0 || viewport.height <= 0) return false;
  // Preserve the Plus proportions while fitting the ordinary board's smaller viewport.
  const int shortSide = std::min({viewport.width, viewport.height, 684});
  const auto spacing = [shortSide](int pixels) { return std::max(1, pixels * shortSide / 684); };
  const int margin = spacing(48);
  Canvas c{renderer, gray};
  renderer.clearScreen();  // the B/W layer starts white; text is copied out of it
  const int x0 = viewport.x + margin, width = viewport.width - 2 * margin;
  const bool split = SETTINGS.standbyCoverLayout == CrossPointSettings::STANDBY_COVER_SPLIT;
  // Sizes follow the text column: about 14 characters a line for the quote.
  const int quoteFace = sdFontSystem.acquireDisplayFace(renderer, pointsFor(width / (split ? 15 : 13)));
  const int titleFace = sdFontSystem.acquireDisplayFace(renderer, pointsFor(width / 17));
  const int metaFace = sdFontSystem.acquireDisplayFace(renderer, pointsFor(width / 27));
  // The opening quote mark is one glyph, set large as the editorial page's ornament.
  const int markFace = split || excerpt_.empty() ? 0 : sdFontSystem.acquireDisplayFace(renderer, pointsFor(width / 4));
  if (quoteFace || titleFace || metaFace || markFace) fontsOwner_ = &renderer;
  c.vector[0] = quoteFace;
  c.vector[1] = titleFace;
  c.vector[2] = metaFace;
  c.vector[3] = markFace;
  const int top = viewport.y, bottom = viewport.y + viewport.height;
  char time[8], date[48];
  clockStrings(time, date);
  char read[24];
  snprintf(read, sizeof(read), tr(STR_RICKY_READ_PERCENT), static_cast<unsigned>(progress_));
  char today[48];
  snprintf(today, sizeof(today), tr(STR_RICKY_HOME_TODAY), todayMinutes_);
  // Without a vector reader font, the UI fonts.
  const int caption = metaFace ? metaFace : RickyDesignUi::CAPTION;
  const int body = metaFace ? metaFace : RickyDesignUi::BODY;
  const int quoteFont =
      quoteFace ? quoteFace : RickyDesignUi::fontFor(renderer, RickyDesignUi::HEADLINE, excerpt_.c_str());
  const int titleFont =
      titleFace ? titleFace : RickyDesignUi::fontFor(renderer, RickyDesignUi::HEADLINE, title_.c_str());
  // A vector face's line height already carries its own leading.
  const int quoteStep = quoteFace ? c.line(quoteFont) * 6 / 5 : c.line(quoteFont) * 3 / 2;

  if (split) {
    // Cover over text: the top 42% is the cover filling the width, the rest white.
    const int band = viewport.height * 42 / 100;
    if (!drawSharpCover(renderer, viewport.x, top, viewport.width, band, true) &&
        !drawCover(c, coverPath_, viewport.x, top, viewport.width, band, true))
      drawTypesetCover(c, viewport.x, top, viewport.width, band, title_, author_, titleFont, caption);
    const int footer = bottom - margin - c.line(caption);
    int y = top + band + spacing(40);
    if (!excerpt_.empty()) {
      c.text(caption, x0, y, tr(STR_RICKY_LAST_READ));
      y += c.line(caption) + 10;
      c.rule(x0, y, 32);
      y += spacing(24);
      const int metadataHeight = c.line(caption) * (chapter_.empty() ? 1 : 2) + spacing(48);
      const int excerptLines = std::clamp((footer - y - metadataHeight) / quoteStep, 1, 6);
      for (const auto& line : wrap(c, quoteFont, excerpt_, width, excerptLines)) {
        c.text(quoteFont, x0, y, line.c_str());
        y += quoteStep;
      }
      y += 16;
    } else {
      c.text(titleFont, x0, y, title_.c_str(), EpdFontFamily::BOLD);
      y += c.line(titleFont) + 8;
      if (!author_.empty()) c.text(body, x0, y, author_.c_str()), y += c.line(body) + 16;
    }
    if (!chapter_.empty()) c.text(caption, x0, y, chapter_.c_str()), y += c.line(caption) + 6;
    c.text(caption, x0, y, today);
    // Footer: time · date · progress on one hairline-topped row.
    c.rule(x0, footer - 18, width, 1);
    std::string row;
    if (time[0]) row += std::string(time) + " · ";
    if (date[0]) row += std::string(date) + " · ";
    row += read;
    c.text(caption, x0, footer, row.c_str());
    return true;
  }

  // Editorial: the excerpt is the hero; cover thumbnail, title and progress at the foot.
  int y = top + margin;
  if (!excerpt_.empty()) c.text(caption, x0, y, tr(STR_RICKY_LAST_READ));
  if (time[0] || date[0]) {
    int ry = y;
    if (time[0]) {
      const int w = c.width(kTimeFont, time, EpdFontFamily::BOLD);
      c.text(kTimeFont, x0 + width - w, ry, time, EpdFontFamily::BOLD);
      ry += c.line(kTimeFont);
    }
    const int w = c.width(caption, date);
    c.text(caption, x0 + width - w, ry, date);
  }
  const int thumbWidth = spacing(132), thumbHeight = spacing(190);
  const int footRule = bottom - margin - c.line(caption) - 14;
  const int thumbTop = footRule - spacing(28) - thumbHeight;
  if (!excerpt_.empty()) {
    y = top + viewport.height / 6;
    const int mark = markFace ? markFace : RickyDesignUi::fontFor(renderer, RickyDesignUi::TITLE, "“");
    c.text(mark, x0 - (markFace ? 8 : 0), y, "“", markFace ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD);
    // The mark's glyph sits high in its line; the quote tucks up under it.
    y += markFace ? c.line(mark) * 3 / 5 : c.line(mark);
    const int room = (thumbTop - 40 - y) / quoteStep;
    for (const auto& line : wrap(c, quoteFont, excerpt_, width, std::max(2, room))) {
      c.text(quoteFont, x0, y, line.c_str());
      y += quoteStep;
    }
    if (!drawSharpCover(renderer, x0, thumbTop, thumbWidth, thumbHeight, false) &&
        !drawCover(c, coverPath_, x0, thumbTop, thumbWidth, thumbHeight, false))
      drawTypesetCover(c, x0, thumbTop, thumbWidth, thumbHeight, title_, std::string(), titleFont, caption);
    int ty = thumbTop + 8;
    const int tx = x0 + thumbWidth + 24;
    const int tw = width - thumbWidth - 24;
    for (const auto& line : wrap(c, titleFont, title_, tw, 2)) {
      c.text(titleFont, tx, ty, line.c_str(), EpdFontFamily::BOLD);
      ty += c.line(titleFont) + 4;
    }
    if (!author_.empty()) c.text(body, tx, ty, author_.c_str()), ty += c.line(body) + 4;
    if (!chapter_.empty())
      for (const auto& line : wrap(c, caption, chapter_, tw, 1)) c.text(caption, tx, ty, line.c_str());
  } else {
    // No excerpt (hidden, or none yet): the cover large, title under it.
    const int coverTop = top + viewport.height / 7;
    const int coverHeight = footRule - 40 - coverTop - c.line(titleFont) * 2 - 24;
    if (!drawSharpCover(renderer, x0, coverTop, width, coverHeight, false) &&
        !drawCover(c, coverPath_, x0, coverTop, width, coverHeight, false)) {
      // Typeset at a book's proportions, centered where the cover would be.
      const int w = std::min(width * 3 / 4, coverHeight * 2 / 3);
      const int h = std::min(coverHeight, w * 3 / 2);
      drawTypesetCover(c, x0 + (width - w) / 2, coverTop + (coverHeight - h) / 2, w, h, title_, author_, titleFont,
                       caption);
    }
    int ty = coverTop + coverHeight + 24;
    const int tw = c.width(titleFont, title_.c_str(), EpdFontFamily::BOLD);
    c.text(titleFont, x0 + (width - std::min(tw, width)) / 2, ty, title_.c_str(), EpdFontFamily::BOLD);
    ty += c.line(titleFont) + 4;
    if (!author_.empty()) {
      const int aw = c.width(body, author_.c_str());
      c.text(body, x0 + (width - aw) / 2, ty, author_.c_str());
    }
  }
  // Progress: a hairline with a tick at the reader's place, "已读 63%" and today's time.
  c.rule(x0, footRule, width, 2);
  const int tick = x0 + width * progress_ / 100;
  c.rule(std::min(tick, x0 + width - 4), footRule - 8, 4, 18);
  const int labels = footRule + 14;
  c.text(caption, x0, labels, read);
  c.text(caption, x0 + width - c.width(caption, today), labels, today);
  return true;
}

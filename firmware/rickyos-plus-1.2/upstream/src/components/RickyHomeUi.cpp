#include "RickyHomeUi.h"

#include <FsHelpers.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "RickyBrandMark.h"
#include "RickyDesignTokens.h"
#include "RickyDesignUi.h"
#include "RickyHomeLayout.h"
#include "RickyMascot.h"
#include "RickyPageUi.h"
#include "RickyPaw.h"
#include "RickyProfile.h"
#include "RickyReadingStreak.h"
#include "UITheme.h"
#include "util/TimeUtils.h"
#include "util/PanelTileCache.h"
#include "RickyCompanion.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId OPEN_BOOK = 1;
constexpr StrId greetingKeys[] = {StrId::STR_RICKY_HOME_GREETING, StrId::STR_RICKY_HOME_GREETING_REST,
                                  StrId::STR_RICKY_HOME_GREETING_SLOW, StrId::STR_RICKY_HOME_GREETING_TIME,
                                  StrId::STR_RICKY_HOME_GREETING_STORY};
static_assert(sizeof(greetingKeys) / sizeof(greetingKeys[0]) == RickyHomeLayout::GREETING_COUNT);
const char* titleOf(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }
const char* formatOf(const std::string& path) {
  return FsHelpers::hasEpubExtension(path)       ? "EPUB"
         : FsHelpers::hasTxtExtension(path)      ? "TXT"
         : FsHelpers::hasMarkdownExtension(path) ? "MD"
                                                 : nullptr;
}
}  // namespace

void RickyHomeUi::begin(const std::vector<RecentBook>& recent, CoverPainter coverPainter, void* activity) {
  books = &recent;
  painter = coverPainter;
  owner = activity;
  // Non-security variety, chosen once per visit. Render/cache refreshes must
  // never change the greeting or cause a timer-driven e-ink refresh.
  static uint8_t previousGreeting = RickyHomeLayout::GREETING_COUNT;
  static uint32_t visit = 0;
  greetingChoice = RickyHomeLayout::chooseGreeting(static_cast<uint32_t>(halClock.nowUtc()) ^ (++visit * 0x9e3779b9U),
                                                   previousGreeting);
  previousGreeting = greetingChoice;
  resetUi();
  app.on(OPEN_BOOK, &RickyHomeUi::onAction, this);
  app.setScreen(&RickyHomeUi::screenFn, this);
}

void RickyHomeUi::configure(Rect bounds, int selection, bool show, int percent) {
  content = bounds;
  selected = selection;
  start = RickyHomeLayout::pageStart(selection);
  showSelection = show;
  progress = std::clamp(percent, 0, 100);
  snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void RickyHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<RickyHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int RickyHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

bool RickyHomeUi::paint(fui::Rect rect, int index) {
  if (painter && painter(owner, index, Rect{rect.x, rect.y, rect.width, rect.height})) {
    // Real artwork gets a light gray edge so pale covers do not dissolve into the
    // page; a black frame overpowered the gray artwork.
    uiTarget.stroke(rect, fui::Paint::solid(fui::Color::LightGray), ricky::ds::stroke::kCoverEdge);
    return true;
  }
  if (!books || index < 0 || index >= static_cast<int>(books->size())) return false;
  const auto& book = (*books)[index];
  auto text = grid.titleText;
  text.font = fui::GfxRendererTarget::FONT_SMALL;  // Full common-character coverage for any title.
  if (rect.width > rect.height * 2) {
    RickyPageUi::bookTile(uiTarget, rect, titleOf(book), text);
    return true;
  }
  RickyDesignUi::cover(renderer, rect, titleOf(book), formatOf(book.path));
  return true;
}

void RickyHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<RickyHomeUi*>(user)->draw(screen); }

void RickyHomeUi::draw(UiScreen& screen) {
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  if (content.height > content.width) {
    drawPortrait(screen);
    return;
  }
  const auto& theme = screen.theme();
  const int gap = std::max(UiHighDpiProfile::controlGap, static_cast<int>(theme.spaceSm));
  const int16_t padding = gap;
  auto heading = theme.bodyText;
  heading.bold = true;
  const int headingHeight = screen.target().lineHeight(heading.font);
  const int labelHeight = screen.target().lineHeight(theme.smallText.font);
  const bool quietPortrait = content.height > content.width;
  const int profileHeight = headingHeight + labelHeight + gap;
  const auto profileRect = screen.takeTop(profileHeight + (quietPortrait ? labelHeight + gap * 2 : 0), gap * 2);
  std::tm local{};
  if (halClock.localTime(local)) {
    snprintf(dateText, sizeof(dateText), "%04d.%02d.%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
  } else {
    snprintf(dateText, sizeof(dateText), "%s", tr(STR_RICKY_DATE_UNSET));
  }
  auto dateStyle = theme.smallText;
  const int avatarSize = std::min<int>(profileHeight, profileRect.width / 4);
  RickyProfile::drawAvatar(renderer, Rect{profileRect.right() - avatarSize, profileRect.y, avatarSize, avatarSize});
  auto textRect = profileRect;
  textRect.width -= avatarSize + gap * 2;
  textRect.height = labelHeight;
  screen.target().text(textRect, dateText, dateStyle);
  textRect.y += labelHeight + gap;
  textRect.height = std::max(0, profileRect.bottom() - textRect.y);
  snprintf(greetingText, sizeof(greetingText), I18N.get(greetingKeys[greetingChoice]), RickyProfile::nickname());
  auto greeting = heading;
  greeting.maxLines = quietPortrait ? 2 : 1;
  screen.target().text(textRect, greetingText, greeting);
  screen.frame().hit(profileRect, OPEN_BOOK, STATISTICS, fui::InputTouch);
  if (quietPortrait) {
    auto phrase = theme.smallText;
    phrase.align = fui::TextAlign::Center;
    phrase.maxLines = 2;
    screen.target().text(screen.takeBottom(labelHeight * 2, gap * 2), RickyProfile::homePhrase(), phrase);
  }
  const auto statsRect = screen.takeBottom(labelHeight + gap * 2, gap * 2);
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_STATS),
           static_cast<unsigned>(READING_STATS.getTodayReadingMs() / 60000),
           static_cast<unsigned>(rickyStreak::evaluate().current));
  auto statsStyle = theme.smallText;
  statsStyle.align = fui::TextAlign::Center;
  screen.target().text(statsRect, statsText, statsStyle);
  screen.frame().hit(statsRect, OPEN_BOOK, STATISTICS, fui::InputTouch);
  if (!books || books->empty()) {
    auto message = theme.bodyText;
    message.align = fui::TextAlign::Center;
    message.maxLines = 2;
    auto rect = screen.body();
    rect.y += std::max(0, (rect.height - headingHeight * 2) / 2);
    rect.height = headingHeight * 2;
    screen.target().text(rect, tr(STR_NO_RECENT_BOOKS), message);
    screen.frame().hit(rect, OPEN_BOOK, LIBRARY, fui::InputTouch);
    return;
  }
  const auto body = screen.body();
  const auto layout = RickyHomeLayout::fit(body.width, body.height, headingHeight, labelHeight, gap, padding);
  const auto textFit = RickyHomeLayout::fitText(layout.coverHeight, headingHeight, labelHeight, gap, layout.rows == 1);
  thumbnailHeight = layout.coverHeight;
  screen.target().text(screen.takeTop(headingHeight, gap),
                       start == 0 ? tr(STR_CONTINUE_READING) : tr(STR_MENU_RECENT_BOOKS), heading);
  const auto& book = (*books)[start];
  card = {};
  card.title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  card.author = book.author.empty() || !textFit.author ? nullptr : book.author.c_str();
  card.titleText = theme.bodyText;
  card.titleText.maxLines = textFit.titleLines;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progress = progress;
  card.progressMax = textFit.progress ? 100 : 0;
  card.progressLabel = textFit.progress ? progressText : nullptr;
  card.coverSize =
      fui::Size{static_cast<int16_t>(layout.coverHeight * 2 / 3), static_cast<int16_t>(layout.coverHeight)};
  card.padding = fui::Insets{padding, padding, padding, padding};
  card.gap = gap * 2;
  card.textGap = gap;
  card.centerTextOnCover = true;
  card.progressHeight = 6;
  card.action = OPEN_BOOK;
  card.value = start;
  card.state = showSelection && selected == start ? fui::StateSelected : fui::StateNormal;
  card.styles = theme.listRow;
  card.selectionIndicator = fui::BookCardSelectionIndicator::CoverFrame;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::BookCardProps&, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    return self.paint(rect, self.start);
  };
  fui::bookCard(screen.frame(), screen.takeTop(layout.rowHeight, gap), card);
  auto recentHeading = screen.takeTop(headingHeight, gap);
  screen.frame().hit(recentHeading, OPEN_BOOK, LIBRARY, fui::InputTouch);
  const int pages = (books->size() + RickyHomeLayout::PAGE_BOOKS - 1) / RickyHomeLayout::PAGE_BOOKS;
  if (pages > 1) {
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "%d / %d", start / RickyHomeLayout::PAGE_BOOKS + 1, pages);
    auto pageStyle = theme.smallText;
    pageStyle.align = fui::TextAlign::Right;
    auto pageRect = recentHeading;
    pageRect.width = std::min<int>(recentHeading.width / 3, headingHeight * 3);
    pageRect.x += recentHeading.width - pageRect.width;
    screen.target().text(pageRect, pageText, pageStyle);
    recentHeading.width -= pageRect.width + gap;
  }
  screen.target().text(recentHeading, tr(STR_MENU_RECENT_BOOKS), heading);
  grid = {};
  grid.count = std::min<int>(RickyHomeLayout::PAGE_BOOKS - 1, books->size() - start - 1);
  grid.columns = layout.columns;
  grid.rowHeight = layout.rowHeight;
  grid.gap = grid.rowGap = gap;
  grid.coverSize = card.coverSize;
  grid.cellInset = card.padding;
  grid.labelHeight = labelHeight;
  grid.labelGap = 0;
  grid.titleText = theme.smallText;
  grid.titleText.maxLines = 1;
  grid.cellStyles = theme.listRow;
  grid.minTouchSize = 0;  // Keep hit regions inside drawn cells, never expand into gaps.
  grid.action = OPEN_BOOK;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = showSelection && selected > start ? selected - start - 1 : -1;
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.scrollIndicator = false;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](uint16_t index, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    const int bookIndex = self.start + index + 1;
    const auto& item = (*self.books)[bookIndex];
    return fui::coverGridItem(item.title.empty() ? item.path.c_str() : item.title.c_str(), bookIndex);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::CoverGridItem&, uint16_t index, void* user) {
    auto& self = *static_cast<RickyHomeUi*>(user);
    return self.paint(rect, self.start + index + 1);
  };
  fui::coverGrid(screen.frame(), screen.body(), grid);
}

void RickyHomeUi::drawPortrait(UiScreen& screen) {
  namespace design = RickyDesignUi;
  namespace ds = ricky::ds;
  const int pad = ds::space::kCardPadding;
  const int gap = ds::space::kGutter;
  auto& target = screen.target();
  const auto box = [](int x, int y, int w, int h) {
    return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                     static_cast<int16_t>(h)};
  };
  std::tm local{};
  static constexpr StrId weekdays[] = {
      StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE, StrId::STR_CAL_WEEKDAY_WED,
      StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI, StrId::STR_CAL_WEEKDAY_SAT};
  if (halClock.localTime(local)) {
    snprintf(dateText, sizeof(dateText), tr(STR_RICKY_HOME_MONTH_DAY), static_cast<unsigned>(local.tm_mon + 1),
             static_cast<unsigned>(local.tm_mday));
    snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_WEEK_STREAK),
             I18N.get(weekdays[std::clamp(local.tm_wday, 0, 6)]),
             static_cast<unsigned>(rickyStreak::evaluate().current));
  } else {
    snprintf(dateText, sizeof(dateText), "%s", tr(STR_RICKY_DATE_UNSET));
    statsText[0] = 0;
  }
  snprintf(greetingText, sizeof(greetingText), I18N.get(greetingKeys[greetingChoice]), RickyProfile::nickname());
  auto greetingStyle = screen.theme().bodyText;
  greetingStyle.maxLines = 2;
  const int greetingWidth = std::max(1, screen.body().width - 250 - gap);
  const int greetingHeight =
      std::clamp<int>(fui::measureWrappedText(target, greetingText, greetingStyle, greetingWidth).height, 36, 72);
  const auto profile = screen.takeTop(88 + greetingHeight + 4, ds::space::k6);
  const auto avatar =
      box(profile.right() - ds::layout::kAvatarSize, profile.y, ds::layout::kAvatarSize, ds::layout::kAvatarSize);
  {
    // Same picture on the same paper every visit; a custom one is otherwise read from SD.
    // The theme picks the default avatar's picture, so it is part of the key.
    char key[sizeof(SETTINGS.rickyAvatarPath) + 52];
    snprintf(key, sizeof(key), "avatar|%s|%u|%d,%d,%d", SETTINGS.rickyAvatarPath,
             static_cast<unsigned>(SETTINGS.rickyCompanion), avatar.x, avatar.y, avatar.width);
    PanelTileCache::draw(renderer, key, "avatar", avatar.x, avatar.y, avatar.width, avatar.height, [&] {
      RickyProfile::drawAvatar(renderer, Rect{avatar.x, avatar.y, avatar.width, avatar.height});
      return true;
    });
  }
  screen.frame().hit(avatar, OPEN_BOOK, PROFILE, fui::InputTouch);
  const int textWidth = profile.width - avatar.width - gap;
  design::text(renderer, box(profile.x, profile.y, textWidth, 56), dateText, design::NUMERIC_DISPLAY);
  design::secondaryText(renderer, box(profile.x, profile.y + 56, textWidth, 28), statsText, design::CAPTION);
  design::secondaryText(renderer, box(profile.x, profile.y + 88, greetingWidth, greetingHeight), greetingText,
                        design::BODY, false, fui::TextAlign::Left, 2);
  const auto note = screen.takeBottom(28, ds::space::k4);
  design::secondaryText(renderer, note, RickyProfile::homePhrase(), design::CAPTION, false, fui::TextAlign::Center);

  const bool empty = !books || books->empty();
  const int cardH = empty ? 256 : std::min(288, screen.body().height - 184 - 24 - 40 - 12 - 200);
  const auto main = screen.takeTop(std::max(256, cardH), ds::space::k6);
  // The puppy's paws rest on the card's top edge, drawn over them next.
  const auto peek = RickyCompanion::cat() ? RickyBrandMark::catPeek() : RickyBrandMark::dogPeek();
  RickyBrandMark::drawDogPeek(renderer, Rect{main.right() - 124 - peek.width, main.y - peek.height + 1,
                                             peek.width, peek.height}, peek);
  const bool selectedCard = !empty && showSelection && selected == start;
  design::softCard(renderer, main, selectedCard);
  const int coverH = main.height - pad * 2;
  const int coverW = coverH * ds::layout::kCoverAspectW / ds::layout::kCoverAspectH;
  thumbnailHeight = coverH;
  const auto art = box(main.x + pad, main.y + pad, coverW, coverH);
  const int x = art.right() + pad;
  const int width = main.right() - x - pad;
  const auto button =
      box(x, main.bottom() - pad - ds::layout::kButtonCompactHeight, width, ds::layout::kButtonCompactHeight);
  if (empty) {
    target.stroke(art, fui::Paint::solid(fui::Color::Black), ds::stroke::kHairline);
    target.line({static_cast<int16_t>(art.x + art.width / 2 - 20), static_cast<int16_t>(art.y + art.height / 2)},
                {static_cast<int16_t>(art.x + art.width / 2 + 20), static_cast<int16_t>(art.y + art.height / 2)},
                ds::stroke::kHairline, fui::Paint::solid(fui::Color::Black));
    target.line({static_cast<int16_t>(art.x + art.width / 2), static_cast<int16_t>(art.y + art.height / 2 - 20)},
                {static_cast<int16_t>(art.x + art.width / 2), static_cast<int16_t>(art.y + art.height / 2 + 20)},
                ds::stroke::kHairline, fui::Paint::solid(fui::Color::Black));
    design::text(renderer, box(x, main.y + pad + 16, width, 44), tr(STR_RICKY_FIRST_BOOK), design::HEADLINE);
    design::fill(renderer, button, ds::gray::kInk, ds::radius::kControl);
    design::text(renderer, button, tr(STR_RICKY_START_TRANSFER), design::BODY, true, fui::TextAlign::Center);
    screen.frame().hit(main, OPEN_BOOK, TRANSFER, fui::InputTouch);
    const auto heading = screen.takeTop(40, 8);
    design::text(renderer, heading, tr(STR_RICKY_TRANSFER_METHODS), design::HEADLINE);
    // These entries are already supported; routing stays on the existing
    // transfer chooser rather than adding a second network implementation.
    const StrId methods[] = {StrId::STR_RICKY_BROWSER_TRANSFER, StrId::STR_RICKY_WEBDAV_CALIBRE,
                             StrId::STR_RICKY_CREATE_HOTSPOT, StrId::STR_RICKY_SD_BOOKS, StrId::STR_WEREAD_TITLE};
    for (auto id : methods) {
      auto row = screen.takeTop(ds::layout::kRowHeight, 0);
      if (row.empty()) break;
      design::text(renderer, row, I18N.get(id));
      RickyPageUi::chevron(target, box(row.right() - 24, row.y, 24, row.height));
      screen.frame().hit(row, OPEN_BOOK,
                         id == StrId::STR_WEREAD_TITLE     ? WEREAD
                         : id == StrId::STR_RICKY_SD_BOOKS ? FILES
                                                           : TRANSFER,
                         fui::InputTouch);
    }
    return;
  }
  const auto& book = (*books)[start];
  paint(art, start);
  // Every row is as tall as its face really is: on Read Pico the UI follows the chosen
  // reading font, which can be taller than the built-in one, and a short box cuts glyphs.
  const int captionHeight = std::max(28, renderer.getLineHeight(design::CAPTION));
  design::secondaryText(renderer, box(x, main.y + pad, width, captionHeight), tr(STR_RICKY_HOME_NOW), design::CAPTION,
                        selectedCard);
  const int titleFont = design::bookTitleFontFor(renderer, titleOf(book), design::HEADLINE);
  const int titleLine = std::max(40, renderer.getLineHeight(titleFont));
  const bool singleTitle = renderer.getTextWidth(titleFont, titleOf(book)) <= width;
  const int titleY = main.y + pad + captionHeight + 4;
  design::bookTitle(renderer, box(x, titleY, width, singleTitle ? titleLine : titleLine * 2), titleOf(book),
               design::HEADLINE, selectedCard, fui::TextAlign::Left, 2);
  // Reserve the author row only when the title fits one line. Two title lines
  // otherwise share that space; neither may collide with progress or the CTA.
  int upperBottom = titleY + (singleTitle ? titleLine : titleLine * 2);
  if (!book.author.empty() && singleTitle) {
    design::secondaryText(renderer, box(x, upperBottom + 8, width, captionHeight), book.author.c_str(),
                          design::CAPTION, selectedCard);
    upperBottom += 8 + captionHeight;
  }
  // The card itself opens the book; its last row says so in words instead of a black
  // button: the progress on the left, "继续阅读 ›" on the right.
  const int linkHeight = std::max(36, renderer.getLineHeight(design::BODY));
  const int linkY = main.bottom() - pad - linkHeight;
  const int barY = linkY - 20;
  design::progress(renderer, box(x, barY, width, 4), progress, selectedCard);
  const auto* saved = READING_STATS.findMatchingBookForPath(book.path);
  const int chapterY = barY - 8 - captionHeight;
  if (saved && !saved->chapterTitle.empty() && chapterY >= upperBottom + 4)
    design::secondaryText(renderer, box(x, chapterY, width, captionHeight), saved->chapterTitle.c_str(),
                          design::CAPTION, selectedCard);
  snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS), static_cast<unsigned>(progress));
  // Both sides share the row's bottom so the caption and the link sit on one line.
  const int progressHeight = std::min(linkHeight, captionHeight);
  design::secondaryText(renderer, box(x, linkY + linkHeight - progressHeight, width / 2, progressHeight), statsText,
                        design::CAPTION, selectedCard);
  constexpr int kChevron = 24;
  design::text(renderer, box(x + width / 2, linkY, width - width / 2 - kChevron - 4, linkHeight),
               tr(STR_CONTINUE_READING), design::BODY, selectedCard, fui::TextAlign::Right);
  if (!selectedCard) RickyPageUi::chevron(target, box(x + width - kChevron, linkY, kChevron, linkHeight));
  screen.frame().hit(main, OPEN_BOOK, start, fui::InputTouch);

  // Today and the streak. The dog reacts to today against the goal; the streak card
  // shows the last seven days as paw prints and the next milestone.
  const auto stats = screen.takeTop(184, ds::space::k6);
  const uint64_t todayMs = READING_STATS.getTodayReadingMs();
  const unsigned todayMinutes = todayMs / 60000;
  const uint64_t goalMs = rickyStreak::goalMs();
  const unsigned goal = goalMs / 60000;
  readingStreak::Mark week[7];
  const uint32_t today = READING_STATS.getTodayDayOrdinal();
  const auto streak = rickyStreak::evaluate(week, today >= 6 ? today - 6 : 0, 7);
  const int col = (stats.width - gap) / 2;
  // No boxes: one light rule over the section and a light divider between the columns.
  design::rule(renderer, box(stats.x, stats.y, stats.width, 0));
  design::fill(renderer, box(stats.x + col + gap / 2 - 1, stats.y + 24, ds::stroke::kHairline, stats.height - 32),
               ds::gray::kLine);
  for (int i = 0; i < 2; ++i) {
    // The left column lines up with the page edge; the right one starts after the divider.
    const auto cell = box(stats.x + i * (col + gap) - (i == 0 ? pad : 0), stats.y, col + (i == 0 ? pad : 0),
                          stats.height);
    design::secondaryText(renderer, box(cell.x + pad, cell.y + 12, col - pad * 2, 28),
                          i == 0 ? tr(STR_RICKY_TODAY_READING) : tr(STR_RICKY_STREAK_READING), design::CAPTION);
    snprintf(statsText, sizeof(statsText), "%u", i == 0 ? todayMinutes : static_cast<unsigned>(streak.current));
    const int numberFont = design::fontFor(renderer, design::NUMERIC_DISPLAY, statsText);
    const int numberWidth = renderer.getTextWidth(numberFont, statsText);
    design::text(renderer, box(cell.x + pad, cell.y + 40, numberWidth + 8, 56), statsText, design::NUMERIC_DISPLAY);
    design::secondaryText(renderer, box(cell.x + pad + numberWidth + 8, cell.y + 64, 64, 28),
                          i == 0 ? tr(STR_RICKY_HOME_MINUTES_UNIT) : tr(STR_RICKY_HOME_DAYS_UNIT), design::CAPTION);
    if (i == 0) {
      const int dog = kRickyMascotSmallSize;
      rickyMascot::draw(renderer, rickyMascot::moodFor(todayMs, goalMs), true, cell.right() - pad - dog + 8,
                        cell.y + 16);
      design::progress(renderer, box(cell.x + pad, cell.y + 128, col - pad * 2, 8),
                       goal ? std::min<uint64_t>(100, uint64_t(todayMinutes) * 100 / goal) : 0);
      if (todayMs < 60000)
        snprintf(statsText, sizeof(statsText), "%s", tr(STR_RICKY_HOME_TODAY_IDLE));
      else if (todayMs < goalMs)
        snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_TODAY_TO_GOAL),
                 static_cast<unsigned>((goalMs - todayMs + 59999) / 60000));
      else
        snprintf(statsText, sizeof(statsText), "%s", RickyCompanion::text(StrId::STR_RICKY_HOME_TODAY_DONE, StrId::STR_RICKY_HOME_TODAY_DONE_CAT));
      design::secondaryText(renderer, box(cell.x + pad, cell.y + 148, col - pad * 2, 28), statsText, design::CAPTION);
    } else {
      if (streak.restCards > 0) {
        snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_REST_CARDS), static_cast<unsigned>(streak.restCards));
        design::secondaryText(renderer, box(cell.x + col / 2, cell.y + 12, col / 2 - pad, 28), statsText,
                              design::CAPTION, false, fui::TextAlign::Right);
      }
      // Oldest on the left; today last, with a dot under it.
      constexpr int kPaw = 26;
      const int step = (col - pad * 2 - kPaw) / 6;
      for (int d = 0; d < 7; ++d) {
        const int x = cell.x + pad + d * step;
        rickyPaw::draw(renderer, x, cell.y + 100, kPaw, week[d]);
        if (d == 6) design::fill(renderer, box(x + kPaw / 2 - 2, cell.y + 130, 4, 4), ds::gray::kInk, 2);
      }
      const uint32_t next = readingStreak::nextMilestone(streak.current);
      if (streak.current == 0)
        snprintf(statsText, sizeof(statsText), "%s", tr(STR_RICKY_HOME_STREAK_START));
      else if (next == 0)
        snprintf(statsText, sizeof(statsText), "%s", tr(STR_RICKY_HOME_STREAK_BEYOND));
      else
        snprintf(statsText, sizeof(statsText), tr(STR_RICKY_HOME_STREAK_NEXT),
                 static_cast<unsigned>(next - streak.current), static_cast<unsigned>(next));
      design::secondaryText(renderer, box(cell.x + pad, cell.y + 148, col - pad * 2, 28), statsText, design::CAPTION);
    }
    screen.frame().hit(cell, OPEN_BOOK, STATISTICS, fui::InputTouch);
  }
  auto recent = screen.takeTop(40, 12);
  design::text(renderer, recent, tr(STR_RICKY_HOME_RECENT), design::HEADLINE);
  const auto all = box(recent.right() - 104, recent.y - 16, 104, 64);
  design::secondaryText(renderer, box(all.x, recent.y, all.width, 40), tr(STR_RICKY_HOME_ALL), design::CAPTION, false,
                        fui::TextAlign::Right);
  screen.frame().hit(all, OPEN_BOOK, LIBRARY, fui::InputTouch);
  const auto shelf = screen.body();
  const int count = std::min(RickyHomeLayout::PAGE_BOOKS - 1, static_cast<int>(books->size()) - start - 1);
  const int cellW = (shelf.width - gap * 3) / 4;
  const int thumbH = std::min(cellW * 3 / 2, std::max(1, shelf.height - 120));
  const int thumbW = thumbH * 2 / 3;
  for (int i = 0; i < count; ++i) {
    const int index = start + 1 + i;
    const auto& item = (*books)[index];
    const auto cell = box(shelf.x + i * (cellW + gap), shelf.y, cellW, thumbH + 120);
    const auto cover = box(cell.x + (cellW - thumbW) / 2, cell.y, thumbW, thumbH);
    const bool selectedBook = showSelection && selected == index;
    if (selectedBook)
      design::fill(renderer, box(cell.x - 4, cell.y - 8, cell.width + 8, cell.height + 8), ds::gray::kInk,
                   ds::radius::kControl);
    paint(cover, index);
    // Titles are any text: the full-coverage body face for every one, so a row never
    // mixes two sizes (the caption face covers UI strings only).
    design::bookTitle(renderer, box(cell.x, cover.bottom() + 12, cell.width, 36), titleOf(item), design::BOOK_TITLE,
                 selectedBook, fui::TextAlign::Center);
    const auto* reading = READING_STATS.findMatchingBookForPath(item.path);
    if (reading && reading->lastProgressPercent >= 100)
      snprintf(statsText, sizeof(statsText), "%s", tr(STR_RICKY_BOOK_FINISHED));
    else if (reading && reading->lastProgressPercent)
      snprintf(statsText, sizeof(statsText), tr(STR_RICKY_READ_PROGRESS), unsigned(reading->lastProgressPercent));
    else
      snprintf(statsText, sizeof(statsText), "%s", tr(STR_RICKY_READ_UNREAD));
    design::secondaryText(renderer, box(cell.x, cover.bottom() + 52, cell.width, 32), statsText, design::CAPTION,
                          selectedBook, fui::TextAlign::Center);
    screen.frame().hit(cell, OPEN_BOOK, index, fui::InputTouch);
  }
}

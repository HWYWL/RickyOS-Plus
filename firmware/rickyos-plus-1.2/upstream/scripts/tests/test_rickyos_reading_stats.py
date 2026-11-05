"""RickyOS reading statistics: one page, the brand puppy reacting to today's reading."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PAGE = ROOT / 'src/activities/apps/reading-stats/RickyReadingStatsActivity.cpp'


def body(source, signature):
    start = source.index(signature)
    depth = 0
    for index in range(source.index('{', start), len(source)):
        depth += {'{': 1, '}': -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(signature)


class RickyReadingStatsTest(unittest.TestCase):
    def test_apps_entry_opens_the_single_page(self):
        menu = (ROOT / 'src/activities/apps/AppsMenuActivity.cpp').read_text()
        self.assertIn('{AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, '
                      '&ActivityManager::goToRickyReadingStats}', menu)
        header = (ROOT / 'src/activities/apps/reading-stats/RickyReadingStatsActivity.h').read_text()
        self.assertIn('int listCount() const override { return 0; }', header)  # nothing to drill into

    def test_mascot_follows_today_against_the_goal(self):
        mascot = (ROOT / 'src/components/RickyMascot.h').read_text()
        self.assertIn('if (todayMs < 60ULL * 1000ULL) return Mood::Sleeping;', mascot)
        self.assertIn('return todayMs >= goalMs ? Mood::Happy : Mood::Reading;', mascot)
        build = body(PAGE.read_text(), 'void RickyReadingStatsActivity::buildScreen(')
        self.assertIn('rickyMascot::moodFor(todayMs, goalMs)', build)
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertIn('rickyMascot::moodFor(todayMs, goalMs), true', home)
        icons = (ROOT / 'src/components/icons/rickyMascotIcons.h').read_text()
        for prefix, size in (('ricky_mascot', 150), ('ricky_mascot_small', 88)):
            for name in ('sleeping', 'cheering', 'happy'):
                data = re.search(rf'{prefix}_{name}\[\] = \{{([^}}]+)\}}', icons).group(1)
                self.assertEqual(len(re.findall(r'0x[0-9A-F]{2}', data)), (size + 7) // 8 * size)

    def test_month_calendar_uses_the_reading_streak(self):
        page = PAGE.read_text()
        build = body(page, 'void RickyReadingStatsActivity::buildScreen(')
        self.assertIn('rickyStreak::evaluate(marks, monthStart, monthDays)', build)
        self.assertIn('rickyPaw::draw(renderer', build)
        self.assertIn('ACTION_DAY', build)
        # Months move within the reader's history, by touch or the page buttons.
        self.assertIn('std::clamp(month, firstMonth_, currentMonth_)', body(page, 'void RickyReadingStatsActivity::showMonth('))
        keys = body(page, 'bool RickyReadingStatsActivity::handleCustomInput(')
        for button in ('Left', 'PageBack', 'Right', 'PageForward'):
            self.assertIn(f'Button::{button}', keys)
        # The home card and header share the same streak; the store's goal-only count
        # stays for the achievements.
        home = (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        self.assertNotIn('getCurrentStreakDays', home)
        self.assertIn('READING_STATS.getCurrentStreakDays()', (ROOT / 'src/AchievementsStore.cpp').read_text())

    def test_week_starts_monday_and_facts_use_real_data(self):
        page = PAGE.read_text()
        self.assertIn('return static_cast<int>((dayOrdinal + 3) % 7);', page)  # 1970-01-01 was a Thursday
        fact = body(page, 'std::string dailyFact(')
        for source in ('getSessionLog()', 'getReadingDays()', 'getBooks()', 'getBooksFinishedCount()'):
            self.assertIn(source, fact)
        self.assertIn('return facts[today % facts.size()];', fact)

    def test_strings_exist_and_fit_the_ui_fonts(self):
        sources = PAGE.read_text() + (ROOT / 'src/components/RickyHomeUi.cpp').read_text()
        keys = set(re.findall(r'STR_RICKY_(?:STATS|HOME)_\w+', sources))
        self.assertGreater(len(keys), 25)
        chinese = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text()
        for language in ('chinese', 'english'):
            strings = (ROOT / f'lib/I18n/translations/{language}.yaml').read_text()
            for key in keys:
                self.assertRegex(strings, rf'(?m)^{key}: "', (language, key))
        # The streak counts reading days now, not goal days.
        self.assertIn('STR_RICKY_STATS_STREAK: "连续 %u 天 · 最长 %u 天"', chinese)
        # New Chinese copy stays within the caption face's characters (no body-font fallback).
        caption = (ROOT / 'lib/EpdFont/builtinFonts/ricky_caption_10_regular.h').read_text()
        block = caption[caption.index('ricky_caption_10_regularIntervals[] = {'):]
        block = block[:block.index('};')]
        covered = set()
        for first, last in re.findall(r'\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)', block):
            covered.update(range(int(first, 16), int(last, 16) + 1))
        for key in ('STR_RICKY_HOME_TODAY_IDLE', 'STR_RICKY_HOME_TODAY_TO_GOAL', 'STR_RICKY_HOME_TODAY_DONE',
                    'STR_RICKY_HOME_STREAK_START', 'STR_RICKY_HOME_STREAK_NEXT', 'STR_RICKY_HOME_STREAK_BEYOND',
                    'STR_RICKY_HOME_REST_CARDS', 'STR_RICKY_STATS_LEGEND_GOAL', 'STR_RICKY_STATS_LEGEND_READ',
                    'STR_RICKY_STATS_LEGEND_REST', 'STR_RICKY_STATS_MONTH_TOTAL', 'STR_RICKY_STATS_STREAK_REST'):
            value = re.search(rf'(?m)^{key}: "(.*)"$', chinese).group(1)
            missing = [c for c in value if ord(c) > 127 and ord(c) not in covered]
            self.assertEqual(missing, [], key)


    def test_badges_and_report_are_reachable_and_translated(self):
        page = PAGE.read_text()
        for opener in ('void RickyReadingStatsActivity::openBadges(', 'void RickyReadingStatsActivity::openReport('):
            self.assertIn(opener, page)
        self.assertIn('startActivityForResultWith<RickyBadgesActivity>', page)
        self.assertIn('startActivityForResultWith<RickyMonthReportActivity>', page)
        reader = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
        self.assertIn('rickyBadgeNotice::announceNew(renderer);', reader)
        self.assertIn('rickyBadgeNotice::seedOnce();', (ROOT / 'src/main.cpp').read_text())
        sources = ''.join((ROOT / path).read_text() for path in (
            'src/activities/apps/reading-stats/RickyBadgesActivity.cpp',
            'src/activities/apps/reading-stats/RickyMonthReportActivity.cpp',
            'src/components/RickyBadgeStatus.h', 'src/components/RickyBadgeNotice.h',
            'src/components/RickyMonthSummary.h'))
        keys = set(re.findall(r'STR_RICKY_(?:BADGE|BADGES|REPORT)_\w+', sources + page))
        self.assertGreater(len(keys), 60)
        caption = (ROOT / 'lib/EpdFont/builtinFonts/ricky_caption_10_regular.h').read_text()
        block = caption[caption.index('ricky_caption_10_regularIntervals[] = {'):]
        block = block[:block.index('};')]
        covered = set()
        for first, last in re.findall(r'\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)', block):
            covered.update(range(int(first, 16), int(last, 16) + 1))
        for language in ('chinese', 'english'):
            strings = (ROOT / f'lib/I18n/translations/{language}.yaml').read_text()
            for key in keys:
                match = re.search(rf'(?m)^{key}: "(.*)"$', strings)
                self.assertIsNotNone(match, (language, key))
                if language == 'chinese':
                    missing = [c for c in match.group(1) if ord(c) > 127 and ord(c) not in covered]
                    self.assertEqual(missing, [], key)
        # One name and one hint per badge, in the badge order.
        status = (ROOT / 'src/components/RickyBadgeStatus.h').read_text()
        self.assertEqual(len(re.findall(r'StrId::STR_RICKY_BADGE_\w+_HINT', status)), 19)


if __name__ == '__main__':
    unittest.main()

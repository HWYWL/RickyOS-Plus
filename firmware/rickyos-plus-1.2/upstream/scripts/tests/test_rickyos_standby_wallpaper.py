"""RickyOS Standby: the user's picture in native 16-gray with an optional date/time corner."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.index(signature)
    depth = 0
    for index in range(source.index('{', start), len(source)):
        depth += {'{': 1, '}': -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(signature)


class RickyStandbyWallpaperTest(unittest.TestCase):
    def setUp(self):
        self.face = (ROOT / 'src/activities/apps/standby/WallpaperFace.cpp').read_text()
        self.activity = (ROOT / 'src/activities/apps/standby/StandbyActivity.cpp').read_text()

    def test_product_offers_only_the_wallpaper_face(self):
        table = self.activity.split('constexpr FaceEntry kFaces[] = {', 1)[1].split('\n};', 1)[0]
        product = table
        self.assertIn('makeUniqueNoThrow<WallpaperFace>()', product)
        self.assertNotIn('SloppyClockFace', product)
        self.assertNotIn('ChineseCalendarFace', product)

    def test_picture_is_the_settings_wallpaper_in_native_16_gray(self):
        self.assertIn('constexpr char kPicture[] = "/sleep.bmp";', self.face)
        self.assertIn('constexpr const char* SLEEP_IMAGE_PATH = "/sleep.bmp";',
                      (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text())
        native = body(self.face, 'bool WallpaperFace::renderNative(')
        order = [native.index(call) for call in ('beginGrayscale16()', 'drawBitmapGrayscale16(',
                                                 'drawCorner(renderer, viewport, true)', 'commitGrayscale16()')]
        self.assertEqual(order, sorted(order))
        self.assertIn('renderer.cancelGrayscale16();', native)

    def test_every_update_redraws_the_whole_16_gray_frame(self):
        # No partial refresh on this panel: a B/W refresh after a 16-gray frame
        # flattened the picture, so minute ticks redraw it through the gray frame.
        native = body(self.face, 'bool WallpaperFace::renderNative(')
        self.assertNotIn('displayBuffer', native)
        self.assertNotIn('pictureShown_', self.face)
        tick = body(self.face, 'StandbyFace::TickResult WallpaperFace::tick()')
        self.assertIn('return TickResult::Redraw;', tick)
        corner = body(self.face, 'void WallpaperFace::drawCorner(')
        self.assertIn('getTextWidth(kTimeFont, "00:00", EpdFontFamily::BOLD)', corner)
        self.assertIn('renderer.copyBwToGrayscale16(x, y, width, height, kCornerRadius)', corner)

    def test_image_preview_starts_and_ends_on_a_full_refresh(self):
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        enter = body(viewer, 'void ImageViewerActivity::onEnter()')
        product = enter[enter.index('// No blank page first'):enter.index('const bool needsPreview')]
        # No blank page before the loading popup (a white screen read as a hang); the
        # picture itself lands in one clean full GC16 refresh.
        self.assertNotIn('displayBuffer', product)
        native = enter[enter.index('Native 16-gray, as Standby'):]
        self.assertLess(native.index('display.setNextGray16Profile(3);'), native.index('commitGrayscale16()'))
        exit_ = body(viewer, 'void ImageViewerActivity::onExit()')
        self.assertIn('renderer.displayBuffer(HalDisplay::FULL_REFRESH);', exit_.split('#else', 1)[0])

    def test_activity_is_full_screen_and_syncs_only_for_a_clock(self):
        enter = body(self.activity, 'void StandbyActivity::onEnter()')
        self.assertIn('mode_ = DisplayMode::Immersive;', enter.split('#else', 1)[0])
        self.assertIn('currentFace_->wantsClock()', enter)
        self.assertIn('if (currentFace_->needsPicture()) return false;', body(self.activity, 'bool StandbyActivity::tryLightSleep('))
        render = body(self.activity, 'void StandbyActivity::render(')
        self.assertLess(render.index('renderNative(renderer, viewport)'), render.index('renderer.clearScreen();'))
        # Leaving a 16-gray picture: the next page must not refresh differentially over it.
        self.assertIn('renderer.requestNextRefresh(HalDisplay::FULL_REFRESH);', body(self.activity, 'void StandbyActivity::onExit()'))
        picker = body(self.activity, 'void StandbyActivity::openPicturePicker(')
        self.assertIn('FileBrowserActivity::Mode::PickWallpaper', picker)
        self.assertIn('startActivityForResultWith<ImageViewerActivity>(reload, entry->path, true)', picker)

    def test_overlay_setting_is_persisted_and_translated(self):
        header = (ROOT / 'src/CrossPointSettings.h').read_text()
        self.assertIn('uint8_t standbyOverlay = STANDBY_OVERLAY_NONE;', header)  # plain picture by default
        settings = (ROOT / 'src/SettingsList.h').read_text()
        entry = settings[settings.index('&CrossPointSettings::standbyOverlay'):][:400]
        self.assertIn('"standbyOverlay"', entry)
        self.assertEqual(len(re.findall(r'STR_RICKY_STANDBY_INFO_\w+', entry)), 3)
        for language in ('chinese', 'english'):
            strings = (ROOT / f'lib/I18n/translations/{language}.yaml').read_text()
            for key in ('STR_RICKY_STANDBY_INFO', 'STR_RICKY_STANDBY_INFO_NONE', 'STR_RICKY_STANDBY_INFO_DATE',
                        'STR_RICKY_STANDBY_INFO_TIME', 'STR_RICKY_STANDBY_EMPTY'):
                self.assertRegex(strings, rf'(?m)^{key}: "')
        self.assertIn('field == &CrossPointSettings::standbyOverlay',
                      (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text())


    def test_apps_entry_opens_the_settings_page_not_standby(self):
        menu = (ROOT / 'src/activities/apps/AppsMenuActivity.cpp').read_text()
        self.assertIn('{AppId::Standby, StrId::STR_RICKY_POWER_PAGE_TITLE, UIIcon::Standby, '
                      '&ActivityManager::goToStandbySettings}', menu)
        page = (ROOT / 'src/activities/apps/standby/RickyStandbySettingsActivity.cpp').read_text()
        activate = body(page, 'void RickyStandbySettingsActivity::activateIndex(')
        self.assertIn('SETTINGS.rickyStandbyFace = kStyles[chosen];', activate)
        self.assertIn('SETTINGS.rickyPowerOffScreen = static_cast<uint8_t>(chosen);', activate)
        self.assertIn('SETTINGS.standbyOverlay = static_cast<uint8_t>(chosen);', activate)
        self.assertIn('openPicturePicker();', activate)
        self.assertIn('startActivityForResultWith<RickyWallpaperDownloadActivity>', activate)
        self.assertIn('startActivityForResultWith<StandbyActivity>', activate)
        self.assertIn('static_assert(std::size(kStyles) == Settings::RICKY_STANDBY_FACE_COUNT);', page)
        # Full-screen Standby returns to whichever page opened it.
        loop = body(self.activity, 'void StandbyActivity::loop()')
        self.assertIn('finish();', loop.split('#else', 1)[0])
        for language in ('chinese', 'english'):
            strings = (ROOT / f'lib/I18n/translations/{language}.yaml').read_text()
            self.assertRegex(strings, r'(?m)^STR_RICKY_POWER_OFF_SCREEN: "')
            self.assertRegex(strings, r'(?m)^STR_RICKY_STANDBY_NOW: "')


    def test_setting_a_picked_picture_returns_quickly(self):
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        install = body(viewer, 'bool ImageViewerActivity::doSetSleepCover(')
        self.assertIn('static char buffer[4096];', install)
        options = body(viewer, 'void ImageViewerActivity::showSleepCoverOptions()')
        self.assertEqual(options.count('wallpaperPicker'), 3)  # product and legacy PNG paths
        self.assertEqual(options.count('finish();'), 4)
        self.assertIn('index == 1 ? doSetSticker()', options)
        self.assertIn('!activityManager.isSwitchPending()) onEnter();', body(viewer, 'void ImageViewerActivity::loop()'))


    def test_preview_draws_native_16_gray_with_a_readable_button(self):
        viewer = (ROOT / 'src/activities/util/ImageViewerActivity.cpp').read_text()
        enter = body(viewer, 'void ImageViewerActivity::onEnter()')
        native = enter[enter.index('Native 16-gray, as Standby'):]
        native = native[:native.index('return;')]
        order = [native.index(call) for call in ('beginGrayscale16()', 'drawBitmapGrayscale16(',
                                                 'GUI.drawActionButton(', 'copyBwToGrayscale16(', 'commitGrayscale16()')]
        self.assertEqual(order, sorted(order))
        self.assertIn('imageReady = shown;', native)


    def test_settings_neither_repeat_nor_link_the_standby_page(self):
        settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        reorganize = body(settings, 'void SettingsActivity::reorganizeRickySettings()')
        moved = reorganize[reorganize.index('const auto elsewhere'):]
        moved = moved[:moved.index('for (auto* list')]
        self.assertIn('&CrossPointSettings::sleepScreen', moved)
        self.assertIn('&CrossPointSettings::standbyOverlay', moved)
        self.assertNotIn('RickyStandbyPage', settings)  # Apps has the one entry
        self.assertNotIn('RickySleepWallpaper', settings)
        self.assertNotIn('RickyWallpaperDownload', settings)

    def test_library_search_stays_shut_without_chinese_input(self):
        library = (ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text()
        search = body(library, 'void LibraryListActivity::openSearch()')
        self.assertIn('return;', search)
        self.assertNotIn('STR_SEARCH', body(library, 'void LibraryListActivity::drawFooter()'))


    def test_without_a_picture_standby_shows_the_rest_screen(self):
        render = body(self.face, 'void WallpaperFace::render(')
        rest = render.split('if (!hasPicture_) {', 1)[1].split('return;', 1)[0]
        for piece in ('RickyBrandMark::draw(renderer, layout.mark, 3)',
                      'tr(STR_RICKY_BRAND_TAGLINE)', 'drawCorner(renderer, viewport, false)'):
            self.assertIn(piece, rest)
        self.assertNotIn('tr(STR_RICKY_REST)', rest)
        self.assertNotIn('drawLine(', rest)
        self.assertNotIn('needsPicture', (ROOT / 'src/activities/apps/standby/WallpaperFace.h').read_text())
        self.assertIn('STR_RICKY_SLEEP_LIGHT: "默认画面"', (ROOT / 'lib/I18n/translations/chinese.yaml').read_text())


    def test_unset_timezone_is_china_standard_time(self):
        zones = (ROOT / 'src/util/Timezones.cpp').read_text()
        product = zones
        self.assertIn('constexpr uint8_t DEFAULT_INDEX = 36;', product)
        self.assertIn('static_assert(TABLE[DEFAULT_INDEX].stdOffsetQ == 32', product)
        self.assertIn('return DEFAULT_INDEX;', body(zones, 'uint8_t activeIndex()'))


    def test_sleep_follows_the_standby_screen_mode_only(self):
        # The separate keep-page switch could stay on after the mode changed on the
        # Standby page and override the chosen picture on automatic sleep.
        for path, start in (('src/main.cpp', 'const bool isQuickResumeSleep ='),
                            ('src/activities/boot_sleep/SleepActivity.cpp',
                             'const bool renderQuickResume = sleepScreenMode()')):
            source = (ROOT / path).read_text()
            product = re.split(re.escape(start).replace(r'\ ', r'\s+'), source, maxsplit=1)[1].split('#else', 1)[0]
            self.assertNotIn('quickResumeSleepScreen', product, path)
        settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        hidden = settings[settings.index('const auto elsewhere'):]
        self.assertIn('&CrossPointSettings::quickResumeSleepScreen', hidden[:hidden.index('for (auto* list')])


if __name__ == '__main__':
    unittest.main()

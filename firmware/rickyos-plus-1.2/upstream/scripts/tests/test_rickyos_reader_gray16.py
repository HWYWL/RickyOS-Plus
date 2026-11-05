"""RickyOS anti-aliased pages: one 16-level render, text-turn commits, native JPEGs."""
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
READER = ROOT / 'src/activities/reader/EpubReaderActivity.cpp'


def load_patch():
    spec = importlib.util.spec_from_file_location('patch_rickyos_epdiy', ROOT / 'scripts/patch_rickyos_epdiy.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def pristine(path):
    """The SDK file as pinned, before the build-time patch."""
    rel = Path(path).relative_to(ROOT / 'freeink-sdk')
    return subprocess.run(['git', '-C', str(ROOT / 'freeink-sdk'), 'show', f'HEAD:{rel}'],
                          check=True, capture_output=True, text=True).stdout


class RickyReaderGray16Test(unittest.TestCase):
    def test_rickyos_takes_the_16_level_path_for_every_face_bold_and_jpeg_pages(self):
        source = READER.read_text()
        start = source.index('  // RickyOS: every text face')
        rule = source[start:source.index('const bool needsAnyGrayscale', start)]
        self.assertIn('page->imagesAreAllJpeg()', rule)
        self.assertNotIn('fakeBold', rule)
        self.assertNotIn('readerFaceIsFourBit', rule)
        # The B/W pre-render is skipped and the commit picks text turn or the GC16 cleanup.
        self.assertIn('if (use16LevelText) {\n    // The 16-level pass below clears the frame', source)
        self.assertIn('display.setNextGray16Profile(refresh == HalDisplay::FAST_REFRESH ? 1 : 2);', source)
        # Cached 16-level pages are copied, not re-rendered.
        self.assertIn('memcpy(renderer.grayscale16Frame(), pageCache16_[pageCacheLiveSlot_].get()', source)
        self.assertIn('renderer.beginGrayscale16Offscreen(pageCache16_[slot].get())', source)

    def test_epdiy_patch_applies_once_and_refuses_changed_sources(self):
        patch = load_patch()
        cpp = ROOT / 'freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp'
        header = ROOT / 'freeink-sdk/libs/display/EpdiyLcd/include/EpdiyLcd.h'
        text = pristine(cpp)
        patched = patch.patch_text(text, patch.OLD_COMMIT, patch.NEW_COMMIT)
        self.assertIn(patch.MARK, patched)
        self.assertEqual(patch.patch_text(patched, patch.OLD_COMMIT, patch.NEW_COMMIT), patched)  # re-runnable
        with self.assertRaises(RuntimeError):
            patch.patch_text(text.replace(patch.OLD_COMMIT, patch.OLD_COMMIT.replace('bwProxy', 'proxy')),
                             patch.OLD_COMMIT, patch.NEW_COMMIT)
        # The header gets every RickyOS declaration once, however many earlier builds ran
        # (separate steps on one anchor used to re-insert each other on every build).
        text = pristine(header)
        once = patch.patch_header(text)
        self.assertEqual(patch.patch_header(once), once)
        doubled = once.replace(patch.HEADER_BLOCK, patch.HEADER_BLOCK * 3)
        self.assertEqual(patch.patch_header(doubled), once)
        for decl in ('void epdiyLcdSetGray16Profile(uint8_t profile);', 'void epdiyLcdRailsOffIfIdle(uint32_t idleMs);',
                     'bool epdiyLcdRestart();'):
            self.assertEqual(once.count(decl), 1)
        with self.assertRaises(RuntimeError):
            patch.patch_header(text.replace('bwProxy', 'proxy'))
        # Reader turns keep the rails up; long-lived frames still power down.
        self.assertIn('const bool turnOff = g_gray16Profile == 0 || g_gray16Profile == 3;', patch.NEW_COMMIT)

    def test_rails_drop_after_frames_stop(self):
        patch = load_patch()
        cpp = pristine(ROOT / 'freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp')
        for old, new in ((patch.OLD_TIMER_INCLUDE, patch.NEW_TIMER_INCLUDE), (patch.OLD_POWER, patch.NEW_POWER),
                         (patch.OLD_POWER_STATE, patch.NEW_POWER_STATE), (patch.OLD_IDLE_FN, patch.NEW_IDLE_FN)):
            cpp = patch.patch_text(cpp, old, new, patch.RAILS_MARK)
            self.assertEqual(patch.patch_text(cpp, old, new, patch.RAILS_MARK), cpp)  # re-runnable
        self.assertIn('g_railsUp = true;', cpp)
        self.assertIn('void epdiyLcdRailsOffIfIdle(uint32_t idleMs) {', cpp)
        # Held-up rails let charge build in the film (old pages show through): the main
        # loop drops them once frames stop, never during a push.
        manager = (ROOT / 'src/activities/ActivityManager.cpp').read_text()
        self.assertIn('RenderLock railsLock(RenderLock::Mode::Try);', manager)
        self.assertIn('if (railsLock.ownsLock()) display.railsOffIfIdle(3000);', manager)

    def test_line_queues_start_each_phase_empty_and_repair_rebuilds_the_scan_path(self):
        patch = load_patch()
        cpp = pristine(ROOT / 'freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp')
        ctx = pristine(ROOT / 'freeink-sdk/libs/display/EpdiyLcd/src/epdiy/src/output_common/render_context.c')
        for old, new, mark in ((patch.OLD_RESTART_FN, patch.NEW_RESTART_FN, patch.RESTART_MARK),
                               (patch.OLD_DIAG_STATS, patch.NEW_DIAG_STATS, patch.LQ_MARK),
                               (patch.OLD_DIAG_PRINT, patch.NEW_DIAG_PRINT, patch.LQ_MARK)):
            cpp = patch.patch_text(cpp, old, new, mark)
            self.assertEqual(patch.patch_text(cpp, old, new, mark), cpp)
        for old, new in ((patch.OLD_LQ_FN, patch.NEW_LQ_FN), (patch.OLD_LQ_RESET, patch.NEW_LQ_RESET)):
            ctx = patch.patch_text(ctx, old, new, patch.LQ_MARK)
            self.assertEqual(patch.patch_text(ctx, old, new, patch.LQ_MARK), ctx)
        # Lines a late interrupt left in a queue shifted every later phase (a lighter copy of
        # the page beside itself until reboot): both queues are emptied before each phase,
        # before the feed threads start on it.
        prepare = ctx[ctx.index('void IRAM_ATTR prepare_context_for_next_frame(RenderContext_t* ctx) {'):]
        self.assertLess(prepare.index('lq_reset(lq);'), prepare.index('ctx->lines_prepared = 0;'))
        self.assertIn(' stale=%u/%u', cpp)
        self.assertIn('bool epdiyLcdRestart() {', cpp)
        repair = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        repair = repair[repair.index('case SettingAction::RickyScreenRepair: {'):]
        self.assertLess(repair.index('display.restartPanel();'), repair.index('renderer.clearScreen(0x00);'))

    def test_jpeg_detection_for_native_images(self):
        header = (ROOT / 'lib/Epub/Epub/blocks/ImageBlock.h').read_text()
        start = header.index('  static bool isJpegPath(')
        body = header[start:header.index('\n  }\n', start) + 4]
        program = '#include <cassert>\n#include <string>\nstruct ImageBlock {\n' + body + '};\nint main(){\n' + \
            ' assert(ImageBlock::isJpegPath("a/b.jpg") && ImageBlock::isJpegPath("x.JPEG"));\n' + \
            ' assert(!ImageBlock::isJpegPath("x.png") && !ImageBlock::isJpegPath("noext"));\n}\n'
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'check.cpp'
            src.write_text(program)
            exe = Path(tmp) / 'check'
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_glyph_native_branch_is_rickyos_only(self):
        source = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
        start = source.index('else if ((is2Bit || is4Bit) && renderer.isGrayscale16Active()')
        branch = source[start:source.index('else if (is2Bit || is4Bit) {', start)]
        self.assertIn('dilate2BitCoverage', branch)
        self.assertIn('draw4BitGlyphPixel(renderer, screenX, screenY, static_cast<uint8_t>(coverage * 5))', branch)


if __name__ == '__main__':
    unittest.main()

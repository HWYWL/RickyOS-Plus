"""Run upstream 6e1d0ea7 overlay/install tests against the actual Plus sources."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests/upstream_newfeatures'))
import test_reading_ui_regressions as helper
helper.ROOT = ROOT/'firmware/rickyos-plus-1.2/upstream'
import test_rickyos_transparent_wallpaper as overlay
suite = unittest.TestSuite(overlay.TransparentWallpaperTest(name) for name in (
    'test_overlay_rollback_once_and_fit', 'test_install_copy_rename_and_settings_faults'))
result = unittest.TextTestRunner(verbosity=2).run(suite)
raise SystemExit(not result.wasSuccessful())

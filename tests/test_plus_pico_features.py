"""Run Pico's production installer/routing regressions against the actual Plus snapshot."""
import importlib.util
from pathlib import Path
import sys
import unittest

PORT = Path(__file__).resolve().parents[1]
APP = PORT / 'firmware/rickyos-plus-1.2/upstream'
FIXTURES = PORT / 'tests/pico_sync'
sys.path.insert(0, str(FIXTURES))
import test_reading_ui_regressions as shared
shared.ROOT = APP


def suite():
    result = unittest.TestSuite()
    for name in ('test_rickyos_standby_power_off', 'test_rickyos_power_off_wallpaper',
                 'test_rickyos_reader_resources'):
        spec = importlib.util.spec_from_file_location('plus_' + name, FIXTURES / (name + '.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.ROOT = APP
        result.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
    return result


if __name__ == '__main__':
    run = unittest.TextTestRunner(verbosity=2).run(suite())
    sys.exit(not run.wasSuccessful())

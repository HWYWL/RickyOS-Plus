"""Reject wrong/dirty IDF trees and stale build metadata without compiling an image."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify_idf', ROOT / 'scripts/verify_plus_release_idf.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class ReleaseIdfTest(unittest.TestCase):
    def test_clean_exact_commit(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(guard.subprocess, 'check_output',
                                                               side_effect=[guard.EXPECTED + '\n', '']):
            self.assertEqual(guard.verify(tmp), Path(tmp).resolve())

    def test_wrong_commit(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(guard.subprocess, 'check_output', return_value='fff9895c'):
            with self.assertRaisesRegex(ValueError, 'Wrong ESP-IDF'):
                guard.verify(tmp)

    def test_dirty_checkout(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(guard.subprocess, 'check_output',
                                                               side_effect=[guard.EXPECTED, ' M startup.c']):
            with self.assertRaisesRegex(ValueError, 'dirty'):
                guard.verify(tmp)

    def test_build_description(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'project.json'
            good = {'git_revision': guard.EXPECTED[:8], 'idf_path': str(Path(tmp).resolve()), 'target': 'esp32s31'}
            for change, valid in (({}, True), ({'git_revision': '9a97f6c5-dirty'}, False),
                                  ({'target': 'esp32s3'}, False), ({'idf_path': '/'}, False)):
                file.write_text(json.dumps(good | change))
                with self.subTest(change=change), patch.object(guard.subprocess, 'check_output',
                                                               side_effect=[guard.EXPECTED, '']):
                    if valid:
                        guard.verify(tmp, file)
                    else:
                        with self.assertRaises(ValueError):
                            guard.verify(tmp, file)

    def test_shell_checks_before_build_and_after(self):
        shell = (ROOT / 'scripts/build_rickyos_plus.sh').read_text()
        first = shell.index('verify_plus_release_idf.py')
        configure = shell.index('"${PLUS_IDF_COMMAND[@]}" reconfigure')
        security = shell.index('check_plus_wifi_security.py')
        private_profile = shell.index('check_plus_private_network_profile.py')
        build = shell.index('"${PLUS_IDF_COMMAND[@]}" build')
        last = shell.rindex('verify_plus_release_idf.py')
        self.assertLess(first, configure)
        self.assertLess(configure, security)
        self.assertLess(security, private_profile)
        self.assertLess(private_profile, build)
        self.assertGreater(last, build)


if __name__ == '__main__':
    unittest.main()

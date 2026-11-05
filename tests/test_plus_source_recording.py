"""Source records must use and verify the separate pinned SDK baseline."""
import importlib.util
import json
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

PORT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('plus_sync', PORT / 'scripts/sync_plus_design.py')
sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync)


class RecordingTests(unittest.TestCase):
    def test_patch_round_trip_without_final_newlines(self):
        for before, after in [(b'old', b'new'), (b'old\n', b'new'), (b'old', b'new\n'),
                              (b'', b'new'), ('字\u2028符'.encode(), '字\u2028表'.encode())]:
            with tempfile.TemporaryDirectory(prefix='plus-patch-eof-') as temporary:
                root = Path(temporary)
                original = root / 'chars.txt'
                following = root / 'next.cpp'
                if before:
                    original.write_bytes(before)
                following.write_bytes(b'old following\n')
                text = sync.unified_source_patch(before, after, 'chars.txt', bool(before))
                text += sync.unified_source_patch(b'old following\n', b'new following\n', 'next.cpp', True)
                patch_file = root / 'changes.patch'
                patch_file.write_text(text)
                subprocess.run(['patch', '--batch', '-p1', '-i', str(patch_file)], cwd=root, check=True,
                               stdout=subprocess.DEVNULL)
                self.assertEqual(original.read_bytes(), after)
                self.assertEqual(following.read_bytes(), b'new following\n')
                subprocess.run(['patch', '--batch', '-R', '-p1', '-i', str(patch_file)], cwd=root, check=True,
                               stdout=subprocess.DEVNULL)
                if before:
                    self.assertEqual(original.read_bytes(), before)
                else:
                    self.assertFalse(original.exists())

    def record(self, baseline, expected_success):
        with tempfile.TemporaryDirectory(prefix='plus-record-test-') as temporary:
            root = Path(temporary)
            upstream = root / 'upstream'
            name = 'freeink-sdk/libs/example.cpp'
            before, after = b'old SDK line\n', b'new SDK line\n'
            target = upstream / name
            target.parent.mkdir(parents=True)
            target.write_bytes(after)
            outputs = {key: root / (key.lower() + '.json') for key in ('MANIFEST', 'CHANGES', 'PATCH')}
            for file in outputs.values():
                file.write_text('existing record\n')
            manifest = {'commit': 'app-pin', 'sdk_commit': 'sdk-pin',
                        'files': {name: sync.sha(before)}}
            changes = {'changes': []}
            with patch.multiple(sync, UPSTREAM=upstream, **outputs), patch.object(sync, 'show', return_value=baseline) as show:
                if expected_success:
                    sync.finish(root / 'app', 'app-pin', manifest, changes,
                                manifest['files'], root / 'sdk')
                    show.assert_called_once_with(root / 'sdk', 'sdk-pin', 'libs/example.cpp')
                    record = json.loads(outputs['CHANGES'].read_text())['changes'][0]
                    self.assertEqual(record['baseline_sha256'], sync.sha(before))
                    self.assertEqual(record['ported_sha256'], sync.sha(after))
                    diff = outputs['PATCH'].read_text()
                    self.assertIn('-old SDK line\n+new SDK line\n', diff)
                    self.assertNotIn('/dev/null', diff)
                else:
                    with self.assertRaisesRegex(SystemExit, 'Missing/mismatched pinned source'):
                        sync.finish(root / 'app', 'app-pin', manifest, changes,
                                    manifest['files'], root / 'sdk')
                    for file in outputs.values():
                        self.assertEqual(file.read_text(), 'existing record\n')

    def test_explicit_sdk_baseline(self):
        self.record(b'old SDK line\n', True)

    def test_missing_baseline_cannot_overwrite_records(self):
        self.record(None, False)

    def test_mismatched_baseline_cannot_overwrite_records(self):
        self.record(b'wrong SDK checkout\n', False)


if __name__ == '__main__':
    unittest.main()

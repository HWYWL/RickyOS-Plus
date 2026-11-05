"""A merged charset must never compile against yesterday's packed font files."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('font_resources', PORT / 'scripts/check_plus_font_resources.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class FontResourceTests(unittest.TestCase):
    def fixture(self, root):
        records = []
        for name in guard.FACES:
            source = root / 'upstream/lib/EpdFont/builtinFonts' / f'{name}.h'
            packed = root / 'generated/fonts' / f'{name}.packed.h'
            source.parent.mkdir(parents=True, exist_ok=True)
            packed.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b'font source')
            packed.write_bytes(b'packed font')
            records.append({'name': name, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()})
        report = root / 'generated/fonts/packing-report.json'
        report.write_text(json.dumps(records))
        return records, report

    def test_current_and_stale_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            guard.verify(root)
            source = root / 'upstream/lib/EpdFont/builtinFonts/ricky_title_16_bold.h'
            source.write_bytes(b'merged background glyphs')
            with self.assertRaisesRegex(ValueError, 'Stale/missing'):
                guard.verify(root)

    def test_missing_and_empty_packed_faces(self):
        for missing in (False, True):
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                self.fixture(root)
                target = root / 'generated/fonts/ricky_title_16_bold.packed.h'
                if missing:
                    target.unlink()
                else:
                    target.write_bytes(b'')
                with self.assertRaisesRegex(ValueError, 'Stale/missing'):
                    guard.verify(root)

    def test_duplicate_report_cannot_hide_missing_face(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            records, report = self.fixture(root)
            records[-1] = records[0]
            report.write_text(json.dumps(records))
            with self.assertRaisesRegex(ValueError, 'exactly once'):
                guard.verify(root)


if __name__ == '__main__':
    unittest.main()

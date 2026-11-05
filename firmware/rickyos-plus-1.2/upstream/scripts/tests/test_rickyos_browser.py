"""Host-only browser bridge protocol, isolation, and HTTP security regressions."""
import hashlib
import http.client
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ricky_preview', ROOT / 'tools/rickyos-browser/server.py')
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


class ProtocolTests(unittest.TestCase):
    def test_status_reports_resumed_reader_after_menu_closes(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            binary = state / 'program'
            binary.touch()
            (state / 'input.sock').touch()
            (state / 'simulator.log').write_text(
                'Entering activity: EpubReader\nEntering activity: EpubReaderMenu\n'
                'Resuming activity: EpubReader\n')
            host = object.__new__(preview.Preview)
            host.lock = threading.RLock()
            host.process = SimpleNamespace(poll=lambda: None)
            host.state_dir = host.transport = state
            host.binary = binary
            host.busy = False
            host.error = ''
            self.assertEqual(host.status()['activity'], 'EpubReader')

    def test_build_uses_checkout_virtualenv_without_overwriting_core_dir(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pio = root / '.venv/bin/pio'
            pio.parent.mkdir(parents=True)
            pio.touch()
            with patch.object(preview, 'ROOT', root), patch.dict(preview.os.environ,
                    {'PLATFORMIO_CORE_DIR': '/test/custom-core'}, clear=True):
                executable, environment = preview.build_toolchain()
            self.assertEqual(executable, str(pio))
            self.assertEqual(environment['PLATFORMIO_CORE_DIR'], '/test/custom-core')

    def test_build_uses_path_pio_when_checkout_has_no_virtualenv(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(preview, 'ROOT', Path(directory)), \
                patch.object(preview.shutil, 'which', return_value='/test/bin/pio'):
            executable, _ = preview.build_toolchain()
            self.assertEqual(executable, '/test/bin/pio')

    def test_build_missing_toolchain_reports_setup_instructions(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(preview, 'ROOT', Path(directory)), \
                patch.object(preview.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'pioarduino'):
                preview.build_toolchain()

    def test_lossless_grayscale_and_dimensions(self):
        pixels = bytes([0, 255, 128, 64, 1, 2])
        raw = struct.pack('<4sIII', b'RKB1', 3, 2, 7) + pixels
        png, w, h, seq = preview.frame_to_png(raw)
        self.assertEqual((w, h, seq), (3, 2, 7))
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        offset, compressed = 8, b''
        while offset < len(png):
            length = struct.unpack_from('>I', png, offset)[0]
            kind = png[offset + 4:offset + 8]
            data = png[offset + 8:offset + 8 + length]
            crc = struct.unpack_from('>I', png, offset + 8 + length)[0]
            self.assertEqual(crc, zlib.crc32(kind + data))
            if kind == b'IDAT':
                compressed += data
            offset += length + 12
        self.assertEqual(zlib.decompress(compressed), b'\0' + pixels[:3] + b'\0' + pixels[3:])

    def test_rejects_corrupt_or_oversized_frame(self):
        for raw in (b'', b'RKB1', struct.pack('<4sIII', b'nope', 1, 1, 0) + b'\0',
                    struct.pack('<4sIII', b'RKB1', 0, 1, 0),
                    struct.pack('<4sIII', b'RKB1', 1216, 1216, 0),
                    struct.pack('<4sIII', b'RKB1', 2, 1, 0) + b'\0'):
            with self.subTest(raw=raw[:16]), self.assertRaises(ValueError):
                preview.frame_to_png(raw)

    def test_valid_keys_have_bounded_release(self):
        for key in preview.KEYS:
            self.assertEqual(preview.input_command({'type': 'key', 'key': key}), f'K {key} 100'.encode())
        for duration in (True, 0, 39, 1501, float('nan'), '80'):
            with self.subTest(duration=duration), self.assertRaises(ValueError):
                preview.input_command({'type': 'key', 'key': 'BACK', 'duration': duration})

    def test_touch_protocol_bounds(self):
        data = dict(type='touch', x1=0, y1=0.5, x2=1, y2=1, duration=650)
        self.assertEqual(preview.input_command(data), b'T 0.000000 0.500000 1.000000 1.000000 650')
        for value in (None, True, -0.1, 1.1, float('nan'), float('inf'), '0.5'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                preview.input_command(dict(data, x1=value))
        with self.assertRaises(ValueError):
            preview.input_command(dict(data, duration=651))

    def test_unknown_commands_cannot_execute(self):
        for data in ([], None, {}, {'type': 'shell', 'cmd': 'anything'}, {'type': 'key', 'key': 'QUIT'}):
            with self.assertRaises(ValueError):
                preview.input_command(data)

    def test_demo_relaunch_preserves_user_settings_and_books(self):
        with tempfile.TemporaryDirectory() as directory:
            sd = Path(directory) / 'sd'
            preview.seed_demo(sd)
            settings = sd / '.crosspoint/settings.json'
            settings.write_text('{"rickyNickname":"测试用户"}')
            book = sd / 'books/阅读与生活.txt'
            book.write_text('自定义测试书籍')
            preview.seed_demo(sd)
            self.assertEqual(settings.read_text(), '{"rickyNickname":"测试用户"}')
            self.assertEqual(book.read_text(), '自定义测试书籍')

    def test_demo_card_cannot_be_shared_by_concurrent_processes(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(preview.Preview, 'compile_bridge'), \
                patch.object(preview.Preview, 'start'):
            first = preview.Preview(Path(directory), 8765)
            try:
                with self.assertRaisesRegex(RuntimeError, '占用'):
                    preview.Preview(Path(directory), 8766)
            finally:
                first.close()
            second = preview.Preview(Path(directory), 8766)
            second.close()


class FakePreview:
    token = 'test-only-token'
    busy = False
    def __init__(self):
        self.events = []
        raw = struct.pack('<4sIII', b'RKB1', 1, 1, 1) + b'\x80'
        png, w, h, _ = preview.frame_to_png(raw)
        self.frame = (png, w, h, hashlib.sha256(raw).hexdigest())
    def get_frame(self):
        return self.frame
    def status(self):
        return {'product': 'RickyOS-browser-preview', 'alive': True}
    def input(self, data):
        self.events.append(preview.input_command(data))
    def start(self):
        self.events.append('restart')
    def rebuild(self):
        self.events.append('rebuild')


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.fake = FakePreview()
        self.server = preview.LocalServer(('127.0.0.1', 0), preview.handler_for(self.fake))
        self.fake.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.fake.port, timeout=3)
        connection.request(method, path, body, headers or {})
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result
    def authorized(self, **changes):
        return dict({'X-Ricky-Token': self.fake.token, 'Origin': f'http://127.0.0.1:{self.fake.port}'}, **changes)
    def test_static_page_contains_token_and_security_headers(self):
        status, headers, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn(self.fake.token.encode(), body)
        self.assertNotIn(b'__TOKEN__', body)
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
    def test_host_rebinding_and_path_traversal_blocked(self):
        self.assertEqual(self.request('GET', '/', headers={'Host': 'evil.invalid'})[0], 403)
        self.assertEqual(self.request('GET', '/../server.py')[0], 404)
        self.assertEqual(self.request('GET', '/api/input')[0], 404)
    def test_post_requires_token_and_same_origin(self):
        for headers in ({}, {'X-Ricky-Token': 'wrong'}, self.authorized(Origin='http://evil.invalid'),
                        self.authorized(Host='evil.invalid')):
            self.assertEqual(self.request('POST', '/api/restart', '{}', headers)[0], 403)
        self.assertEqual(self.fake.events, [])
    def test_valid_input_and_invalid_input(self):
        self.assertEqual(self.request('POST', '/api/input', '{"type":"key","key":"BACK"}', self.authorized())[0], 200)
        self.assertEqual(self.fake.events, [b'K BACK 100'])
        self.assertEqual(self.request('POST', '/api/input', '{"type":"key","key":"QUIT"}', self.authorized())[0], 400)
        self.assertEqual(self.request('POST', '/api/input', '[', self.authorized())[0], 400)
        self.assertEqual(self.request('POST', '/api/input', 'x' * 2049, self.authorized())[0], 413)
    def test_frame_etag_and_original_screenshot(self):
        status, headers, png = self.request('GET', '/api/frame')
        self.assertEqual(status, 200)
        self.assertEqual(headers['X-Frame-Width'], '1')
        self.assertEqual(self.request('GET', '/api/frame', headers={'If-None-Match': headers['ETag']})[0], 304)
        status, headers, body = self.request('GET', '/api/screenshot')
        self.assertEqual(status, 200)
        self.assertEqual(body, png)
        self.assertIn('attachment', headers['Content-Disposition'])
    def test_restart_and_rebuild_routes(self):
        for route in ('restart', 'rebuild'):
            self.assertEqual(self.request('POST', '/api/' + route, '{}', self.authorized())[0], 200)
        self.assertEqual(self.fake.events, ['restart', 'rebuild'])
        self.fake.busy = True
        self.assertEqual(self.request('POST', '/api/restart', '{}', self.authorized())[0], 503)
    def test_waiting_for_initial_frame(self):
        self.fake.frame = None
        self.assertEqual(self.request('GET', '/api/frame')[0], 503)


if __name__ == '__main__':
    unittest.main()

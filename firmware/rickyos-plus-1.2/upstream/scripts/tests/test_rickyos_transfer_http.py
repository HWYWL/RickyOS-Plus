"""Opt-in real simulator HTTP/WebSocket tests. Never targets a physical device."""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import unittest
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[2]
PORT = os.environ.get('RICKYOS_TRANSFER_TEST_HTTP_PORT')
SD = ROOT / '.cache/rickyos-transfer-qa/sd'
WORKSPACE = ROOT.parents[2] if ROOT.name == 'candidate' and ROOT.parent.parent.name == 'upgrade-review' else ROOT.parents[1]
FONT = WORKSPACE / 'artifacts/RickyOS-Fonts-0.1/fonts/RickySans/RickySans_18.cpfont'


@unittest.skipUnless(PORT, 'requires explicitly launched local simulator')
class SimulatorTransferTest(unittest.TestCase):
    def request(self, path, data=None, headers=None, method=None):
        request = urllib.request.Request('http://127.0.0.1:' + PORT + path,
                                         data=data, headers=headers or {}, method=method)
        # Ignore any host proxy configuration for this localhost-only fixture.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(request, timeout=25) as response:
                return response.status, response.read(), response.headers
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers

    def upload(self, path, fields, name, data):
        boundary = 'rickyos-local-test-boundary'
        body = bytearray()
        for key, value in fields.items():
            body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n'
                         f'{value}\r\n').encode())
        body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                     f'filename="{name}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode())
        body.extend(data)
        body.extend(f'\r\n--{boundary}--\r\n'.encode())
        return self.request(path, bytes(body), {'Content-Type': 'multipart/form-data; boundary=' + boundary})

    def test_pages_and_status_repeatedly(self):
        for _ in range(10):
            for path in ('/', '/files', '/fonts', '/settings', '/api/status', '/api/files?path=/books'):
                code, data, _ = self.request(path)
                self.assertEqual(code, 200, path)
                self.assertTrue(data, path)
        _, data, _ = self.request('/api/status')
        self.assertEqual(json.loads(data)['device'], 'read_pico')

    def test_font_large_upload_replace_and_invalid_preservation(self):
        data = FONT.read_bytes()
        target = SD / '.fonts/TransferQA/RickySans_18.cpfont'
        expected = hashlib.sha256(data).hexdigest()
        for _ in range(2):
            code, response, _ = self.upload('/api/fonts/upload', {'family': 'TransferQA'}, FONT.name, data)
            self.assertEqual(code, 200, response)
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), expected)
        for bad in (b'', b'bad', b'not-a-font' * 600):
            code, _, _ = self.upload('/api/fonts/upload', {'family': 'TransferQA'}, FONT.name, bad)
            self.assertEqual(code, 400)
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), expected)
        code, _, _ = self.upload('/api/fonts/upload', {'family': 'TransferQA'}, '../escape.cpfont', data[:32])
        self.assertEqual(code, 400)
        self.assertFalse(list(target.parent.glob('*.upload-*')))
        code, response, _ = self.request('/api/fonts')
        self.assertEqual(code, 200)
        self.assertIn('TransferQA', response.decode())

    def test_http_file_upload_and_download(self):
        data = ('RickyOS 文件传输回归测试\n' * 25000).encode()
        name = f'http-transfer-{uuid.uuid4().hex[:8]}.txt'
        code, response, _ = self.upload('/upload', {'path': '/books'}, name, data)
        self.assertEqual(code, 200, response)
        code, downloaded, _ = self.request('/download?path=/books/' + name)
        self.assertEqual(code, 200)
        self.assertEqual(hashlib.sha256(downloaded).digest(), hashlib.sha256(data).digest())

    def test_websocket_file_upload(self):
        with socket.create_connection(('127.0.0.1', int(PORT) + 1), timeout=15) as connection:
            connection.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{int(PORT)+1}\r\n'
                                'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                                'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
                                'Sec-WebSocket-Version: 13\r\n\r\n').encode())
            handshake = b''
            while not handshake.endswith(b'\r\n\r\n'):
                part = connection.recv(1)
                self.assertTrue(part)
                handshake += part
            self.assertIn(b'101', handshake.split(b'\r\n')[0])

            def send(opcode, payload):
                length = len(payload)
                prefix = bytes((0x80 | opcode, 0x80 | (length if length < 126 else 126)))
                if length >= 126:
                    prefix += struct.pack('!H', length)
                mask = b'QA12'
                connection.sendall(prefix + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

            def exact(length):
                result = bytearray()
                while len(result) < length:
                    part = connection.recv(length - len(result))
                    self.assertTrue(part)
                    result.extend(part)
                return bytes(result)

            def receive():
                header = exact(2)
                length = header[1] & 0x7f
                if length == 126:
                    length = struct.unpack('!H', exact(2))[0]
                elif length == 127:
                    length = struct.unpack('!Q', exact(8))[0]
                self.assertEqual(header[1] & 0x80, 0)
                return exact(length).decode()

            data = b'RickyOS-WebSocket-check\n' * 7000
            name = f'ws-transfer-{uuid.uuid4().hex[:8]}.txt'
            send(1, f'START:{name}:{len(data)}:/books'.encode())
            self.assertEqual(receive(), 'READY')
            for offset in range(0, len(data), 4096):
                send(2, data[offset:offset+4096])
            for _ in range(10):
                message = receive()
                self.assertFalse(message.startswith('ERROR:'), message)
                if message == 'DONE':
                    break
            else:
                self.fail('No completion message')
            self.assertEqual((SD / 'books' / name).read_bytes(), data)
            send(8, b'')


if __name__ == '__main__':
    unittest.main()

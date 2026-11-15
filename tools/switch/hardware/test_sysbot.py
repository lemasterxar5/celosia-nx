import socket
import threading
import unittest
import contextlib
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import MagicMock, patch
from sysbot import SysBot, ProtocolError, validate_endpoint, duration, main


class Peer:
    def __init__(self, handler):
        self.server = socket.socket()
        self.server.bind(('127.0.0.1', 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.errors = []
        def run():
            try:
                with self.server:
                    connection, _ = self.server.accept()
                    with connection:
                        connection.settimeout(2)
                        handler(connection)
            except BaseException as error:
                self.errors.append(error)
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def finish(self):
        self.thread.join(3)
        if self.thread.is_alive():
            raise AssertionError('Test peer did not stop')
        if self.errors:
            raise self.errors[0]


class SysBotTests(unittest.TestCase):
    def test_fragmented_status(self):
        def handler(connection):
            stream = connection.makefile('rb')
            self.assertEqual(stream.readline(), b'getVersion\r\n')
            connection.sendall(b'2.')
            connection.sendall(b'5\n')
            self.assertEqual(stream.readline(), b'getTitleID\r\n')
            connection.sendall(b'010000000000100D\r\n')
        peer = Peer(handler)
        with SysBot('127.0.0.1', peer.port) as client:
            self.assertEqual(client.status(), {'version': '2.5', 'title_id': '010000000000100D'})
        peer.finish()

    def test_hex_capture(self):
        jpeg = bytes.fromhex('FFD8FFE00004ABCDFFD9')
        def handler(connection):
            self.assertEqual(connection.makefile('rb').readline(), b'pixelPeek\r\n')
            for part in (jpeg.hex()[:7], jpeg.hex()[7:], '\n'):
                connection.sendall(part.encode())
        peer = Peer(handler)
        with SysBot('127.0.0.1', peer.port) as client:
            self.assertEqual(client.screenshot(), jpeg)
        peer.finish()

    def test_invalid_capture(self):
        for response in (b'\n', b'nothex\n', b'000102\n'):
            with self.subTest(response=response):
                def handler(connection):
                    connection.recv(128)
                    connection.sendall(response)
                peer = Peer(handler)
                with SysBot('127.0.0.1', peer.port) as client:
                    with self.assertRaises(ProtocolError):
                        client.screenshot()
                peer.finish()

    def test_controls_cleanup(self):
        expected = [b'press A\r\n', b'release A\r\n', b'getVersion\r\n',
                    b'setStick LEFT 0 20000\r\n', b'setStick LEFT 0 0\r\n', b'getVersion\r\n']
        def handler(connection):
            stream = connection.makefile('rb')
            for command in expected:
                self.assertEqual(stream.readline(), command)
                if command == b'getVersion\r\n':
                    connection.sendall(b'2.5\n')
        peer = Peer(handler)
        with SysBot('127.0.0.1', peer.port) as client:
            client.hold('A', 0)
            client.stick('LEFT', 0, 20000, 0)
        peer.finish()

    def test_invalid_endpoint(self):
        for host, port in [('bad\r\n', 6000), ('1.2.3.4', 0), ('1.2.3.4', 65536)]:
            with self.assertRaises(ValueError):
                validate_endpoint(host, port)

    def test_closed_response(self):
        peer = Peer(lambda connection: connection.recv(128))
        with SysBot('127.0.0.1', peer.port) as client:
            with self.assertRaises(ProtocolError):
                client.status()
        peer.finish()

    def test_oversized_line(self):
        def handler(connection):
            connection.recv(128)
            connection.sendall(b'X' * 1024 + b'\n')
        peer = Peer(handler)
        with SysBot('127.0.0.1', peer.port) as client:
            with self.assertRaises(ProtocolError):
                client.status()
        peer.finish()

    def test_invalid_controls(self):
        client = SysBot('127.0.0.1')
        for value in (-1, 31, float('nan')):
            with self.assertRaises(ValueError):
                duration(value)
        with self.assertRaises(ValueError):
            client.hold('A\r\nHOME', 0)
        with self.assertRaises(ValueError):
            client.stick('INVALID', 0, 0, 0)
        with self.assertRaises(ValueError):
            client.stick('LEFT', 32768, 0, 0)
        with self.assertRaises(ProtocolError):
            client.click('A')
        with self.assertRaises(ValueError):
            SysBot('127.0.0.1', timeout=0)

    def test_release_on_interruption(self):
        def handler(connection):
            stream = connection.makefile('rb')
            self.assertEqual(stream.readline(), b'press A\r\n')
            self.assertEqual(stream.readline(), b'release A\r\n')
            self.assertEqual(stream.readline(), b'getVersion\r\n')
            connection.sendall(b'2.5\n')
        peer = Peer(handler)
        with SysBot('127.0.0.1', peer.port) as client:
            with patch('sysbot.time.sleep', side_effect=KeyboardInterrupt):
                with self.assertRaises(KeyboardInterrupt):
                    client.hold('A', 1)
        peer.finish()

    def test_invalid_status_response(self):
        for response in (b'echo getVersion\n', b'2.5\nwrong-title\n'):
            def handler(connection):
                stream = connection.makefile('rb')
                stream.readline()
                connection.sendall(response)
                if response.startswith(b'2.5'):
                    stream.readline()
            peer = Peer(handler)
            with SysBot('127.0.0.1', peer.port) as client:
                with self.assertRaises(ProtocolError):
                    client.status()
            peer.finish()

    def test_cli_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'sysbot.json'
            base = ['sysbot', '--config', str(config)]
            with contextlib.redirect_stdout(io.StringIO()):
                with patch('sys.argv', base + ['status']):
                    self.assertEqual(main(), 1)
                with patch('sys.argv', base + ['--host', '127.0.0.1', 'configure']):
                    self.assertEqual(main(), 0)
                self.assertEqual(json.loads(config.read_text()), {'host': '127.0.0.1', 'port': 6000})
                config.write_text('[]')
                with patch('sys.argv', base + ['status']):
                    self.assertEqual(main(), 1)

    def test_cli_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            base = ['sysbot', '--config', str(Path(directory) / 'config.json'), '--host', '127.0.0.1']
            client = MagicMock()
            client.status.return_value = {'version': '2.5', 'title_id': None}
            client.screenshot.return_value = b'\xff\xd8\xff\xd9'
            context = MagicMock()
            context.__enter__.return_value = client
            output = Path(directory) / 'capture.jpg'
            actions = [['status'], ['screenshot', '--output', str(output)], ['click', 'A'],
                       ['hold', 'PLUS', '0.2'], ['stick', 'LEFT', '0', '20000', '1']]
            with patch('sysbot.SysBot', return_value=context), contextlib.redirect_stdout(io.StringIO()):
                for action in actions:
                    with patch('sys.argv', base + action):
                        self.assertEqual(main(), 0)
            self.assertEqual(output.read_bytes(), b'\xff\xd8\xff\xd9')
            client.click.assert_called_once_with('A')
            client.hold.assert_called_once_with('PLUS', 0.2)
            client.stick.assert_called_once_with('LEFT', 0, 20000, 1.0)


if __name__ == '__main__':
    unittest.main()

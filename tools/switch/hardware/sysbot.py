"""PC client for official sys-botbase TCP/CRLF protocol; standard library only."""
import argparse
from datetime import datetime
import ipaddress
import json
from pathlib import Path
import re
import socket
import time


class ProtocolError(RuntimeError):
    pass


BUTTONS = frozenset('A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK HOME CAPTURE'.split())


def validate_endpoint(host, port):
    ipaddress.IPv4Address(host)
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        raise ValueError('El puerto debe estar entre 1 y 65535')


def duration(value):
    value = float(value)
    if not 0 <= value <= 30:
        raise ValueError('La duracion debe estar entre 0 y 30 segundos')
    return value


class SysBot:
    def __init__(self, host, port=6000, timeout=8):
        validate_endpoint(host, port)
        if not 0 < timeout <= 60:
            raise ValueError('Tiempo de espera invalido')
        self.host, self.port, self.timeout = host, port, timeout
        self.socket = None
        self.buffer = bytearray()

    def __enter__(self):
        self.socket = socket.create_connection((self.host, self.port), self.timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return self

    def __exit__(self, *_):
        if self.socket:
            self.socket.close()
            self.socket = None

    def _send(self, command):
        if self.socket is None:
            raise ProtocolError('No hay conexion')
        self.socket.settimeout(self.timeout)
        self.socket.sendall((command + '\r\n').encode('ascii'))

    def _query(self, command, limit=256):
        self._send(command)
        deadline = time.monotonic() + self.timeout
        while True:
            end = self.buffer.find(b'\n')
            if end >= 0:
                if end > limit:
                    raise ProtocolError('Respuesta demasiado grande')
                result = bytes(self.buffer[:end]).rstrip(b'\r')
                del self.buffer[:end + 1]
                return result
            if len(self.buffer) > limit:
                raise ProtocolError('Respuesta demasiado grande')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('sys-botbase no completo la respuesta')
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(min(65536, limit + 1 - len(self.buffer)))
            if not chunk:
                raise ProtocolError('La Switch cerro la conexion antes de completar la respuesta')
            self.buffer.extend(chunk)

    def version(self):
        value = self._query('getVersion').decode('ascii')
        if not re.fullmatch(r'\d+(?:\.\d+){0,3}(?:[-+][A-Za-z0-9._-]+)?', value):
            raise ProtocolError('Respuesta de version no reconocida: ' + repr(value))
        return value

    def status(self):
        version = self.version()
        title = self._query('getTitleID').decode('ascii')
        if title and not re.fullmatch('[0-9A-Fa-f]{16}', title):
            raise ProtocolError('Identificador de aplicacion no valido')
        return {'version': version, 'title_id': title.upper() or None}

    def screenshot(self):
        response = self._query('pixelPeek', 2 * 1024 * 1024)
        try:
            image = bytes.fromhex(response.decode('ascii'))
        except (ValueError, UnicodeError) as error:
            raise ProtocolError('pixelPeek no devolvio hexadecimal valido') from error
        if not image.startswith(b'\xff\xd8') or not image.endswith(b'\xff\xd9'):
            raise ProtocolError('La captura no es JPEG; comprueba pixelPeek en la Switch')
        return image

    def click(self, button):
        button = self._button(button)
        self._send('click ' + button)
        self.version()  # ordered protocol barrier; not proof of in-game response

    @staticmethod
    def _button(button):
        button = button.upper()
        if button not in BUTTONS:
            raise ValueError('Boton no reconocido')
        return button

    def hold(self, button, seconds):
        button, seconds = self._button(button), duration(seconds)
        self._send('press ' + button)
        try:
            time.sleep(seconds)
        finally:
            self._send('release ' + button)
            self.version()

    def stick(self, side, x, y, seconds):
        side, seconds = side.upper(), duration(seconds)
        if side not in ('LEFT', 'RIGHT') or not all(isinstance(n, int) and -32768 <= n <= 32767 for n in (x, y)):
            raise ValueError('Stick o coordenadas no validos')
        self._send(f'setStick {side} {x} {y}')
        try:
            time.sleep(seconds)
        finally:
            self._send(f'setStick {side} 0 0')
            self.version()


def main():
    parser = argparse.ArgumentParser(description='Cliente PC de sys-botbase para pruebas de Celosia')
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('sysbot.json'))
    parser.add_argument('--host', help='IP local IPv4 de la Switch')
    parser.add_argument('--port', type=int)
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('configure')
    commands.add_parser('status')
    shot = commands.add_parser('screenshot')
    shot.add_argument('--output', type=Path)
    click = commands.add_parser('click')
    click.add_argument('button')
    hold = commands.add_parser('hold')
    hold.add_argument('button')
    hold.add_argument('seconds', type=float)
    stick = commands.add_parser('stick')
    stick.add_argument('side', choices=('LEFT', 'RIGHT'))
    stick.add_argument('x', type=int)
    stick.add_argument('y', type=int)
    stick.add_argument('seconds', type=float)
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text(encoding='utf-8-sig')) if args.config.exists() else {}
        if not isinstance(config, dict):
            raise ValueError('El archivo de configuracion debe ser un objeto JSON')
        host, port = args.host or config.get('host'), args.port if args.port is not None else config.get('port', 6000)
        if not host:
            raise ValueError('Falta la IP de la Switch: usa --host IP configure')
        validate_endpoint(host, port)
        if args.action == 'configure':
            args.config.parent.mkdir(parents=True, exist_ok=True)
            args.config.write_text(json.dumps({'host': host, 'port': port}, indent=2) + '\n', encoding='utf-8')
            print('Configuracion guardada; falta comprobar la conexion con status')
            return 0
        with SysBot(host, port) as client:
            if args.action == 'status':
                print(json.dumps(client.status(), indent=2))
            elif args.action == 'screenshot':
                image = client.screenshot()
                output = args.output or Path(__file__).parent / 'capturas' / (datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.jpg')
                output.parent.mkdir(parents=True, exist_ok=True)
                with output.open('xb') as file:
                    file.write(image)
                print(output.resolve())
            elif args.action == 'click':
                client.click(args.button)
            elif args.action == 'hold':
                client.hold(args.button, args.seconds)
            elif args.action == 'stick':
                client.stick(args.side, args.x, args.y, args.seconds)
        return 0
    except (OSError, ValueError, ProtocolError, UnicodeError, TypeError) as error:
        print('ERROR: ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

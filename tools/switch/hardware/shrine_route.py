"""Reproducible cold shrine GAMEPLAY benchmark (calibrated on hardware 2026-11-15 with v47).

From HOME with the Celosia forwarder selected:
  A (launch), 30 s bird intro; PLUS -> title menu, 6 s; B (Novo Jogo), 4 s; PLUS skips the
  opening cinematic -> Wander controllable at the altar, 6 s. About 15-20 s later the scripted
  Agro arrival cinematic starts; wait 35 s in total and PLUS skips it (Wander back at the altar,
  free control from then on). Screenshots before/after that skip must be inspected.
Phase 'static': left stick back 3 s (standard view: Wander in front of the altar), settle 5 s,
  then a read-only FTP benchmark.
Phase 'move': deterministic loop of left-stick walking (rectangle inside the shrine) and
  right-stick camera turns running while a second benchmark reads status.
Never writes clocks or saves. Inputs are released in every exit path.
"""
import argparse
import ftplib
import hashlib
import json
from pathlib import Path
import threading
import time

import benchmark
from sysbot import SysBot

NRO_PATH = '/switch/celosia-native-boot/celosia_switch_game.nro'
CLOCKS_PATH = '/switch/celosia-native-boot/hardware-clocks.txt'
# Route is adaptive: see adaptive_route().
AGRO_SKIP_WAIT = 6
STEP_BACK = ('LEFT', 0, -25000, 3.0)  # kept in route.json for older runs; unused
FIRST_CONTROL_SETTLE = 8
SETTLE_SECONDS = 5
# (kind, x, y, seconds): 'run' holds the left stick (full deflection) and taps X (jump in this
# port's mapping, proven on hardware in v41) every JUMP_PERIOD seconds; 'cam' turns the right stick.
JUMP_PERIOD = 1.0
MOVE_LOOP = (('run', 0, -32000, 4.0), ('run', -32000, 0, 3.0), ('cam', 25000, 0, 2.0),
             ('run', 0, 32000, 4.0), ('run', 32000, 0, 3.0), ('cam', -25000, 0, 2.0))
GPU_CEILING_HZ = 387_000_000


def ftp_read(host, path, sink):
    with ftplib.FTP() as ftp:
        ftp.connect(host, 5000, timeout=15)
        ftp.login()
        ftp.retrbinary('RETR '+path, sink, 1 << 18)


def remote_sha256(host):
    digest = hashlib.sha256()
    ftp_read(host, NRO_PATH, digest.update)
    return digest.hexdigest().upper()


def read_clocks(host):
    data = bytearray()
    ftp_read(host, CLOCKS_PATH, data.extend)
    clocks = dict(line.split('=', 1) for line in data.decode('ascii').splitlines() if '=' in line)
    if int(clocks.get('GPU_hz', '0')) > GPU_CEILING_HZ:
        raise ValueError('GPU clock above 387 MHz ceiling: '+clocks['GPU_hz'])
    return clocks


# Directory for scene-check captures that are not 'gameplay' (set by main); None keeps nothing.
CHECK_DIR = None


def scene_check(bot, classifier, phase):
    """One scene check; a capture classified as anything but gameplay is kept for review."""
    image = bot.screenshot()
    result = (phase, round(time.monotonic(), 3))+classifier.classify(image)
    if CHECK_DIR is not None and result[2] != 'gameplay':
        (CHECK_DIR/f'check-{phase}-{result[1]:.0f}-{result[2]}.jpg').write_bytes(image)
    return result


def release_all(host, port):
    with SysBot(host, port) as bot:
        for button in ('A', 'B', 'PLUS', 'X', 'HOME'):
            bot._send('release '+button)
        bot._send('setStick LEFT 0 0')
        bot._send('setStick RIGHT 0 0')
        bot.version()


def move_loop(host, port, stop, log, classifier=None, shots=None):
    with SysBot(host, port) as bot:
        while not stop.is_set():
            if classifier is not None:
                shots.append(scene_check(bot, classifier, 'move'))
            for kind, x, y, seconds in MOVE_LOOP:
                if stop.is_set():
                    break
                if bot.status()['title_id'] != benchmark.TITLE_ID:
                    # The game closed or crashed: never send movement to HOME.
                    log.append((round(time.monotonic(), 3), 'game-not-running'))
                    stop.set()
                    break
                jumps = 0
                if kind == 'cam':
                    bot.stick('RIGHT', x, y, seconds)
                else:
                    bot._send(f'setStick LEFT {x} {y}')
                    try:
                        end = time.monotonic()+seconds
                        while time.monotonic() < end and not stop.is_set():
                            bot.click('X')
                            jumps += 1
                            time.sleep(min(JUMP_PERIOD, max(0, end-time.monotonic())))
                    finally:
                        bot._send('setStick LEFT 0 0')
                        bot.version()
                log.append((round(time.monotonic(), 3), kind, x, y, seconds, jumps))


class SceneClassifier:
    """Coarse scene check from a pixelPeek JPEG (thresholds calibrated on v47 captures).

    menu: downscaled grey image within 25 of the private title-menu reference;
    cinematic: letterbox, i.e. top band (rows 8..92) almost uniformly black;
    gameplay: textured top band and far from the menu; anything else is 'other'.
    """
    def __init__(self, menu_reference):
        from PIL import Image, ImageChops, ImageStat
        self.Image, self.ImageChops, self.ImageStat = Image, ImageChops, ImageStat
        self.menu = self._small(Image.open(menu_reference))
        # The menu's option text alone (v89: the sky / bloom settings and the FPS counter change the
        # rest of the picture): 0-1 on the title menu, >= 55 elsewhere in the v88/v89 captures.
        self.menuText = self._text(Image.open(menu_reference))

    def _text(self, image):
        return image.convert('L').crop((30, 530, 330, 640)).resize((60, 22))

    def _small(self, image):
        return image.convert('L').crop((0, 0, 1280, 660)).resize((64, 33))

    def classify(self, jpeg):
        import io
        image = self.Image.open(io.BytesIO(jpeg))
        top = self.ImageStat.Stat(image.convert('L').crop((0, 8, 1280, 92)))
        diff = self.ImageStat.Stat(self.ImageChops.difference(self._small(image), self.menu)).mean[0]
        mean, sd = top.mean[0], top.stddev[0]
        textDiff = self.ImageStat.Stat(self.ImageChops.difference(self._text(image), self.menuText)).mean[0]
        if diff < 25 or textDiff < 15:
            kind = 'menu'
        elif mean < 3 and sd < 2:
            kind = 'cinematic'
        elif sd > 10:  # diff >= 25 here; gameplay floor views reached 32 (v49)
            kind = 'gameplay'
        else:
            kind = 'other'
        return kind, round(mean, 1), round(sd, 1), round(diff, 1)


class GameNotRunning(RuntimeError):
    """The forwarder is not the running title: stop sending inputs (they would reach HOME)."""


def require_game(bot, label):
    title = bot.status()['title_id']
    if title != benchmark.TITLE_ID:
        raise GameNotRunning(f'{label}: game not running (title {title}); inputs stopped')


def wait_for(bot, classifier, kinds, timeout, log, label, poll=2.0, action=None, action_after=None):
    """Poll screenshots until one of kinds appears; optional action pressed every action_after s.
    Every poll first checks that the game is still the running title, so no button is ever
    pressed on HOME after a failed launch or a crash."""
    start = last_action = time.monotonic()
    while time.monotonic()-start < timeout:
        require_game(bot, label)
        result = classifier.classify(bot.screenshot())
        log.append((label, round(time.monotonic()-start, 1))+result)
        if result[0] in kinds:
            return result[0]
        if action and time.monotonic()-last_action >= action_after:
            bot.click(action)
            log.append((label, 'press', action))
            last_action = time.monotonic()
        time.sleep(poll)
    raise TimeoutError(f'{label}: no {kinds} within {timeout}s')


def adaptive_route(bot, classifier, out_dir, log):
    """Launch -> title menu -> new game -> skip opening -> skip Agro arrival -> free gameplay."""
    bot.click('A')
    # Fail fast: if the forwarder did not start, nothing else is pressed.
    deadline = time.monotonic()+15
    while bot.status()['title_id'] != benchmark.TITLE_ID:
        if time.monotonic() > deadline:
            raise GameNotRunning('launch: forwarder did not start; no further inputs sent')
        time.sleep(1)
    log.append(('launch', 'running'))
    time.sleep(20)
    require_game(bot, 'boot')
    # Bird intro: PLUS (every 6 s) until the title menu is recognised.
    wait_for(bot, classifier, ('menu',), 90, log, 'to-menu', action='PLUS', action_after=6)
    time.sleep(2)
    bot.click('B')
    log.append(('menu', 'press', 'B'))
    wait_for(bot, classifier, ('cinematic', 'gameplay', 'other'), 20, log, 'leave-menu', action='B', action_after=5)
    time.sleep(3)
    bot.click('PLUS')
    log.append(('opening', 'press', 'PLUS'))
    wait_for(bot, classifier, ('gameplay',), 40, log, 'opening-skip', action='PLUS', action_after=8)
    (out_dir/'altar-first-control.jpg').write_bytes(bot.screenshot())
    # Deterministic start (v48/v49 lesson): inputs right after first control were sometimes
    # ignored (subtitle still up), so stepping away put Wander in different places per run.
    # Wait a fixed FIRST_CONTROL_SETTLE with no input; the static phase is measured at this
    # initial altar view. The Agro arrival cinematic is only triggered by moving away; if it
    # starts during the move phase the scene checks invalidate the run.
    time.sleep(FIRST_CONTROL_SETTLE)
    log.append(('first-control', 'settled', FIRST_CONTROL_SETTLE))
    return

def scene_monitor(host, port, classifier, stop, shots, phase, period=10.0):
    with SysBot(host, port) as bot:
        while not stop.wait(period):
            shots.append(scene_check(bot, classifier, phase))


def run_benchmark(args, label, seconds):
    return benchmark.main(['--label', label, '--seconds', str(seconds),
                           '--output', str(args.out_dir/(label+'.json')), '--config', str(args.config),
                           '--skip-title-check'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--expect-sha256', required=True)
    parser.add_argument('--static-seconds', type=int, default=20)
    parser.add_argument('--move-seconds', type=int, default=30)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('sysbot.json'))
    parser.add_argument('--menu-reference', type=Path, required=True,
                        help='Private screenshot of the title menu (kept outside the public repo)')
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text(encoding='utf-8-sig'))
    host, port = config['host'], config.get('port', 6000)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    global CHECK_DIR
    CHECK_DIR = args.out_dir
    meta = dict(label=args.label, route='adaptive (see stages)', step_back=STEP_BACK, settle_seconds=SETTLE_SECONDS,
                move_loop=MOVE_LOOP, phases={})
    stop = threading.Event()
    mover = monitor = None
    shots = []
    try:
        meta['remote_sha256'] = remote_sha256(host)
        if meta['remote_sha256'] != args.expect_sha256.upper():
            raise ValueError('Installed NRO does not match expected build: '+meta['remote_sha256'])
        classifier = SceneClassifier(args.menu_reference)
        with SysBot(host, port) as bot:
            if bot.status()['title_id'] is not None:
                raise ValueError('An application is already running; close it from HOME first')
            meta['stages'] = stages = []
            adaptive_route(bot, classifier, args.out_dir, stages)
            meta['title_after_route'] = bot.status()['title_id']
            if meta['title_after_route'] != benchmark.TITLE_ID:
                raise ValueError('Forwarder not running after route: '+str(meta['title_after_route']))
            time.sleep(SETTLE_SECONDS)
            image = bot.screenshot()
            (args.out_dir/'static-before.jpg').write_bytes(image)
            if classifier.classify(image)[0] != 'gameplay':
                raise ValueError('Not in gameplay before static phase')
        meta['clocks_before'] = read_clocks(host)
        meta['scene_checks'] = shots
        monitor = threading.Thread(target=scene_monitor, args=(host, port, classifier, stop, shots, 'static'), daemon=True)
        monitor.start()
        meta['phases']['static'] = run_benchmark(args, args.label+'-static', args.static_seconds)
        stop.set(); monitor.join(timeout=15); stop.clear()
        moves = []
        meta['move_log'] = moves
        mover = threading.Thread(target=move_loop, args=(host, port, stop, moves, classifier, shots), daemon=True)
        monitor = None
        mover.start()
        meta['phases']['move'] = run_benchmark(args, args.label+'-move', args.move_seconds)
        stop.set()
        mover.join(timeout=15)
        with SysBot(host, port) as bot:
            image = bot.screenshot()
            (args.out_dir/'move-after.jpg').write_bytes(image)
            shots.append(('move-end', round(time.monotonic(), 3))+classifier.classify(image))
        meta['clocks_after'] = read_clocks(host)
        bad = [shot for shot in shots if shot[2] != 'gameplay']
        meta['scene_valid'] = not bad
        if bad:
            meta['phases']['scene_invalid'] = 2
    finally:
        stop.set()
        for thread in (mover, monitor):
            if thread is not None:
                thread.join(timeout=15)
        try:
            release_all(host, port)
            meta['inputs_released'] = True
        except Exception as error:
            meta['inputs_released'] = 'error: '+str(error)
        (args.out_dir/'route.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
    return max(meta['phases'].values(), default=1)


if __name__ == '__main__':
    raise SystemExit(main())

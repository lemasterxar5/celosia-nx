"""Download the .gcda profiles written by a CELOSIA_PGO=GENERATE build (read-only on the console).

The instrumented NRO dumps every 30 s into sdmc:/switch/celosia_nx/pgo (see
platform/switch/hybrid/pgo_dump.cpp). Run this after the profiling route, while or after the
app runs (wait at least one dump period after the last gameplay you want included).
"""
import argparse
import ftplib
import json
from pathlib import Path

REMOTE = '/switch/celosia_nx/pgo'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--host', default=None)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('sysbot.json'))
    args = parser.parse_args(argv)
    host = args.host or json.loads(args.config.read_text(encoding='utf-8-sig'))['host']
    args.output.mkdir(parents=True, exist_ok=True)
    files = {}
    with ftplib.FTP() as ftp:
        ftp.connect(host, 5000, timeout=30)
        ftp.login()
        lines = []
        ftp.dir(REMOTE, lines.append)
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) == 9 and parts[0].startswith('-') and parts[8].endswith('.gcda'):
                files[parts[8]] = int(parts[4])
        for name, size in sorted(files.items()):
            target = args.output/name
            with open(target, 'wb') as out:
                ftp.retrbinary('RETR '+REMOTE+'/'+name, out.write, 1 << 18)
            if target.stat().st_size != size:
                raise ValueError(f'Size mismatch for {name}: {target.stat().st_size} != {size}')
    total = sum(files.values())
    print(json.dumps(dict(files=len(files), bytes=total, output=str(args.output)), indent=2))
    if not files:
        raise SystemExit('No .gcda profiles found on the console')


if __name__ == '__main__':
    main()

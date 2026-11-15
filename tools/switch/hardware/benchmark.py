"""Read-only native presentation benchmark; FTP status windows, never input or clocks."""
import argparse
from datetime import datetime, timezone
import ftplib
import json
import math
from pathlib import Path
import statistics
import time
import traceback

from sysbot import SysBot, validate_endpoint

TITLE_ID = '055FDD199F883000'
STATUS_PATH = '/switch/celosia-native-boot/hardware-status.txt'


def parse_status(text):
    fields = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        key, separator, value = line.partition('=')
        if not separator or not key or key in fields:
            raise ValueError('Malformed or duplicate hardware status field')
        fields[key] = value
    record = {'raw': text}
    for key in ('native_width', 'native_height', 'presented_frames', 'interval_ns', 'sample_end_ns'):
        value = fields.get(key, '')
        if not value.isascii() or not value.isdigit():
            raise ValueError('Missing or invalid integer: '+key)
        record[key] = int(value)
    if (record['native_width'], record['native_height']) != (1280, 720):
        raise ValueError('Native raster must be 1280x720')
    frames, interval, end = (record[key] for key in ('presented_frames', 'interval_ns', 'sample_end_ns'))
    if not 4_000_000_000 <= interval <= 30_000_000_000 or not 0 < frames <= 3600 or not interval <= end < 2**64:
        raise ValueError('Invalid native presentation window range')
    fps = frames*1e9/interval
    reported = float(fields.get('present_fps', 'nan'))
    if not math.isfinite(reported) or not 0 < fps <= 120 or abs(reported-fps) > 0.001:
        raise ValueError('Reported FPS is invalid or inconsistent with native window')
    record.update(fps=fps, present_fps=reported)
    if 'last_gpu_ms' in fields:
        value = float(fields['last_gpu_ms'])
        if not math.isfinite(value) or not 0 <= value <= 30_000:
            raise ValueError('Invalid last GPU duration')
        record['last_gpu_ms'] = value
        timing_frame = fields.get('gpu_timing_frame', '0')
        if not timing_frame.isascii() or not timing_frame.isdigit():
            raise ValueError('Invalid GPU timing frame')
        record['gpu_timing_frame'] = int(timing_frame)
    return record


class WindowCollector:
    def __init__(self):
        self.records = []
        self.baseline = None
        self.last_end = None
        self.rejected = dict(baseline=0, duplicate=0, stale=0, overlap=0)

    def add(self, record):
        end = record['sample_end_ns']
        if self.last_end is None:
            self.baseline = record
            self.last_end = end
            self.rejected['baseline'] += 1
            return False
        reason = ('duplicate' if end == self.last_end else 'stale' if end < self.last_end
                  else 'overlap' if end-record['interval_ns'] < self.last_end else None)
        if reason:
            self.rejected[reason] += 1
            return False
        self.records.append(record)
        self.last_end = end
        return True


def summarize(records):
    frames = sum(record['presented_frames'] for record in records)
    interval = sum(record['interval_ns'] for record in records)
    windows = [record['fps'] for record in records]
    # A report exposes one delayed GPU frame, not all frames in its FPS window.
    gpu_by_frame = {record['gpu_timing_frame']: record['last_gpu_ms'] for record in records
                    if record.get('gpu_timing_frame', 0) > 0 and 'last_gpu_ms' in record}
    gpu = list(gpu_by_frame.values())
    return dict(native_width=1280, native_height=720, windows=len(records),
                presented_frames=frames, interval_ns=interval,
                weighted_fps=frames*1e9/interval if interval else None,
                window_fps=dict(min=min(windows), max=max(windows), median=statistics.median(windows)) if windows else None,
                window_description='Native presentation windows nominally five seconds; exact intervals retained',
                gpu_last_ms_samples=dict(count=len(gpu), arithmetic_mean_ms=statistics.mean(gpu) if gpu else None,
                    description='Arithmetic mean of unique last-GPU-frame samples; not a frame average over the FPS windows'))


def check_title(host, port):
    with SysBot(host, port) as client:
        result = client.status()
    if result['title_id'] != TITLE_ID:
        raise ValueError('Expected running title '+TITLE_ID+', observed '+str(result['title_id']))
    return result


def read_status(host):
    data = bytearray()
    def receive(chunk):
        if len(data)+len(chunk) > 65536:
            raise ValueError('Hardware status exceeds 64 KiB')
        data.extend(chunk)
    with ftplib.FTP() as ftp:
        ftp.connect(host, 5000, timeout=8)
        ftp.login()
        ftp.retrbinary('RETR '+STATUS_PATH, receive)
    return data.decode('ascii')


def persist(output, report, collector):
    report.update(summary=summarize(collector.records), records=collector.records,
                  baseline=collector.baseline, rejected=collector.rejected)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name+'.partial')
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    temporary.replace(output)


def seconds_argument(value):
    seconds = float(value)
    if not math.isfinite(seconds) or not 10 <= seconds <= 180:
        raise argparse.ArgumentTypeError('Duration must be 10..180 seconds')
    return seconds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('sysbot.json'))
    parser.add_argument('--seconds', type=seconds_argument, default=35)
    parser.add_argument('--label', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    collector = WindowCollector()
    report = dict(label=args.label, requested_seconds=args.seconds, expected_title_id=TITLE_ID,
                  started_utc=datetime.now(timezone.utc).isoformat(), status='collecting', errors=[])
    host = None
    result = 0
    try:
        config = json.loads(args.config.read_text(encoding='utf-8-sig'))
        if not isinstance(config, dict):
            raise ValueError('Config must be a JSON object')
        candidate, port = config.get('host'), config.get('port', 6000)
        validate_endpoint(candidate, port)
        validate_endpoint(candidate, 5000)
        host = candidate
        report['begin_title'] = check_title(host, port)
        deadline = time.monotonic()+args.seconds
        while time.monotonic() < deadline:
            if collector.add(parse_status(read_status(host))):
                persist(args.output, report, collector)
            remaining = deadline-time.monotonic()
            if remaining > 0:
                time.sleep(min(1.5, remaining))
        if not collector.records:
            raise ValueError('No fresh complete native presentation windows collected')
        report['status'] = 'complete'
    except KeyboardInterrupt:
        report['status'] = 'interrupted'
        result = 130
    except Exception as error:
        traceback.print_exc()
        report['errors'].append(dict(type=type(error).__name__, message=str(error)))
        report['status'] = 'error'
        result = 1
    finally:
        if host is not None:
            try:
                report['end_title'] = check_title(host, port)
            except Exception as error:
                traceback.print_exc()
                report['errors'].append(dict(phase='end_title', type=type(error).__name__, message=str(error)))
                report['status'] = 'error'
                result = 1
        report['ended_utc'] = datetime.now(timezone.utc).isoformat()
        persist(args.output, report, collector)
    print(json.dumps(dict(output=str(args.output), status=report['status'], summary=report['summary']), indent=2))
    return result


if __name__ == '__main__':
    raise SystemExit(main())

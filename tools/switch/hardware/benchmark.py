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

TITLE_ID = '05E1B33926CFF000'
STATUS_PATH = '/switch/celosia_nx/hardware-status.txt'


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
    if 'worst_frame_ms' in fields:
        samples = fields.get('frame_samples', '')
        if not samples.isascii() or not samples.isdigit():
            raise ValueError('Invalid per-frame sample count')
        record['frame_samples'] = int(samples)
        for key in ('worst_frame_ms', 'low1_frame_ms', 'low1_fps'):
            value = float(fields.get(key, 'nan'))
            if not math.isfinite(value) or not 0 <= value <= 30_000:
                raise ValueError('Invalid per-frame field: '+key)
            record[key] = value
        if record['frame_samples'] and record['low1_frame_ms'] > record['worst_frame_ms']+0.001:
            raise ValueError('1% low frame time cannot exceed the worst frame time')
    # Optional diagnostics (v48+): kept as numbers when present and finite; never required.
    for key in OPTIONAL_DIAGNOSTICS:
        try:
            value = float(fields[key])
        except (KeyError, ValueError):
            continue
        if math.isfinite(value):
            record[key] = value
    return record


OPTIONAL_DIAGNOSTICS = (
    'last_draws', 'last_early_draws', 'last_prepass_draws', 'last_prepass_backdrops', 'prepass_enabled', 'gpu_average_ms', 'gpu_peak_ms', 'last_opaque_draws', 'last_generic_draws',
    'last_shadow_draws', 'counter_avg_fragments', 'counter_avg_vertices', 'generic_backdrop_draws',
    'descriptor_set_binds', 'texture_handle_binds', 'descriptor_slots_in_use', 'descriptor_private_textures',
    'coverage_frame', 'coverage_opaque', 'coverage_generic', 'coverage_failure', 'coverage_backdrop',
    'coverage_shadow', 'coverage_fullscreen_draws', 'last_record_ms', 'last_begin_wait_ms', 'last_end_ms')


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
    per_frame = [record for record in records if record.get('frame_samples', 0) >= 2]
    lows = (dict(windows_with_samples=len(per_frame), frame_samples=sum(r['frame_samples'] for r in per_frame),
                 worst_frame_ms=max(r['worst_frame_ms'] for r in per_frame),
                 low1_fps_min=min(r['low1_fps'] for r in per_frame),
                 low1_frame_ms_max=max(r['low1_frame_ms'] for r in per_frame),
                 description='Worst presentation gap and slowest-1% mean over the windows that reported per-frame data; CPU submission cadence')
            if per_frame else None)
    return dict(native_width=1280, native_height=720, windows=len(records),
                presented_frames=frames, interval_ns=interval,
                weighted_fps=frames*1e9/interval if interval else None,
                window_fps=dict(min=min(windows), max=max(windows), median=statistics.median(windows)) if windows else None,
                window_description='Native presentation windows nominally five seconds; exact intervals retained',
                per_frame=lows, minimum_basis='per_frame' if lows else 'window_minimum_only',
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


READ_ATTEMPTS = 3
READ_BACKOFF_SECONDS = 0.15
RETRYABLE_READ_ERRORS = (ftplib.error_temp, ConnectionError, TimeoutError, EOFError)


def read_status_with_retry(host, events, attempts=READ_ATTEMPTS):
    """Bounded retry of a transient status read; each attempt opens a fresh connection.

    Transient failures (FTP 4xx while the file is being rewritten, resets, timeouts) are
    retried at most ``attempts`` times with a short growing pause. Permanent failures and
    anything else propagate immediately. Every failed attempt is appended to ``events`` so
    incomplete captures keep their record; a later success marks them ``recovered``.
    """
    pending = []
    for attempt in range(1, attempts+1):
        try:
            text = read_status(host)
        except RETRYABLE_READ_ERRORS as error:
            event = dict(attempt=attempt, type=type(error).__name__, message=str(error),
                         recovered=False, exhausted=attempt == attempts,
                         utc=datetime.now(timezone.utc).isoformat())
            events.append(event)
            pending.append(event)
            if attempt == attempts:
                raise
            time.sleep(READ_BACKOFF_SECONDS*2**(attempt-1))
            continue
        for event in pending:
            event['recovered'] = True
        return text


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
    parser.add_argument('--skip-title-check', action='store_true',
                        help='Caller already verifies the title over its own sys-botbase session (single-client server)')
    args = parser.parse_args(argv)
    collector = WindowCollector()
    report = dict(label=args.label, requested_seconds=args.seconds, expected_title_id=TITLE_ID,
                  started_utc=datetime.now(timezone.utc).isoformat(), status='collecting', errors=[],
                  read_retry_events=[])
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
        report['begin_title'] = 'skipped' if args.skip_title_check else check_title(host, port)
        deadline = time.monotonic()+args.seconds
        while time.monotonic() < deadline:
            if collector.add(parse_status(read_status_with_retry(host, report['read_retry_events']))):
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
        if host is not None and not args.skip_title_check:
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

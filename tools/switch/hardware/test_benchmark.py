import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import benchmark


def status(end=10_000_000_000, frames=50, interval=5_000_000_000, **changes):
    fields = dict(native_width=1280, native_height=720, present_fps=frames*1e9/interval,
                  presented_frames=frames, interval_ns=interval, sample_end_ns=end,
                  last_gpu_ms=12.3, gpu_timing_frame=42)
    fields.update(changes)
    return '\n'.join(f'{key}={value}' for key, value in fields.items())


class BenchmarkTests(unittest.TestCase):
    def test_valid_native_window_and_gpu_label(self):
        record = benchmark.parse_status(status())
        self.assertEqual(record['fps'], 10)
        result = benchmark.summarize([record])
        self.assertEqual(result['weighted_fps'], 10)
        self.assertEqual(result['gpu_last_ms_samples']['arithmetic_mean_ms'], 12.3)
        self.assertIn('not a frame average', result['gpu_last_ms_samples']['description'])

    def test_rejects_invalid_or_inconsistent_window(self):
        for changes in ({'native_width':640}, {'presented_frames':0}, {'interval_ns':0},
                        {'present_fps':'nan'}, {'present_fps':'inf'}, {'present_fps':11},
                        {'sample_end_ns':0}, {'last_gpu_ms':'nan'}, {'interval_ns':1},
                        {'presented_frames':'1.5'}, {'presented_frames':1000}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                benchmark.parse_status(status(**changes))
        with self.assertRaises(ValueError):
            benchmark.parse_status(status()+'\nnative_width=1280')
        with self.assertRaises(ValueError):
            benchmark.parse_status('native_width=1280')

    def test_weighted_not_average_of_fps(self):
        records = [benchmark.parse_status(status(frames=50)),
                   benchmark.parse_status(status(end=16_000_000_000, frames=120, interval=6_000_000_000))]
        summary = benchmark.summarize(records)
        self.assertAlmostEqual(summary['weighted_fps'], 170/11)
        self.assertEqual(summary['window_fps'], {'min':10, 'max':20, 'median':15})

    def test_unique_increasing_and_nonoverlapping(self):
        collector = benchmark.WindowCollector()
        self.assertFalse(collector.add(benchmark.parse_status(status())))
        self.assertFalse(collector.add(benchmark.parse_status(status())))
        self.assertFalse(collector.add(benchmark.parse_status(status(end=9_000_000_000))))
        self.assertFalse(collector.add(benchmark.parse_status(status(end=12_000_000_000))))
        self.assertTrue(collector.add(benchmark.parse_status(status(end=15_000_000_000))))
        self.assertEqual(len(collector.records), 1)
        self.assertEqual(collector.rejected['baseline'], 1)
        self.assertEqual(collector.rejected['duplicate'], 1)
        self.assertEqual(collector.rejected['stale'], 1)
        self.assertEqual(collector.rejected['overlap'], 1)

    def test_title_guard_is_readonly(self):
        with patch.object(benchmark, 'SysBot') as client:
            client.return_value.__enter__.return_value.status.return_value = {'title_id':benchmark.TITLE_ID}
            self.assertEqual(benchmark.check_title('127.0.0.1',6000)['title_id'], benchmark.TITLE_ID)
            client.return_value.__enter__.return_value.status.return_value = {'title_id':'0000000000000000'}
            with self.assertRaises(ValueError): benchmark.check_title('127.0.0.1',6000)

    def test_failure_persists_partial_json_and_checks_end_title(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)/'sysbot.json';config.write_text('{"host":"127.0.0.1"}')
            output = Path(directory)/'result.json'
            with patch.object(benchmark, 'check_title', return_value={'title_id':benchmark.TITLE_ID}) as title, \
                 patch.object(benchmark, 'read_status', side_effect=OSError('FTP unavailable')), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                result = benchmark.main(['--config',str(config),'--seconds','10','--label','test','--output',str(output)])
            report=json.loads(output.read_text())
            self.assertEqual(result,1);self.assertEqual(title.call_count,2)
            self.assertEqual(report['status'],'error')
            self.assertIn('FTP unavailable',report['errors'][0]['message'])

    def test_ctrl_c_persists_partial(self):
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'sysbot.json';config.write_text('{"host":"127.0.0.1"}')
            output=Path(directory)/'result.json'
            with patch.object(benchmark,'check_title',return_value={'title_id':benchmark.TITLE_ID}), \
                 patch.object(benchmark,'read_status',side_effect=KeyboardInterrupt), \
                 contextlib.redirect_stdout(io.StringIO()):
                result=benchmark.main(['--config',str(config),'--seconds','10','--label','test','--output',str(output)])
            self.assertEqual(result,130)
            self.assertEqual(json.loads(output.read_text())['status'],'interrupted')

    def test_complete_capture_ignores_baseline_and_duplicate(self):
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'sysbot.json';config.write_text('{"host":"127.0.0.1"}')
            output=Path(directory)/'result.json'
            with patch.object(benchmark,'check_title',return_value={'title_id':benchmark.TITLE_ID}) as title, \
                 patch.object(benchmark,'read_status',side_effect=[status(),status(end=15_000_000_000),status(end=15_000_000_000)]), \
                 patch.object(benchmark.time,'monotonic',side_effect=[0,0,1,2,3,4,5,11]), \
                 patch.object(benchmark.time,'sleep'), contextlib.redirect_stdout(io.StringIO()):
                result=benchmark.main(['--config',str(config),'--seconds','10','--label','field','--output',str(output)])
            report=json.loads(output.read_text())
            self.assertEqual(result,0);self.assertEqual(title.call_count,2)
            self.assertEqual(report['status'],'complete')
            self.assertEqual(report['summary']['windows'],1)
            self.assertEqual(report['rejected']['duplicate'],1)
            self.assertEqual(report['summary']['weighted_fps'],10)

    def test_per_frame_minimums_are_optional_validated_and_summarised(self):
        legacy = benchmark.parse_status(status())
        self.assertNotIn('worst_frame_ms', legacy)
        self.assertEqual(benchmark.summarize([legacy])['minimum_basis'], 'window_minimum_only')
        self.assertIsNone(benchmark.summarize([legacy])['per_frame'])
        first = benchmark.parse_status(status(frame_samples=50, worst_frame_ms=180.5, low1_frame_ms=180.5, low1_fps=5.54))
        second = benchmark.parse_status(status(end=15_000_000_000, frame_samples=49, worst_frame_ms=90.25, low1_frame_ms=88.0, low1_fps=11.364))
        summary = benchmark.summarize([first, second])
        self.assertEqual(summary['minimum_basis'], 'per_frame')
        self.assertEqual(summary['per_frame']['windows_with_samples'], 2)
        self.assertEqual(summary['per_frame']['frame_samples'], 99)
        self.assertEqual(summary['per_frame']['worst_frame_ms'], 180.5)
        self.assertEqual(summary['per_frame']['low1_fps_min'], 5.54)
        self.assertEqual(summary['per_frame']['low1_frame_ms_max'], 180.5)
        for broken in (dict(frame_samples=50, worst_frame_ms='nan', low1_frame_ms=1, low1_fps=1),
                       dict(frame_samples='x', worst_frame_ms=10, low1_frame_ms=10, low1_fps=100),
                       dict(frame_samples=50, worst_frame_ms=10, low1_frame_ms=20, low1_fps=50),
                       dict(frame_samples=50, worst_frame_ms=10, low1_frame_ms=10)):
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                benchmark.parse_status(status(**broken))

    def test_ftp_only_retrieves_bounded_status(self):
        with patch.object(benchmark.ftplib,'FTP') as factory:
            ftp=factory.return_value.__enter__.return_value
            ftp.retrbinary.side_effect=lambda command, callback: callback(status().encode('ascii'))
            self.assertEqual(benchmark.parse_status(benchmark.read_status('127.0.0.1'))['fps'],10)
            ftp.connect.assert_called_once_with('127.0.0.1',5000,timeout=8)
            self.assertEqual(ftp.retrbinary.call_args.args[0],'RETR '+benchmark.STATUS_PATH)
            ftp.storbinary.assert_not_called()
            ftp.retrbinary.side_effect=lambda command, callback: callback(b'x'*65537)
            with self.assertRaises(ValueError):benchmark.read_status('127.0.0.1')

    def test_duration_limits(self):
        for value in ('9','181','nan','inf'):
            with self.subTest(value=value),self.assertRaises(benchmark.argparse.ArgumentTypeError):
                benchmark.seconds_argument(value)

    def test_temporary_ftp_read_retries_fresh_connection_and_records_recovery(self):
        events=[]
        with patch.object(benchmark.ftplib,'FTP') as factory, patch.object(benchmark.time,'sleep') as sleep:
            ftp=factory.return_value.__enter__.return_value
            attempts=0
            def transfer(command,callback):
                nonlocal attempts
                attempts+=1
                if attempts==1:
                    callback(b'partial discarded')
                    raise benchmark.ftplib.error_temp('450 failed to open file')
                callback(status().encode('ascii'))
            ftp.retrbinary.side_effect=transfer
            self.assertEqual(benchmark.read_status_with_retry('127.0.0.1',events),status())
            self.assertEqual(factory.call_count,2)
            sleep.assert_called_once_with(0.15)
        self.assertEqual(len(events),1)
        self.assertEqual(events[0]['attempt'],1)
        self.assertTrue(events[0]['recovered'])
        self.assertIn('450',events[0]['message'])

    def test_connection_reset_and_timeout_are_bounded_retryable(self):
        events=[]
        with patch.object(benchmark,'read_status',side_effect=[ConnectionResetError('reset'),TimeoutError('timeout'),status()]) as read, \
             patch.object(benchmark.time,'sleep') as sleep:
            self.assertEqual(benchmark.read_status_with_retry('127.0.0.1',events),status())
            self.assertEqual(read.call_count,3)
            self.assertEqual([call.args[0] for call in sleep.call_args_list],[0.15,0.3])
        self.assertEqual(len(events),2)
        self.assertTrue(all(event['recovered'] for event in events))

    def test_retry_exhaustion_and_permanent_failure_persist_events(self):
        for error,expected_attempts in ((benchmark.ftplib.error_temp('450 busy'),3),
                                        (benchmark.ftplib.error_perm('550 missing'),1)):
            with self.subTest(error=error),tempfile.TemporaryDirectory() as directory:
                config=Path(directory)/'sysbot.json';config.write_text('{"host":"127.0.0.1"}')
                output=Path(directory)/'result.json'
                with patch.object(benchmark,'check_title',return_value={'title_id':benchmark.TITLE_ID}), \
                     patch.object(benchmark,'read_status',side_effect=error) as read, \
                     patch.object(benchmark.time,'sleep'), contextlib.redirect_stdout(io.StringIO()), \
                     contextlib.redirect_stderr(io.StringIO()):
                    result=benchmark.main(['--config',str(config),'--seconds','10','--label','test','--output',str(output)])
                report=json.loads(output.read_text())
                self.assertEqual(result,1);self.assertEqual(report['status'],'error')
                self.assertEqual(read.call_count,expected_attempts)
                self.assertEqual(len(report['read_retry_events']),3 if expected_attempts==3 else 0)
                if expected_attempts==3:
                    self.assertTrue(report['read_retry_events'][-1]['exhausted'])
                    self.assertFalse(report['read_retry_events'][0]['recovered'])


if __name__ == '__main__': unittest.main()

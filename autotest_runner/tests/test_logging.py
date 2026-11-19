"""Offline regression tests for round summaries and command transcripts."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import autotest_runner as runner


class LoggingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'check.py').write_text('def run(context, config):\n    return None\n')
        self.config_path = self.root / 'config.json'

    def config(self, runtime=None):
        data = {
            'connection': {'host': 'unused', 'username': 'root', 'password': 'unused'},
            'runtime': {'max_rounds': 11, 'round_interval_seconds': 0, **(runtime or {})},
            'checks': [{'name': 'sample', 'script': 'check.py'}],
        }
        self.config_path.write_text(json.dumps(data))
        return runner.load_config(str(self.config_path))

    def test_default_interval(self):
        self.assertEqual(self.config()['runtime']['full_log_every_n_rounds'], 5)

    def test_interval_validation(self):
        for value in [0, -1, True, False, 1.5, '5', None]:
            with self.subTest(value=value), self.assertRaises(runner.ConfigError):
                self.config({'full_log_every_n_rounds': value})
        self.assertEqual(self.config({'full_log_every_n_rounds': 1})['runtime']['full_log_every_n_rounds'], 1)

    def run_rounds(self, fail_at=None, error=None, interval=5):
        app = runner.AutotestRunner(self.config({'full_log_every_n_rounds': interval}), str(self.config_path))
        calls = 0

        def check(context, config):
            nonlocal calls
            calls += 1
            context.logger.log(f'round-detail-{calls}')
            if calls == fail_at:
                if error:
                    raise error
                context.fail('intentional failure')
            return runner.CheckResult.ok()

        app.loaded_checks[0][1].run = check
        app.connect = lambda: setattr(app, 'client', object())
        app.close = lambda: None
        console = io.StringIO()
        with contextlib.redirect_stdout(console), patch('autotest_runner.time.sleep'):
            result = app.run()
        return result, console.getvalue(), Path(app.log_path).read_text()

    def test_first_round_and_every_five_rounds_verbose_and_all_rounds_in_file(self):
        result, console, disk = self.run_rounds()
        self.assertEqual(result, 0)
        for index in range(1, 12):
            self.assertEqual(console.count(f'Round {index} started'), 1)
            self.assertEqual(console.count(f'Round {index} passed'), 1)
            self.assertLess(console.index(f'Round {index} started'), console.index(f'Round {index} passed'))
            self.assertIn(f'round-detail-{index}', disk)
            if index in (1, 6, 11):
                self.assertIn(f'Round {index} started', console)
                self.assertIn(f'round-detail-{index}', console)
            else:
                self.assertNotIn(f'round-detail-{index}', console)

        self.assertEqual(console.count('Sleeping 0.0 seconds before next round'), 10)

    def test_interval_one_preserves_verbose_logging(self):
        result, console, _ = self.run_rounds(interval=1)
        self.assertEqual(result, 0)
        for index in range(1, 12):
            self.assertIn(f'round-detail-{index}', console)

    def test_failure_replays_only_failed_round_once(self):
        result, console, disk = self.run_rounds(fail_at=2)
        self.assertEqual(result, 1)
        self.assertEqual(console.count('round-detail-1'), 1)
        self.assertEqual(console.count('round-detail-2'), 1)
        self.assertEqual(disk.count('round-detail-2'), 1)
        self.assertEqual(console.count('Round 2 started'), 1)
        self.assertIn('AUTOTEST FAILURE', console)
        self.assertNotIn('Round 2 passed', console)

    def test_interrupt_replays_active_round(self):
        result, console, _ = self.run_rounds(fail_at=2, error=KeyboardInterrupt())
        self.assertEqual(result, 130)
        self.assertIn('round-detail-2', console)
        self.assertIn('Stopped by user', console)

    def test_runtime_config_error_replays_active_round(self):
        result, console, _ = self.run_rounds(fail_at=2, error=runner.ConfigError('bad check'))
        self.assertEqual(result, 2)
        self.assertIn('round-detail-2', console)
        self.assertIn('bad check', console)

    def test_unexpected_exception_replays_active_round(self):
        result, console, _ = self.run_rounds(fail_at=2, error=ValueError('plugin error'))
        self.assertEqual(result, 1)
        self.assertIn('round-detail-2', console)
        self.assertIn('ValueError', console)

    def test_unicode_replay_keeps_all_checks_without_rewriting_file(self):
        log = runner.Logger(str(self.root / 'unicode.log'))
        self.addCleanup(log.close)
        console = io.StringIO()
        with contextlib.redirect_stdout(console):
            log.log('前一轮已完成')
            log.begin_round(verbose=False)
            log.log('检查一通过：中文输出')
            log.log('检查二失败：诊断信息')
            log.end_round(replay=True)
        self.assertEqual(console.getvalue().count('检查一通过'), 1)
        self.assertEqual(console.getvalue().count('检查二失败'), 1)
        disk = Path(log.log_path).read_text()
        self.assertEqual(disk.count('检查一通过'), 1)
        self.assertEqual(disk.count('检查二失败'), 1)

    def test_timeout_and_eof_preserve_partial_command_output(self):
        for error_type in (TimeoutError, EOFError):
            with self.subTest(error=error_type.__name__):
                client = runner.RawTelnetClient.__new__(runner.RawTelnetClient)
                client.encoding = 'utf-8'
                client._display_buffer = bytearray()
                client.write_text = lambda text: None
                client.read_available_text = lambda seconds: ''

                def read(prompt, timeout):
                    client._display_buffer.extend('./app\r\n部分诊断\r\n'.encode())
                    raise error_type('connection failed')

                client.read_until_text = read
                log = runner.Logger(str(self.root / (error_type.__name__ + '.log')))
                context = runner.RunnerContext(client=client, connection=self.config()['connection'], logger=log, state={})
                console = io.StringIO()
                try:
                    with contextlib.redirect_stdout(console), self.assertRaises(error_type):
                        context.run_command('./app')
                    self.assertIn('部分诊断', console.getvalue())
                    self.assertIn('./app', console.getvalue())
                    self.assertEqual(client._display_buffer, bytearray())
                finally:
                    log.close()

    def test_command_logs_raw_but_returns_cleaned_output(self):
        class Client:
            def read_available_text(self, quiet_seconds):
                return ''

            def write_text(self, text):
                self.sent = text

            def read_until_text(self, prompt, timeout):
                return './app --test\r\nRESULT: PASS\r\n#'

        client = Client()
        log = runner.Logger(str(self.root / 'commands.log'))
        self.addCleanup(log.close)
        context = runner.RunnerContext(client=client, connection=self.config()['connection'], logger=log, state={})
        console = io.StringIO()
        with contextlib.redirect_stdout(console):
            result = context.run_command('./app --test')
        self.assertEqual(client.sent, './app --test\n')
        self.assertEqual(result['output'], 'RESULT: PASS')
        self.assertEqual(result['raw_output'], './app --test\r\nRESULT: PASS\r\n#')
        self.assertIn('./app --test', console.getvalue())
        self.assertIn('RESULT: PASS', console.getvalue())
        self.assertIn('#', console.getvalue())


if __name__ == '__main__':
    unittest.main()

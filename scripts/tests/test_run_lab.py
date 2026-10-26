"""Exercise fatal-worker reporting without building or provoking a kernel fault."""
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SupervisorTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.launcher = self.directory / 'run_lab.py'
        shutil.copyfile(ROOT / 'scripts/run_lab.py', self.launcher)
        self.worker = self.directory / 'lab.py'
        self.journal_calls = self.directory / 'calls'
        journal = self.directory / 'journalctl'
        journal.write_text(f'#!{sys.executable}\n' + '''
import os, sys
with open(os.environ['JOURNAL_CALLS'], 'a') as log:
    log.write(repr(sys.argv[1:]) + '\\n')
if os.environ.get('JOURNAL_DENIED'):
    print('Permission denied', file=sys.stderr)
    sys.exit(1)
if '--show-cursor' in sys.argv:
    if not os.environ.get('NO_CURSOR'):
        print('-- cursor: test-cursor')
else:
    print(os.environ.get('KERNEL_MESSAGES', ''), end='')
''')
        journal.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.directory),
                        JOURNAL_CALLS=str(self.journal_calls), KERNEL_MESSAGES='')
        for key in ('JOURNAL_DENIED', 'NO_CURSOR'):
            self.env.pop(key, None)

    def run_worker(self, source, **environment):
        self.worker.write_text(source)
        return subprocess.run([sys.executable, str(self.launcher), 'module-build'],
                              env=dict(self.env, **environment), capture_output=True,
                              text=True, timeout=10)

    def test_success_preserves_output_and_does_not_print_journal(self):
        result = self.run_worker('import sys\nprint("compiler output")\n'
                                 'print("compiler warning", file=sys.stderr)\n')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, 'compiler output\n')
        self.assertEqual(result.stderr, 'compiler warning\n')
        self.assertEqual(len(self.journal_calls.read_text().splitlines()), 1)

    def test_sigkill_reports_signal_and_entire_new_kernel_trace(self):
        trace = 'BUG: unable to handle page fault\nCall Trace:\n  vfs_statx\nend trace\n'
        result = self.run_worker('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n',
                                 KERNEL_MESSAGES=trace)
        self.assertEqual(result.returncode, 137)
        self.assertIn('module-build terminated by SIGKILL (signal 9)', result.stderr)
        self.assertIn(trace, result.stderr)
        self.assertIn('may be unrelated', result.stderr)
        self.assertIn('--after-cursor=test-cursor', self.journal_calls.read_text())
        self.assertNotIn('--since=', self.journal_calls.read_text())

    def test_regular_failure_keeps_diagnostics_and_kernel_messages(self):
        result = self.run_worker('import sys\nprint("bad source", file=sys.stderr)\n'
                                 'sys.exit(2)\n', KERNEL_MESSAGES='Out of memory: Killed process 42\n')
        self.assertEqual(result.returncode, 2)
        self.assertIn('bad source', result.stderr)
        self.assertIn('Out of memory:', result.stderr)
        self.assertNotIn('terminated by', result.stderr)

    def test_killed_build_subprocess_reports_its_signal(self):
        source = (
            'import sys\n'
            f'sys.path.insert(0, {str(ROOT / "scripts")!r})\n'
            'from mapple_dev import core as c\n'
            'try:\n'
            ' c.logged([sys.executable, "-c", '
            '"import os, signal; os.kill(os.getpid(), signal.SIGKILL)"], '
            f'{str(self.directory / "build.log")!r})\n'
            'except RuntimeError as error:\n'
            ' print(error, file=sys.stderr)\n'
            ' sys.exit(1)\n')
        result = self.run_worker(source)
        self.assertEqual(result.returncode, 1)
        self.assertIn('Build command terminated by SIGKILL (signal 9)', result.stderr)

    def test_captured_command_error_prints_both_output_streams(self):
        # Exercise the real entry point with the build operation replaced.
        source = (
            'import runpy, subprocess, sys\n'
            'from unittest.mock import patch\n'
            f'path = {str(ROOT / "scripts/lab.py")!r}\n'
            f'sys.path.insert(0, {str(ROOT / "scripts")!r})\n'
            'from mapple_dev import kernels, runs\n'
            'error = subprocess.CalledProcessError(1, ["compiler"], '
            'output=b"stdout diagnostic", stderr=b"stderr diagnostic")\n'
            'with patch.object(kernels, "selected", return_value={}), '
            'patch.object(runs, "build", side_effect=error):\n'
            ' runpy.run_path(path, run_name="__main__")\n')
        result = self.run_worker(source)
        self.assertEqual(result.returncode, 1)
        self.assertIn('stdout diagnostic', result.stderr)
        self.assertIn('stderr diagnostic', result.stderr)

    def test_no_kernel_evidence_does_not_invent_a_cause(self):
        result = self.run_worker('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n')
        self.assertEqual(result.returncode, 137)
        self.assertIn('termination cause is unknown', result.stderr)

    def test_permission_failure_still_reports_worker_status(self):
        result = self.run_worker('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n',
                                 JOURNAL_DENIED='1')
        self.assertEqual(result.returncode, 137)
        self.assertIn('SIGKILL', result.stderr)
        self.assertIn('Kernel diagnostics unavailable: Permission denied', result.stderr)

    def test_no_cursor_uses_invocation_time(self):
        result = self.run_worker('raise SystemExit(1)\n', NO_CURSOR='1')
        self.assertEqual(result.returncode, 1)
        self.assertIn('--since=', self.journal_calls.read_text())
        self.assertEqual(result.stderr, '')

    def test_missing_journalctl_does_not_mask_failure(self):
        (self.directory / 'journalctl').unlink()
        result = self.run_worker('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n')
        self.assertEqual(result.returncode, 137)
        self.assertIn('Kernel diagnostics unavailable:', result.stderr)

    def test_output_arrives_before_command_finishes(self):
        self.worker.write_text('print("validation phase")\ninput()\nprint("finished")\n')
        process = subprocess.Popen([sys.executable, str(self.launcher), 'module-build'],
                                   env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            self.assertTrue(select.select([process.stdout], [], [], 5)[0], 'Output was buffered')
            self.assertEqual(process.stdout.readline(), 'validation phase\n')
            self.assertIsNone(process.poll())
            output, error = process.communicate('\n', timeout=5)
            self.assertEqual(output, 'finished\n')
            self.assertEqual(error, '')
            self.assertEqual(process.returncode, 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_terminal_interrupt_reaches_worker_and_cleanup_completes(self):
        self.worker.write_text('import time\nprint("ready")\ntry:\n time.sleep(30)\n'
                               'except KeyboardInterrupt:\n print("cleaned up")\n raise SystemExit(130)\n')
        process = subprocess.Popen([sys.executable, str(self.launcher), 'module-build'],
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            self.assertTrue(select.select([process.stdout], [], [], 5)[0])
            self.assertEqual(process.stdout.readline(), 'ready\n')
            os.killpg(process.pid, signal.SIGINT)
            output, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130)
            self.assertIn('cleaned up', output)
            self.assertEqual(error, '')
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()

    def test_make_prints_full_error_when_worker_is_killed(self):
        self.worker.write_text('import os, signal\nos.kill(os.getpid(), signal.SIGKILL)\n')
        result = subprocess.run(['/usr/bin/make', '--no-print-directory', 'module-build',
                                 f'LAB={sys.executable} {self.launcher}'], cwd=ROOT,
                                env=dict(self.env, KERNEL_MESSAGES='BUG: fixture kernel trace\n'),
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn('terminated by SIGKILL', result.stderr)
        self.assertIn('BUG: fixture kernel trace', result.stderr)
        self.assertIn('Error 137', result.stderr)


if __name__ == '__main__':
    unittest.main()

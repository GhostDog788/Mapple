#!/usr/bin/env python3
"""Supervise the development CLI so fatal signals can be explained to Make users.

Keep this process outside the build's filesystem walks. In particular, SIGKILL
cannot be caught inside lab.py, but its parent can still report the termination
and read the host kernel journal. Child output goes directly to the terminal.
"""
from datetime import datetime, timezone
from pathlib import Path
import signal
import subprocess
import sys


def journal(arguments):
    try:
        result = subprocess.run(
            ['journalctl', '-k', '--boot', '--no-pager', '--quiet', *arguments],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            errors='replace', timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        return '', str(error)
    if result.returncode:
        return '', result.stderr.strip() or f'journalctl exited with status {result.returncode}'
    return result.stdout, result.stderr.strip()


def journal_start():
    # A cursor excludes earlier crashes, even ones from the same second.
    since = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S.%f UTC')
    output, _ = journal(['--lines=0', '--show-cursor'])
    for line in output.splitlines():
        if line.startswith('-- cursor: '):
            return ['--after-cursor=' + line.removeprefix('-- cursor: ')]
    return ['--since=' + since]


def report_failure(command, code, start):
    if code < 0:
        number = -code
        try:
            name = signal.Signals(number).name
        except ValueError:
            name = str(number)
        print(f'mapple dev: {command} terminated by {name} (signal {number}).',
              file=sys.stderr, flush=True)
        if number == signal.SIGKILL:
            print('The worker could not print an error or run cleanup. SIGKILL alone '
                  'does not identify the cause.', file=sys.stderr, flush=True)
    # Exit status 1 can also mean a compiler or another descendant was killed.
    # Include the entire interval, not only a grep of the first Oops line.
    output, error = journal(start)
    if output.strip():
        print('Host kernel messages recorded during this command '
              '(concurrent events may be unrelated):', file=sys.stderr)
        print(output, end='' if output.endswith('\n') else '\n', file=sys.stderr)
    elif code < 0 or error:
        detail = f'Kernel diagnostics unavailable: {error}' if error else (
            'No new host kernel messages were available. The termination cause is unknown.')
        print('mapple dev: ' + detail, file=sys.stderr)
    if error and output.strip():
        print('mapple dev: Kernel journal notice: ' + error, file=sys.stderr)


def supervise(argv, command):
    start = journal_start()
    child = subprocess.Popen(argv)
    previous_term = signal.getsignal(signal.SIGTERM)

    def terminate(signum, _frame):
        child.send_signal(signum)

    signal.signal(signal.SIGTERM, terminate)
    interrupted = False
    try:
        while True:
            try:
                code = child.wait()
                break
            except KeyboardInterrupt:
                # Terminal Ctrl+C already reaches the child in our process group.
                # Wait for its cleanup rather than leaving it behind.
                interrupted = True
    finally:
        signal.signal(signal.SIGTERM, previous_term)
    if code and not interrupted and code not in (-signal.SIGINT, -signal.SIGTERM, 130, 143):
        report_failure(command, code, start)
    return 128 - code if code < 0 else code


def main():
    args = sys.argv[1:]
    return supervise([sys.executable, '-u', str(Path(__file__).with_name('lab.py')), *args],
                     args[0] if args else 'help')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as error:
        print('mapple dev: Could not supervise command: ' + str(error), file=sys.stderr)
        sys.exit(1)

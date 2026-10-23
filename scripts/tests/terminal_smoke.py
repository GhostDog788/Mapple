"""PTY driver used behind a real terminal launcher during the local lab test."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import sys
import termios
import time

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm-256color'
    fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 35, 140, 0, 0))
    os.execvp(sys.argv[1], sys.argv[1:])
output = b''
started = time.monotonic()
sent = False
done = 0
status = 0
try:
    while time.monotonic() - started < 45:
        if select.select([fd], [], [], .1)[0]:
            try:
                output += os.read(fd, 65536)
            except OSError:
                break
        if not sent and time.monotonic() - started > 5:
            if 'gdb-attach' in sys.argv:
                os.write(fd, b'detach\nquit\n')
            else:
                os.write(fd, b'\x03')
            sent = True
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            break
    else:
        raise TimeoutError('Terminal did not exit.')
    if not done:
        done, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0, (status, output[-1500:])
    assert b'mapple' in output.lower() or b'gdb' in output.lower(), output[-1500:]
finally:
    if os.environ.get('MAPPLE_TERMINAL_LOG'):
        Path(os.environ['MAPPLE_TERMINAL_LOG']).write_bytes(output)
    os.close(fd)

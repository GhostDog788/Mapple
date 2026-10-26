"""Private state, locking and transport shared by development commands."""
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
STATE = Path(os.environ.get('MAPPLE_LAB_DIR', ROOT / '.lab')).resolve()


def fail(message):
    raise RuntimeError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def name(value):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', value or ''):
        fail('Names must start with a letter or digit and contain letters, digits, dots, _ or -.')
    return value


def option(key, default=None):
    return os.environ.get(key) or default


def directory(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    return path


def save(path, data):
    directory(STATE).chmod(0o700)
    directory(path.parent)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with open(temporary, 'x', opener=lambda p, flags: os.open(p, flags, 0o600)) as f:
            json.dump(data, f, indent=2)
            f.write('\n')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def load(path):
    if not path.is_file():
        fail(f'Missing record: {path}')
    return json.loads(path.read_text())


def record(kind, value):
    return STATE / kind / name(value) / 'record.json'


def get(kind, value):
    return load(record(kind, value))


def records(kind):
    return [load(p) for p in sorted((STATE / kind).glob('*/record.json'))]


@contextmanager
def lock(key, wait=0):
    path = directory(STATE / 'locks') / (name(key) + '.lock')
    with open(path, 'a') as f:
        deadline = time.monotonic() + wait
        while True:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    fail(f'{key} is busy in another command. Retry when it finishes.')
                time.sleep(.1)
        try:
            yield f.fileno()
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def run(argv, **kwargs):
    kwargs.setdefault('cwd', ROOT)
    return subprocess.run([str(a) for a in argv], check=True, **kwargs)


def output_log(process, path):
    def drain():
        with open(path, 'wb') as log:
            while True:
                chunk = process.stdout.read1(65536)
                if not chunk:
                    break
                log.write(chunk)
                log.flush()
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    thread = threading.Thread(target=drain, daemon=True)
    thread.start()
    return thread


def logged(argv, path, **kwargs):
    process = subprocess.Popen([str(a) for a in argv], cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, **kwargs)
    writer = output_log(process, path)
    try:
        code = process.wait()
    except BaseException:
        process.terminate()
        process.wait(timeout=10)
        raise
    finally:
        writer.join(timeout=10)
    if code:
        reason = (f'terminated by {signal.Signals(-code).name} (signal {-code})'
                  if code < 0 else f'exited with status {code}')
        fail(f'Build command {reason}. Output was streamed above; saved log: {path}')


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def active():
    path = STATE / 'active-machine'
    if not path.exists():
        if not records('machines'):
            fail('No active machine: no machines are registered.\n'
                 'Create a QEMU VM: make machine-create NAME=dev KERNEL=<kernel>\n'
                 'Or register an SSH target: make machine-add NAME=client HOST=<host> SSH_USER=<user>')
        fail('No active machine. Use make machine-use NAME=<machine>.')
    return name(path.read_text().strip())


def machine():
    return get('machines', option('MACHINE') or active())


def connection_key(m):
    host = m['host']
    try:
        host = sorted({entry[4][0] for entry in socket.getaddrinfo(host, None)})[0]
    except socket.gaierror:
        pass  # SSH configuration aliases need not resolve through DNS here.
    identity = f'{host}:{m.get("ssh_port", 22)}'
    return 'target-' + hashlib.sha256(identity.encode()).hexdigest()[:24]


def ssh_args(m):
    args = ['ssh', '-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=5',
            '-o', 'ServerAliveInterval=10', '-o', f'UserKnownHostsFile={STATE / "known_hosts"}',
            '-p', str(m.get('ssh_port', 22))]
    if m.get('ssh_key'):
        args += ['-i', m['ssh_key'], '-o', 'IdentitiesOnly=yes']
    if m.get('ssh_password'):
        args = ['sshpass', '-e', *args]
    else:
        args += ['-o', 'BatchMode=yes']
    return args + [f'{m["user"]}@{m["host"]}']


def ssh(m, command, sudo=False, **kwargs):
    password = m.get('sudo_password') or m.get('ssh_password', '')
    if sudo and m['user'] != 'root':
        if 'input' in kwargs:
            fail('Internal error: privileged transport cannot also consume stdin.')
        command = "sudo -k -S -p '' sh -c " + shlex.quote(command)
        kwargs['input'] = (password + '\n').encode()
    elif sudo:
        command = 'sh -c ' + shlex.quote(command)
    kwargs.setdefault('timeout', 30)
    kwargs.setdefault('capture_output', True)
    return run([*ssh_args(m), command], env=dict(os.environ, SSHPASS=m.get('ssh_password', '')), **kwargs)


def send(m, source, destination):
    # POSIX cat avoids requiring rsync, scp's SFTP server, or Python on routers.
    with open(source, 'rb') as f:
        result = subprocess.run([*ssh_args(m), 'cat > ' + shlex.quote(destination)], stdin=f,
            env=dict(os.environ, SSHPASS=m.get('ssh_password', '')), capture_output=True, timeout=300)
    if result.returncode:
        fail(result.stderr.decode(errors='replace'))


def receive(m, source, destination):
    directory(destination.parent)
    with open(destination, 'wb') as f:
        result = subprocess.run([*ssh_args(m), 'cat ' + shlex.quote(source)], stdout=f,
            stderr=subprocess.PIPE, env=dict(os.environ, SSHPASS=m.get('ssh_password', '')), timeout=300)
    if result.returncode:
        destination.unlink(missing_ok=True)
        fail(result.stderr.decode(errors='replace'))


def wait_ssh(m, old_boot=None, seconds=240):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            boot = ssh(m, 'cat /proc/sys/kernel/random/boot_id', timeout=8).stdout.decode().strip()
            if old_boot is None or boot != old_boot:
                return
        except (subprocess.SubprocessError, OSError):
            pass
        time.sleep(2)
    fail('Machine did not reconnect. Inspect its console and machine-show; assignment was not committed.')


def executable(value, env=None):
    found = shutil.which(value, path=(env or os.environ).get('PATH'))
    if not found:
        fail(f'Missing executable {value}. Install it or configure the kernel toolchain.')
    return found


def copy_tree(source, destination, source_only=False):
    """Copy a closed SDK, preserving internal links and closing external links.

    Out-of-tree Kbuild commonly links build/source back to its parent tree.
    Remember copied roots before traversing links so this cycle stays internal.
    """
    import fnmatch
    source = Path(source).expanduser().resolve(strict=True)
    if destination.exists():
        fail(f'Snapshot destination already exists: {destination}')
    directory(destination.parent)
    mappings = {}
    patterns = ['.git', '.lab', '__pycache__']
    if source_only:
        patterns += ['*.o', '*.a', '*.ko', '*.cmd', '*.mod', '*.mod.c', 'vmlinux', 'bzImage', 'Image', 'zImage', 'System.map']

    def covered(original):
        parents = [(old, new) for old, new in mappings.items() if original.is_relative_to(old)]
        if not parents:
            return None
        old, new = max(parents, key=lambda item: len(str(item[0])))
        return new / original.relative_to(old)

    def internal_link(link, target):
        link.unlink(missing_ok=True)
        link.symlink_to(os.path.relpath(target, link.parent))

    def copy(original, owned):
        mappings[original] = owned
        def ignore(folder, names):
            return [n for n in names if any(fnmatch.fnmatch(n, pattern) for pattern in patterns)
                    or Path(folder) / n in mappings]
        shutil.copytree(original, owned, symlinks=True, ignore=ignore)
        # A copied source parent may contain the already-copied build directory.
        for old, new in list(mappings.items()):
            if old != original and old.is_relative_to(original):
                back = owned / old.relative_to(original)
                directory(back.parent)
                if not back.exists() and not back.is_symlink():
                    internal_link(back, new)
        for link in list(owned.rglob('*')):
            if not link.is_symlink():
                continue
            before = original / link.relative_to(owned)
            # Back-links we just created already point inside managed storage.
            if not before.is_symlink():
                continue
            try:
                target = before.resolve(strict=True)
            except FileNotFoundError:
                # Partial SDKs can contain links to uninstalled optional tools.
                # Keep the owned snapshot closed; required missing files remain
                # capability/build blockers rather than external dependencies.
                print('Omitting dangling SDK link: ' + str(before))
                link.unlink()
                continue
            mapped = covered(target)
            if mapped is not None:
                internal_link(link, mapped)
            elif target.is_file():
                link.unlink()
                shutil.copy2(target, link)
            else:
                external = destination / '.external' / hashlib.sha256(str(target).encode()).hexdigest()[:16]
                if target not in mappings:
                    directory(external.parent)
                    copy(target, external)
                internal_link(link, mappings[target])
        return owned

    copy(source, destination)
    # Relocate textual build dependencies without modifying compiled ELF/DWARF.
    replacements = {str(old).encode(): str(new).encode() for old, new in mappings.items()}
    expression = re.compile(b'|'.join(re.escape(old) for old in sorted(replacements, key=len, reverse=True)))
    for p in destination.rglob('*'):
        if p.is_file() and (p.name == 'Makefile' or p.name.endswith('.cmd')):
            original_data = p.read_bytes()
            data = expression.sub(lambda match: replacements[match[0]], original_data)
            if data != original_data:
                p.write_bytes(data)
    return mappings

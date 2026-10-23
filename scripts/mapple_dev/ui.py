"""Host setup, frozen server/TUI configurations and machine-bound VS Code workspaces."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import shlex
import shutil
import subprocess
import sys
import go_toolchain
from . import core as c, kernels, machines, runs


def setup():
    if c.option('NO_INSTALL') != '1':
        c.run(['sudo', 'apt-get', 'update'])
        packages = ['build-essential', 'bc', 'bison', 'flex', 'dwarves', 'gcc-12', 'libelf-dev',
                    'libssl-dev', 'clangd-18', 'gdb', 'gdb-multiarch', 'python3', 'openssh-client',
                    'rsync', 'sshpass', 'iproute2', 'openssl', 'qemu-system-x86', 'qemu-utils', 'cloud-image-utils', 'xterm']
        if not shutil.which('go'):
            packages.append('golang-go')
        if c.option('TOOLCHAIN_PACKAGES'):
            packages += shlex.split(c.option('TOOLCHAIN_PACKAGES'))
        c.run(['sudo', 'apt-get', 'install', '-y', *packages])
        if Path('/dev/kvm').exists() and not os.access('/dev/kvm', os.R_OK | os.W_OK) and not machines.kvm_group_member():
            import pwd
            c.run(['sudo', 'usermod', '-aG', 'kvm', pwd.getpwuid(os.getuid()).pw_name])
    c.executable('go')
    c.run(['go', 'mod', 'download'], cwd=c.ROOT / 'user', env=go_toolchain.environment())
    c.directory(c.STATE).chmod(0o700)
    if c.option('NO_INSTALL') != '1' and shutil.which('code'):
        from . import navigation
        navigation.install_extension()
    print('Setup complete. Define a kernel with kernel-create or kernel-import, then register/create a machine.')


def app_build(component):
    command = 'mappled' if component == 'server' else 'mapple'
    output = c.directory(c.STATE / 'builds/go') / command
    with c.lock('go-build-' + component, wait=300):
        c.run(['go', 'build', '-o', output, './cmd/' + command], cwd=c.ROOT / 'user',
              env=dict(go_toolchain.environment(), CGO_ENABLED='0'))
    return output


def terminal(title, argv):
    if c.option('FOREGROUND') == '1':
        os.execvpe(str(argv[0]), [str(a) for a in argv], os.environ)
    if c.option('TERMINAL'):
        prefix = shlex.split(c.option('TERMINAL'))
    elif shutil.which('gnome-terminal') and os.environ.get('DISPLAY'):
        prefix = ['gnome-terminal', '--title', title, '--']
    elif shutil.which('konsole') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        prefix = ['konsole', '-e']
    elif shutil.which('x-terminal-emulator') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        prefix = ['x-terminal-emulator', '-T', title, '-e']
    else:
        c.fail('No graphical terminal. Use FOREGROUND=1 or set TERMINAL="xterm -e".')
    c.run([*prefix, *argv])


def app_config(m):
    if not m.get('kernel'):
        c.fail('Assign/adopt a kernel before starting this machine application session.')
    with c.lock('app-config', wait=30):
        candidates = [r for r in c.records('runs') if r['kind'] == 'server' and r['machine_id'] == m['id'] and r['status'] in ('configured', 'running')]
        available = []
        for r in candidates:
            if r['status'] == 'running' and not process_alive(r.get('pid'), r.get('pid_token')):
                r['status'] = 'stopped'
                c.save(c.record('runs', r['name']), r)
            else:
                available.append(r)
        if available:
            return max(available, key=lambda item: (item['status'] == 'running', item['created_at']))
        k = c.get('kernels', m['kernel'])
        r = runs.new(m, k, 'server')
        directory = runs.root(r)
        cert, key = directory / 'server.crt', directory / 'server.key'
        c.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key,
               '-out', cert, '-days', '365', '-subj', '/CN=localhost',
               '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key.chmod(0o600)
        token = secrets.token_hex(32)
        port = machines.free_port(8443)
        r.update(port=port, status='configured')
        c.save(directory / 'server.json', {'listen_address': f'127.0.0.1:{port}',
               'tls_cert_file': str(cert), 'tls_key_file': str(key), 'handshake_timeout_seconds': 30,
               'lease_timeout_minutes': 15, 'identities': [{'name': 'dev', 'token_sha256': hashlib.sha256(token.encode()).hexdigest()}]})
        c.save(directory / 'tui.json', {'active_profile': 'dev', 'profiles': {'dev': {
               'server_url': f'https://127.0.0.1:{port}', 'bearer_token': token, 'ca_file': str(cert)}},
               'module_profiles': {m['name']: {'transport': 'tcp', 'host': m['host'], 'port': m.get('module_port', 4444)}}})
        c.save(c.record('runs', r['name']), r)
        return r


def process_token(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return fields[19] if fields[0] != 'Z' else None
    except (OSError, IndexError):
        return None


def process_alive(pid, token=None):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        current = process_token(pid)
        return current is not None and (token is None or current == token)
    except ProcessLookupError:
        return False


def server(m):
    binary = app_build('server')
    r = app_config(m)
    with c.lock('server-' + r['name']):
        if r['status'] == 'running' and process_alive(r.get('pid'), r.get('pid_token')):
            c.fail('Server already running for this machine.')
        local = runs.root(r) / 'mappled'
        shutil.copyfile(binary, local)
        local.chmod(0o700)
        p = subprocess.Popen([str(local), '-config', str(runs.root(r) / 'server.json')], cwd=c.ROOT,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        writer = c.output_log(p, runs.root(r) / 'server.log')
        r.update(pid=p.pid, pid_token=process_token(p.pid), status='running')
        c.save(c.record('runs', r['name']), r)
        print(f'Server {r["name"]} for {m["name"]}: https://127.0.0.1:{r["port"]}', flush=True)
        previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP)}
        for sig in previous:
            signal.signal(sig, lambda signum, frame: p.terminate() if p.poll() is None else None)
        try:
            p.wait()
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
            if p.poll() is None:
                p.terminate()
                p.wait(timeout=15)
            writer.join(timeout=10)
            r['status'] = 'stopped'
            c.save(c.record('runs', r['name']), r)
        if p.returncode:
            c.fail('Server exited with status ' + str(p.returncode))


def tui(m):
    binary = app_build('tui')
    r = app_config(m)
    frozen = runs.new(m, c.get('kernels', m['kernel']), 'tui')
    local = runs.root(frozen) / 'mapple'
    shutil.copyfile(binary, local)
    local.chmod(0o700)
    frozen.update(server_run=r['name'], status='launched')
    c.save(c.record('runs', frozen['name']), frozen)
    terminal('Mapple: ' + m['name'], [local, '-config', runs.root(r) / 'tui.json'])


def vscode(m):
    if not m.get('kernel'):
        c.fail('Assign or adopt a kernel first.')
    if c.option('NO_OPEN') != '1':
        from . import navigation
        navigation.ensure_extension()
    k = c.get('kernels', m['kernel'])
    if not m.get('gdb') or not kernels.characteristics(k)['build']:
        c.fail('VS Code debugging needs build support and a configured GDB endpoint.')
    r = runs.new(m, k)
    directory = runs.root(r)
    # A workspace gets its own reserved run. Its prelaunch task prepares exactly
    # that run; changing the active machine cannot redirect this F5 operation.
    program = str(directory / 'mapple-relocated.ko')
    wrapper = directory / 'gdb-wrapper'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.join(['env', 'MAPPLE_LAB_DIR=' + str(c.STATE), sys.executable, str(c.ROOT / 'scripts/lab.py'),
                       'gdb-attach', '--run', r['name'], '--']) + ' "$@"\n')
    wrapper.chmod(0o700)
    arch = kernels.ARCHES.get(k['arch'], (None, None, None, None))[3]
    launch = {'name': 'Mapple: ' + m['name'], 'type': 'mapple-kernel', 'request': 'launch',
              'program': program, 'cwd': str(c.ROOT), 'MIMode': 'gdb', 'miDebuggerPath': str(wrapper),
              'preLaunchTask': 'Prepare ' + r['name'], 'postDebugTask': 'Finish ' + r['name'], 'externalConsole': False,
              'miDebuggerServerAddress': 'serial-target:0' if m['gdb'].startswith('/') else m['gdb'], 'customLaunchSetupCommands': [
                  {'text': 'source ' + str(directory / 'debug.gdb'), 'ignoreFailures': False}],
              'launchCompleteCommand': 'exec-continue'}
    if arch in ('x64', 'x86', 'arm', 'arm64'):
        launch['targetArchitecture'] = arch
    workspace = {'folders': [{'path': str(c.ROOT)}],
                 'settings': {'mapple.navigation.labDirectory': str(c.STATE),
                              'clangd.path': str(c.ROOT / 'scripts/clangd.py'),
                              'C_Cpp.intelliSenseEngine': 'disabled',
                              'clangd.arguments': ['--mapple-state-dir=' + str(c.STATE)]},
                 'launch': {'version': '0.2.0', 'configurations': [launch]},
                 'tasks': {'version': '2.0.0', 'tasks': [{'label': 'Prepare ' + r['name'], 'type': 'process',
                     'command': sys.executable, 'args': [str(c.ROOT / 'scripts/lab.py'), 'vscode-prepare', '--run', r['name']],
                     'options': {'cwd': str(c.ROOT), 'env': {'MAPPLE_LAB_DIR': str(c.STATE),
                                'MIN_DEBUG': c.option('MIN_DEBUG', 'module')}}, 'problemMatcher': ['$gcc']},
                     {'label': 'Finish ' + r['name'], 'type': 'process', 'command': sys.executable,
                      'args': [str(c.ROOT / 'scripts/lab.py'), 'vscode-finish', '--run', r['name']],
                      'options': {'cwd': str(c.ROOT), 'env': {'MAPPLE_LAB_DIR': str(c.STATE)}}, 'problemMatcher': []}]}}
    # Keep the script source absolute and whitespace-safe (GDB's source command
    # accepts unquoted paths including spaces as its filename).
    workspace_path = directory / (m['name'] + '.code-workspace')
    c.save(workspace_path, workspace)
    print('Workspace: ' + str(workspace_path))
    if c.option('NO_OPEN') != '1':
        c.run(['code', '--new-window', workspace_path])
    return workspace_path

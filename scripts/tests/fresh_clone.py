#!/usr/bin/env python3
"""Real two-machine lab acceptance test. Run on an isolated fresh clone, not your working lab.

Requires Ubuntu host tools (make setup), roughly 20 GiB disk, and 4 GiB free RAM.
Use --resume only for this harness's own interrupted test lab. It preserves logs
and destroys its two owned test VMs unless --keep-machines is supplied.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import socketserver
import select
import threading
from contextlib import contextmanager
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from protocol import RPC

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--resume', action='store_true')
parser.add_argument('--keep-machines', action='store_true')
parser.add_argument('--skip-setup', action='store_true')
parser.add_argument('--phase', choices=('all', 'live', 'debug', 'vscode', 'tiers', 'switch'), default='all')
args = parser.parse_args()
repo = args.repo.resolve()
# This destructive acceptance fixture must never inherit another lab's state.
os.environ['MAPPLE_LAB_DIR'] = str(repo / '.lab')
if (repo / '.lab/machines').exists() and not args.resume:
    parser.error('Lab already contains machines. Use a fresh clone or --resume for this harness only.')
sys.path.insert(0, str(repo / 'scripts'))
from mapple_dev import core as c, kernels, machines, runs, ui
import go_toolchain
logs = c.directory(repo / '.lab/acceptance')
env = dict(go_toolchain.environment(), PYTHONUNBUFFERED='1')
results = {}


def command(argv, label, extra=None, timeout=300, input_data=None):
    print(label, flush=True)
    with open(logs / (label + '.log'), 'wb') as f:
        process = subprocess.run([str(a) for a in argv], cwd=repo, env=dict(env, **(extra or {})),
                                 stdout=f, stderr=subprocess.STDOUT, timeout=timeout, input=input_data)
    if process.returncode:
        raise AssertionError(f'{label}: exited {process.returncode}\n' + (logs / (label + '.log')).read_text(errors='replace')[-10000:])
    results[label] = 'PASS'
    (logs / ('summary.json' if args.phase == 'all' else 'summary-' + args.phase + '.json')).write_text(json.dumps(results, indent=2) + '\n')


def make(target, label=None, **options):
    command(['make', target, *[f'{key}={value}' for key, value in options.items()]], label or target, timeout=3600)


def target_env(m):
    return dict(env, MAPPLE_TEST_TCP_HOST=m['host'], MAPPLE_TEST_TCP_PORT=str(m['module_port']))


def latest(machine, kind='module'):
    return max((r for r in c.records('runs') if r['machine'] == machine and r['kind'] == kind), key=lambda r: r['created_at'])


def wait_text(path, marker, process, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(errors='replace') if path.exists() else ''
        if marker in text:
            return text
        if process.poll() is not None:
            raise AssertionError(text[-6000:])
        time.sleep(.1)
    raise TimeoutError(marker + ': ' + text[-3000:])


def batch_debug(machine, kernel=False):
    tier = c.get('kernels', c.get('machines', machine)['kernel'])['capabilities']['tier']
    make('module-debug', f'prepare-{machine}-t{tier}-' + ('linux' if kernel else 'module'), MACHINE=machine)
    r = latest(machine)
    m = c.get('machines', machine)
    script = logs / (machine + f'-t{tier}' + ('-linux.gdb' if kernel else '-module.gdb'))
    breakpoint = 'kernel_sendmsg' if kernel else str(repo / 'kernel/src/control/commands/info/info.c') + ':21'
    script.write_text(f'''break {breakpoint}
printf "ACCEPTANCE_READY\\n"
continue
printf "ACCEPTANCE_BREAKPOINT\\n"
{('print sock' if r['debug_mode'] == 'kernel-source' else 'print $pc') if kernel else 'print context->session->id'}
{'stepi' if kernel and r['debug_mode'] == 'kernel-symbol' else 'next'}
frame
printf "ACCEPTANCE_STEP\\n"
disable breakpoints
detach
quit
''')
    log = logs / (script.stem + '.log')
    output = open(log, 'wb')
    process = subprocess.Popen([sys.executable, str(repo / 'scripts/lab.py'), 'gdb-attach', '--run', r['name'], '--', '-batch', '-x', str(script)],
                               cwd=repo, env=env, stdout=output, stderr=subprocess.STDOUT)
    return process, output, log, m


def finish_debug(job):
    process, output, log, m = job
    try:
        wait_text(log, 'ACCEPTANCE_READY', process)
        trigger = subprocess.run(['go', 'test', '-tags=integration', '-count=1', '-run', '^TestIntegration_Info$', './server/internal/moduleclient'],
                                 cwd=repo / 'user', env=target_env(m), capture_output=True, timeout=60)
        if trigger.returncode:
            raise AssertionError(trigger.stdout.decode() + trigger.stderr.decode() + log.read_text())
        process.wait(timeout=30)
        output.close()
        text = log.read_text()
        assert process.returncode == 0 and 'ACCEPTANCE_BREAKPOINT' in text and 'ACCEPTANCE_STEP' in text, text[-8000:]
        assert 'Breakpoint' in text, text[-3000:]
        results[log.stem] = 'PASS'
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)
        output.close()


def native_debug_tests():
    # Two independent machine/endpoint sessions exist at the same time.
    a, b = batch_debug('dev'), batch_debug('twin')
    wait_text(a[2], 'ACCEPTANCE_READY', a[0])
    wait_text(b[2], 'ACCEPTANCE_READY', b[0])
    make('machine-use', 'selection-during-debug', NAME='twin')
    finish_debug(a)
    finish_debug(b)
    finish_debug(batch_debug('dev', kernel=True))


def vscode_test(machine='dev', minimum='kernel-source'):
    make('vscode-open', 'vscode-workspace-' + machine, MACHINE=machine, MIN_DEBUG=minimum, NO_OPEN=1)
    r = latest(machine)
    workspace = c.load(next(runs.root(r).glob('*.code-workspace')))
    task = workspace['tasks']['tasks'][0]
    config = workspace['launch']['configurations'][0]
    # This lower-level test drives the Microsoft adapter directly. The real
    # editor suite separately exercises Mapple's DAP console-control bridge.
    config['type'] = 'cppdbg'
    config.pop('preLaunchTask')
    config.pop('postDebugTask')
    adapter = next((Path.home() / '.vscode/extensions').glob('ms-vscode.cpptools-*/debugAdapters/bin/OpenDebugAD7'), None)
    if not adapter:
        raise RuntimeError('Install the recommended Microsoft C/C++ extension to validate the real debug adapter.')
    for attempt in range(2):
        command([task['command'], *task['args']], ('vscode-prelaunch-' if attempt == 0 else 'vscode-relaunch-') + machine,
                extra=task['options']['env'])
        with open(logs / f'cppdbg-{machine}-{attempt}.log', 'w') as error:
            rpc = RPC([str(adapter)], error)
            try:
                rpc.dap('initialize', {'adapterID': 'cppdbg', 'clientID': 'vscode', 'linesStartAt1': True, 'columnsStartAt1': True, 'pathFormat': 'path'})
                seq = rpc.dap('launch', config, False)
                rpc.wait(lambda m: m.get('event') == 'initialized', 60)
                source = repo / 'kernel/src/control/commands/info/info.c'
                bp = rpc.dap('setBreakpoints', {'source': {'path': str(source)}, 'breakpoints': [{'line': 21}]})
                assert bp['breakpoints'][0]['verified'], bp
                rpc.dap('configurationDone', {})
                rpc.response(seq)
                m = c.get('machines', machine)
                trigger = subprocess.Popen(['go', 'test', '-tags=integration', '-count=1', '-run', '^TestIntegration_Info$', './server/internal/moduleclient'],
                                           cwd=repo / 'user', env=target_env(m), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                event = rpc.wait(lambda m: m.get('event') == 'stopped', 40)
                assert event['body']['reason'] == 'breakpoint', event
                tid = event['body']['threadId']
                frame = rpc.dap('stackTrace', {'threadId': tid, 'levels': 1})['stackFrames'][0]
                assert frame['source']['path'] == str(source) and frame['line'] == 21, frame
                value = rpc.dap('evaluate', {'expression': 'context->session->id', 'frameId': frame['id'], 'context': 'watch'})
                assert value.get('result'), value
                rpc.dap('next', {'threadId': tid})
                rpc.wait(lambda m: m.get('event') == 'stopped', 40)
                next_frame = rpc.dap('stackTrace', {'threadId': tid, 'levels': 1})['stackFrames'][0]
                assert next_frame['line'] != 21, next_frame
                rpc.dap('setBreakpoints', {'source': {'path': str(source)}, 'breakpoints': []})
                rpc.dap('continue', {'threadId': tid})
                output = trigger.communicate(timeout=45)[0]
                assert trigger.returncode == 0, output
                rpc.dap('pause', {'threadId': tid})
                rpc.wait(lambda m: m.get('event') == 'stopped', 20)
                rpc.dap('continue', {'threadId': tid})
                rpc.dap('disconnect', {'terminateDebuggee': True})
                results['vscode-' + machine + '-breakpoint-watch-step-pause-detach'] = 'PASS'
            finally:
                if rpc.p.poll() is None:
                    rpc.p.terminate()
                    rpc.p.wait(timeout=10)
        # Deliberately omit postDebugTask on the first Stop: prelaunch must
        # recover even when VS Code/its launcher never runs cleanup.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            current = c.get('runs', r['name'])
            if not runs.process_identity(current['debugger_pid']):
                break
            time.sleep(.05)
        else:
            raise AssertionError('GDB did not exit after disconnect')
    finish = workspace['tasks']['tasks'][1]
    command([finish['command'], *finish['args']], 'vscode-post-debug-' + machine, extra=finish['options']['env'])
    assert c.get('runs', r['name'])['status'] == 'detached'
    results['vscode-' + machine + '-same-workspace-restart-and-cleanup'] = 'PASS'


def navigation_test():
    make('machine-use', 'navigation-selection', NAME='dev')
    k = c.get('kernels', 'baseline')
    source = kernels.path(k, 'source')
    with open(logs / 'clangd.log', 'w') as error:
        rpc = RPC(['clangd-18', '--compile-commands-dir=' + str(repo), '--background-index=false', '--clang-tidy=false'], error)
        try:
            rpc.lsp('initialize', {'processId': None, 'rootUri': repo.as_uri(), 'capabilities': {}})
            rpc.lsp('initialized', {}, False)
            files = [source / 'net/socket.c', repo / 'kernel/src/control/transports/tcp_server/tcp.c']
            for f in files:
                rpc.lsp('textDocument/didOpen', {'textDocument': {'uri': f.as_uri(), 'languageId': 'c', 'version': 1, 'text': f.read_text()}}, False)
            lines = files[-1].read_text().splitlines()
            line = next(i for i, text in enumerate(lines) if 'kernel_accept(' in text)
            column = lines[line].index('kernel_accept') + 3
            for _ in range(30):
                definition = rpc.lsp('textDocument/definition', {'textDocument': {'uri': files[-1].as_uri()}, 'position': {'line': line, 'character': column}})
                if definition and any(str(source / 'net/socket.c') in d.get('uri', d.get('targetUri', '')) for d in definition):
                    break
                time.sleep(.2)
            assert definition and any(str(source / 'net/socket.c') in d.get('uri', d.get('targetUri', '')) for d in definition), definition
            results['clangd-kernel-definition'] = 'PASS'
            rpc.lsp('shutdown', None)
            rpc.lsp('exit', None, False)
        finally:
            if rpc.p.poll() is None:
                rpc.p.terminate()
                rpc.p.wait(timeout=10)


@contextmanager
def tui_test_destination(m):
    # The existing Go TUI integration test fixes its destination port at 4444.
    # Relay a separate loopback address to the actual per-machine forwarded port
    # rather than changing application source/tests or accidentally testing dev twice.
    class Relay(socketserver.BaseRequestHandler):
        def handle(self):
            with socket.create_connection((m['host'], m['module_port'])) as peer:
                sockets = [self.request, peer]
                while True:
                    readable, _, _ = select.select(sockets, [], [], 30)
                    if not readable:
                        return
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        (peer if source is self.request else self.request).sendall(data)
    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True
    with Server(('127.0.0.2', 4444), Relay) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield '127.0.0.2'
        finally:
            server.shutdown()
            thread.join(5)


def live_tests():
    for name in ('dev', 'twin'):
        make('module-run', 'load-' + name, MACHINE=name)
        m = c.get('machines', name)
        command(['go', '-C', str(repo / 'user'), 'test', '-tags=integration', '-count=1', '-timeout=120s', './server/internal/moduleclient'],
                'tcp-' + name, extra=target_env(m), timeout=150)
    binary = logs / 'moduleclient.test'
    command(['go', '-C', str(repo / 'user'), 'test', '-c', '-tags=integration', '-o', binary, './server/internal/moduleclient'], 'build-chardev-test', extra={'CGO_ENABLED': '0'})
    m = c.get('machines', 'dev')
    c.send(m, binary, '/tmp/mapple-chardev.test')
    output = c.ssh(m, "chmod 700 /tmp/mapple-chardev.test; MAPPLE_TEST_CHARDEV_PATH=/dev/mapple /tmp/mapple-chardev.test -test.run '^TestIntegration_CharDev$' -test.v", sudo=True, timeout=90)
    (logs / 'chardev.log').write_bytes(output.stdout + output.stderr)
    results['chardev'] = 'PASS'
    processes = []
    try:
        for name in ('dev', 'twin'):
            f = open(logs / ('server-' + name + '.log'), 'wb')
            p = subprocess.Popen(['make', 'server-run', 'MACHINE=' + name], cwd=repo, env=env, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((p, f))
        sessions = {}
        for name in ('dev', 'twin'):
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                candidates = [r for r in c.records('runs') if r['kind'] == 'server' and r['machine'] == name and r['status'] == 'running' and ui.process_alive(r.get('pid'), r.get('pid_token'))]
                if candidates:
                    r = max(candidates, key=lambda item: item['created_at'])
                    config = c.load(runs.root(r) / 'tui.json')
                    profile = config['profiles']['dev']
                    try:
                        context = ssl.create_default_context(cafile=profile['ca_file'])
                        urllib.request.urlopen(profile['server_url'] + '/v1/health/live', context=context, timeout=1).close()
                        sessions[name] = r, profile, context
                        break
                    except OSError:
                        pass
                time.sleep(.2)
            else:
                raise TimeoutError('Server ' + name)
        assert sessions['dev'][0]['port'] != sessions['twin'][0]['port']
        for name, (r, profile, context) in sessions.items():
            try:
                urllib.request.urlopen(profile['server_url'] + '/v1/status', context=context, timeout=3)
            except urllib.error.HTTPError as error:
                assert error.code == 401
            else:
                raise AssertionError('Unauthenticated API accepted')
            machine = c.get('machines', name)
            assert c.load(runs.root(r) / 'tui.json')['module_profiles'][name]['port'] == machine['module_port']
            with tui_test_destination(machine) as host:
                command(['go', '-C', str(repo / 'user'), 'test', '-tags=integration', '-count=1', '-timeout=120s', './tui/...'],
                        'api-tui-' + name, extra=dict(target_env(machine), MAPPLE_TUI_SERVER_URL=profile['server_url'],
                        MAPPLE_TUI_TOKEN=profile['bearer_token'], MAPPLE_TUI_MODULE_HOST=host), timeout=150)
        # Selection does not retarget either already-running server configuration.
        before = [c.load(runs.root(r) / 'tui.json') for r, _, _ in sessions.values()]
        make('machine-use', 'switch-running-apps', NAME='twin')
        assert before == [c.load(runs.root(r) / 'tui.json') for r, _, _ in sessions.values()]
        if shutil_which('Xvfb') and shutil_which('xterm'):
            display = ':193'
            x = subprocess.Popen(['Xvfb', display, '-screen', '0', '1024x768x24'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(.5)
                launcher = 'xterm -display ' + display + ' -e ' + shlex.join([sys.executable, str(repo / 'scripts/tests/terminal_smoke.py')])
                command(['make', 'tui-run', 'MACHINE=dev', 'TERMINAL=' + launcher], 'real-terminal-tui',
                        extra={'DISPLAY': display, 'MAPPLE_TERMINAL_LOG': str(logs / 'tui-terminal.log')}, timeout=60)
            finally:
                x.terminate()
                x.wait(timeout=10)
        else:
            raise RuntimeError('Install Xvfb and xterm for graphical terminal validation.')
    finally:
        for p, f in processes:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                p.wait(timeout=15)
            f.close()


def shutil_which(value):
    import shutil
    return shutil.which(value)


def terminal_gdb_test():
    make('module-debug', 'prepare-gdb-terminal', MACHINE='dev')
    r = latest('dev')
    display = ':194'
    x = subprocess.Popen(['Xvfb', display, '-screen', '0', '1024x768x24'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(.5)
        launcher = 'xterm -display ' + display + ' -e ' + shlex.join([sys.executable, str(repo / 'scripts/tests/terminal_smoke.py')])
        command(['make', 'gdb-open', 'RUN=' + r['name'], 'TERMINAL=' + launcher], 'real-terminal-gdb-tui',
                extra={'DISPLAY': display, 'MAPPLE_TERMINAL_LOG': str(logs / 'gdb-terminal.log')}, timeout=60)
    finally:
        x.terminate()
        x.wait(timeout=10)


def switch_kernel_test():
    m = c.get('machines', 'dev')
    stock = c.get('kernels', 'dev-observed')
    image = logs / 'stock-image'
    modules = logs / 'stock-modules.tar.gz'
    image.write_bytes(c.ssh(m, 'cat /boot/vmlinuz-' + shlex.quote(stock['release']), sudo=True, timeout=120).stdout)
    modules.write_bytes(c.ssh(m, 'tar -czf - -C /lib/modules ' + shlex.quote(stock['release']), sudo=True, timeout=300).stdout)
    if not stock['capabilities']['installable']:
        make('kernel-enrich', 'retain-stock-installation', KERNEL=stock['name'], IMAGE=image, MODULES=modules)
    if machines.inspect(m)['release'] != c.get('kernels', 'baseline')['release']:
        make('machine-prepare', 'resume-switch-baseline', MACHINE='dev', KERNEL='baseline')
        m = c.get('machines', 'dev')
    assert c.get('kernels', stock['name'])['capabilities']['installable']
    make('machine-add', 'register-custom-installer', NAME='hooked', HOST=m['host'], SSH_USER=m['user'],
         SSH_PORT=m['ssh_port'], SSH_KEY=m['ssh_key'], INSTALLER='/bin/false')
    old = c.get('machines', 'hooked')['kernel']
    with open(logs / 'failed-installer.log', 'wb') as output:
        failed = subprocess.run(['make', 'machine-prepare', 'MACHINE=hooked', 'KERNEL=' + stock['name']], cwd=repo,
                                env=env, stdout=output, stderr=subprocess.STDOUT, timeout=120)
    assert failed.returncode != 0
    assert c.get('machines', 'hooked')['kernel'] == old
    assert c.get('machines', 'hooked')['transition']['status'] == 'failed'
    assert machines.inspect(m)['release'] == c.get('kernels', 'baseline')['release']
    results['failed-installer-preserves-running-kernel-and-assignment'] = 'PASS'
    make('machine-remove', 'remove-hook-record', MACHINE='hooked')
    make('machine-prepare', 'switch-to-stock-kernel', MACHINE='dev', KERNEL=stock['name'])
    assert c.get('machines', 'dev')['kernel'] == stock['name']
    assert machines.inspect(c.get('machines', 'dev'))['release'] == stock['release']
    make('machine-adopt', 'adopt-distribution-headers', MACHINE='dev')
    stock = c.get('kernels', stock['name'])
    assert stock['capabilities']['build'], stock['capabilities']
    # Exercise both adoption paths on our disposable stock guest. Hide only
    # its development headers briefly; preserve and restore installed files.
    client = c.get('machines', 'dev')
    make('machine-add', 'register-package-adoption', NAME='downloaded', KERNEL='downloaded-stock',
         HOST=client['host'], SSH_USER=client['user'], SSH_PORT=client['ssh_port'], SSH_KEY=client['ssh_key'])
    header_path = '/usr/src/linux-headers-' + stock['release']
    held = header_path + '.mapple-test-held'
    c.ssh(client, 'mv ' + shlex.quote(header_path) + ' ' + shlex.quote(held), sudo=True)
    try:
        make('machine-adopt', 'offline-without-headers', MACHINE='downloaded', KERNEL='downloaded-stock', OFFLINE=1)
        assert c.get('kernels', 'downloaded-stock')['capabilities']['tier'] == 0
        make('machine-adopt', 'download-matching-headers', MACHINE='downloaded', KERNEL='downloaded-stock')
        downloaded = c.get('kernels', 'downloaded-stock')
        assert downloaded['capabilities']['build'], downloaded['capabilities']
        kernels.validate(downloaded)
    finally:
        c.ssh(client, 'mv ' + shlex.quote(held) + ' ' + shlex.quote(header_path), sudo=True)
    make('machine-remove', 'remove-package-adoption-machine', MACHINE='downloaded')
    make('kernel-remove', 'remove-package-adoption-kernel', KERNEL='downloaded-stock')
    with open(logs / 'known-api-incompatibility.log', 'wb') as output:
        incompatible = subprocess.run(['make', 'module-build', 'KERNEL=' + stock['name']], cwd=repo, env=env, stdout=output, stderr=subprocess.STDOUT)
    assert incompatible.returncode != 0
    assert 'get_file_rcu' in (logs / 'known-api-incompatibility.log').read_text()
    results['stock-kernel-build-inputs-and-api-blocker'] = 'PASS'
    make('machine-prepare', 'switch-back-to-baseline', MACHINE='dev', KERNEL='baseline')
    assert machines.inspect(c.get('machines', 'dev'))['build_id'] == c.get('kernels', 'baseline')['capabilities']['build_id']
    make('module-run', 'reload-after-kernel-switch', MACHINE='dev')


def debug_fixtures(base):
    # Generate large test-only ELF files before starting VMs where possible.
    # Reuse only files tied to this exact kernel; an interrupted command's
    # zero-length output must never be treated as a valid cached artifact.
    expected = base['capabilities']['build_id']
    for filename, option, dwarf, label in (
            ('vmlinux-symbols', '--strip-debug', False, 'strip-linux-dwarf'),
            ('linux.debug', '--only-keep-debug', True, 'separate-linux-dwarf')):
        output = logs / filename
        info = kernels.elf_info(output)
        if info.get('build_id') == expected and info.get('dwarf') == dwarf and info.get('symbols'):
            results[label + '-verified-cache'] = 'PASS'
        else:
            command(['objcopy', option, kernels.path(base, 'vmlinux'), output], label)


def tier_tests():
    # Observe an existing target with no SDK in its guest, then supply a compact
    # prepared SDK copied from the independently built baseline kernel.
    m = c.get('machines', 'dev')
    if not c.record('machines', 'adopted').exists():
        make('machine-add', 'register-existing', NAME='adopted', KERNEL='adopted-observed', HOST=m['host'], SSH_USER=m['user'],
             SSH_PORT=m['ssh_port'], SSH_KEY=m['ssh_key'], MODULE_PORT=m['module_port'], GDB_ENDPOINT=m['gdb'], CC='gcc-12')
    k = c.get('kernels', 'adopted-observed')
    assert k['capabilities']['tier'] >= 0
    base = c.get('kernels', 'baseline')
    tree = kernels.path(base, 'build')
    sdk = c.directory(logs / 'sdk')
    import shutil
    for part in ('include', 'scripts', 'tools/objtool', 'tools/include', 'arch/x86/include'):
        destination = sdk / part
        if not destination.exists():
            c.copy_tree(tree / part, destination)
    for part in ('.config', 'Module.symvers', 'Makefile'):
        shutil.copy2(tree / part, sdk / part)
    for file in (tree / 'arch/x86').glob('Makefile*'):
        shutil.copy2(file, sdk / 'arch/x86' / file.name)
    if not k['capabilities']['build']:
        make('kernel-enrich', 'tier1-sdk', KERNEL=k['name'], BUILD=sdk)
    assert c.get('kernels', k['name'])['capabilities']['tier'] >= 1
    if not c.record('kernels', 'offline-sdk').exists():
        make('kernel-import', 'import-offline-sdk', NAME='offline-sdk', BUILD=sdk, ARCH='x86_64', CC='gcc-12', BUILD_ID=base['capabilities']['build_id'])
    make('module-build', 'build-imported-sdk-offline', KERNEL='offline-sdk')
    make('kernel-remove', 'remove-unassigned-import', KERNEL='offline-sdk')
    make('machine-configure', 'disable-debug-access', MACHINE='adopted', GDB_ENDPOINT='none')
    make('module-run', 'tier1-build-run', MACHINE='adopted')
    with open(logs / 'debug-without-endpoint.log', 'wb') as output:
        denied = subprocess.run(['make', 'module-debug', 'MACHINE=adopted'], cwd=repo, env=env, stdout=output, stderr=subprocess.STDOUT)
    assert denied.returncode != 0
    assert 'No debugger endpoint' in (logs / 'debug-without-endpoint.log').read_text()
    results['build-run-without-debug-access'] = 'PASS'
    make('machine-configure', 'restore-debug-access', MACHINE='adopted', GDB_ENDPOINT=m['gdb'])
    finish_debug(batch_debug('adopted'))
    vscode_test('adopted', 'module')
    if not kernels.path(c.get('kernels', k['name']), 'system_map'):
        make('kernel-enrich', 'tier2-system-map', KERNEL=k['name'], SYSTEM_MAP=kernels.path(base, 'system_map'))
    assert c.get('kernels', k['name'])['capabilities']['tier'] >= 2
    finish_debug(batch_debug('adopted', kernel=True))
    stripped = logs / 'vmlinux-symbols'
    debug_fixtures(base)
    if not kernels.path(c.get('kernels', k['name']), 'vmlinux'):
        make('kernel-enrich', 'tier2-symbols', KERNEL=k['name'], VMLINUX=stripped)
    assert c.get('kernels', k['name'])['capabilities']['tier'] >= 2
    finish_debug(batch_debug('adopted', kernel=True))
    if not kernels.path(c.get('kernels', k['name']), 'source'):
        make('kernel-enrich', 'tier2-add-source', KERNEL=k['name'], SOURCE=tree)
    assert c.get('kernels', k['name'])['capabilities']['tier'] >= 2
    debug = logs / 'linux.debug'
    debug_fixtures(base)
    if not kernels.path(c.get('kernels', k['name']), 'debug_info'):
        make('kernel-enrich', 'tier3-dwarf', KERNEL=k['name'], DEBUG_INFO=debug)
    assert c.get('kernels', k['name'])['capabilities']['tier'] == 3
    finish_debug(batch_debug('adopted', kernel=True))
    # Import is a stable copy, not an alias to the externally supplied SDK.
    header = sdk / 'include/linux/fs.h'
    header.write_text(header.read_text() + '\n/* original SDK changed after import */\n')
    kernels.validate(c.get('kernels', k['name']))
    results['import-snapshot-independent'] = 'PASS'
    make('machine-adopt', 'offline-adoption-existing-artifacts', MACHINE='adopted', OFFLINE=1)
    assert c.get('kernels', k['name'])['capabilities']['tier'] == 3


try:
    if args.phase == 'all':
        if not args.skip_setup:
            make('setup', 'setup')
        make('check', 'repository-checks')
        if not c.record('kernels', 'baseline').exists():
            make('kernel-create', 'define-kernel', NAME='baseline', VERSION='6.6.6', PROFILE='debug')
        make('kernel-build', 'build-linux', KERNEL='baseline', JOBS=2)
        debug_fixtures(c.get('kernels', 'baseline'))
        for name in ('dev', 'twin'):
            if not c.record('machines', name).exists():
                make('machine-create', 'create-' + name, NAME=name, KERNEL='baseline')
            make('machine-start', 'start-' + name, MACHINE=name)
            make('machine-inspect', 'inspect-stock-' + name, MACHINE=name)
            make('machine-prepare', 'prepare-' + name, MACHINE=name, KERNEL='baseline')
            before = machines.inspect(c.get('machines', name))['boot_id']
            make('machine-prepare', 'repeat-prepare-' + name, MACHINE=name, KERNEL='baseline')
            assert before == machines.inspect(c.get('machines', name))['boot_id']
            make('doctor', 'doctor-' + name, MACHINE=name)
            make('machine-start', 'start-already-running-' + name, MACHINE=name)
            command(['make', 'machine-ssh', 'MACHINE=' + name], 'ssh-shell-' + name, input_data=b'uname -r\nexit\n')
        make('machine-list', 'list-machines')
        make('kernel-list', 'list-kernels')
        make('machine-show', 'show-machine', MACHINE='dev')
        make('kernel-show', 'show-kernel', KERNEL='baseline')
        make('module-build', 'offline-module-build', KERNEL='baseline')
        live_tests()
        native_debug_tests()
        vscode_test()
        terminal_gdb_test()
        navigation_test()
        tier_tests()
        switch_kernel_test()
        prepared = c.get('kernels', 'baseline')
        make('clean', 'clean-application-outputs')
        kernels.validate(prepared)
        assert (c.STATE / 'machines/dev/vm/disk.qcow2').exists()
        assert c.records('runs')
        make('module-run', 'rebuild-after-clean', MACHINE='dev')
        make('server-build', 'server-build-after-clean')
        make('tui-build', 'tui-build-after-clean')
        make('machine-stop', 'graceful-stop', MACHINE='twin')
        deadline = time.monotonic() + 90
        while machines.owned_process(c.get('machines', 'twin')) and time.monotonic() < deadline:
            time.sleep(.2)
        assert machines.owned_process(c.get('machines', 'twin')) is None
        make('machine-start', 'restart-persistent-machine', MACHINE='twin')
        make('module-run', 'reload-after-machine-restart', MACHINE='twin')
    elif args.phase == 'live':
        live_tests()
    elif args.phase == 'debug':
        native_debug_tests()
        vscode_test()
        terminal_gdb_test()
        navigation_test()
    elif args.phase == 'vscode':
        vscode_test()
    elif args.phase == 'tiers':
        tier_tests()
    elif args.phase == 'switch':
        switch_kernel_test()
    results['result'] = 'PASS'
except BaseException as error:
    results['result'] = 'FAIL'
    results['error'] = str(error)
    raise
finally:
    (logs / ('summary.json' if args.phase == 'all' else 'summary-' + args.phase + '.json')).write_text(json.dumps(results, indent=2) + '\n')
    if not args.keep_machines:
        for name in ('dev', 'twin'):
            if c.record('machines', name).exists():
                try:
                    make('machine-destroy', 'cleanup-' + name, MACHINE=name)
                except BaseException as error:
                    print('Cleanup failed: ' + str(error), file=sys.stderr)
    print('Results: ' + str(logs / ('summary.json' if args.phase == 'all' else 'summary-' + args.phase + '.json')), flush=True)

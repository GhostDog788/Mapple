"""Offline compiler context, persistent per-kernel indexes and editor selection."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import time
import uuid
import zipfile

import compile_commands
from . import core as c, kernels
from .lsp import Client

DEFAULT_ARGS = ['--background-index', '--header-insertion=never', '--clang-tidy=false']


def root(k):
    return c.STATE / 'navigation' / c.name(k['id'])


def editor_file():
    return c.STATE / 'navigation/editor.json'


def capture(k, database):
    """Keep only navigation inputs; never depend on disposable module outputs."""
    entries = [compile_commands.make_clangd_compatible(e) for e in c.load(database)
               if Path(e['file']).is_file() and Path(e['file']).suffix in ('.c', '.h')]
    # Resolve custom SDK drivers using the same environment as the real build.
    env = kernels.toolenv(k)
    for entry in entries:
        driver = entry['arguments'][0]
        if resolved := shutil.which(driver, path=env.get('PATH')):
            entry['arguments'][0] = resolved
    directory = c.directory(root(k))
    origin = database.with_name('linux.origin.json')
    source = kernels.path(k, 'source')
    commands = c.load(origin)['kind'] if source and origin.exists() else 'unavailable' if source else 'headers-only'
    old = c.load(directory / 'record.json') if (directory / 'record.json').exists() else {}
    requested_engine = c.option('CLANGD') or old.get('clangd') or 'clangd-18'
    # Module builds do not require an editor engine; preparation does.
    engine = shutil.which(requested_engine) or requested_engine
    fingerprint = hashlib.sha256(json.dumps({'schema': 1, 'entries': entries, 'kernel': k['id'],
        'source': str(source) if source else None, 'commands': commands, 'clangd': engine}, sort_keys=True).encode()).hexdigest()
    drivers = sorted({e['arguments'][0] for e in entries if Path(e['arguments'][0]).is_absolute()})
    record = {'kernel': k['name'], 'kernel_id': k['id'], 'release': k.get('release'),
              'directory': str(directory), 'fingerprint': fingerprint, 'clangd': engine, 'kernel_root': str(kernels.root(k)),
              'source': str(source) if source else None, 'commands': commands,
              'scope': 'linux-source' if source else 'headers-only', 'units': len(entries), 'drivers': drivers}
    if old.get('fingerprint') != fingerprint:
        c.save(directory / 'compile_commands.json', entries)
        c.save(directory / 'record.json', record)
        (directory / 'ready.json').unlink(missing_ok=True)
    return record


def selection(m=None):
    """Called under the editor lock. A new context requires a new clangd process."""
    previous = c.load(editor_file()) if editor_file().exists() else {}
    manifest = {'machine': m['name'] if m else None, 'kernel': m.get('kernel') if m else None,
                'phase': 'no-active-machine' if not m else 'no-assigned-kernel'}
    destination = c.ROOT / 'compile_commands.json'
    if m and m.get('kernel'):
        k = c.get('kernels', m['kernel'])
        manifest.update(kernel_id=k['id'], release=k.get('release'), phase='missing-database')
        record = root(k) / 'record.json'
        if record.exists() and (root(k) / 'compile_commands.json').exists():
            manifest.update(c.load(record), phase='loading')
            # Kept for other CDB consumers. VS Code uses the per-kernel directory.
            data = (root(k) / 'compile_commands.json').read_text()
            if not destination.exists() or destination.read_text() != data:
                temporary = destination.with_name('compile_commands.' + uuid.uuid4().hex + '.tmp')
                temporary.write_text(data)
                temporary.replace(destination)
        else:
            destination.unlink(missing_ok=True)
    else:
        destination.unlink(missing_ok=True)
    same = all(manifest.get(key) == previous.get(key) for key in ('kernel_id', 'fingerprint', 'phase'))
    manifest['generation'] = previous['generation'] if same and previous.get('generation') else uuid.uuid4().hex
    if manifest != previous:
        c.save(editor_file(), manifest)
    return manifest


def refresh_selected(k):
    with c.lock('editor', wait=30):
        try:
            m = c.get('machines', c.active())
        except RuntimeError:
            return
        if m.get('kernel') == k['name']:
            selection(m)


def status(k=None):
    if k:
        directory = root(k)
        record = c.load(directory / 'record.json') if (directory / 'record.json').exists() else {
            'kernel': k['name'], 'kernel_id': k['id'], 'phase': 'missing-database'}
    else:
        record = c.load(editor_file()) if editor_file().exists() else {'phase': 'no-active-machine'}
        directory = Path(record['directory']) if record.get('directory') else None
    ready = c.load(directory / 'ready.json') if directory and (directory / 'ready.json').exists() else None
    if ready and ready.get('fingerprint') == record.get('fingerprint'):
        record = dict(record, preparation='ready', prepared_at=ready['at'])
    else:
        record = dict(record, preparation='not-prepared')
    sessions = []
    for file in (c.STATE / 'navigation/sessions').glob('*.json'):
        try:
            session = c.load(file)
            os.kill(int(session['pid']), 0)
            if session.get('generation') == record.get('generation'):
                sessions.append(session)
        except (OSError, RuntimeError, ValueError, KeyError):
            continue
    return dict(record, editors=sessions)


def arguments(record):
    drivers = ['--query-driver=' + ','.join(record['drivers'])] if record.get('drivers') else []
    return [record['clangd'], '--compile-commands-dir=' + record['directory'], *DEFAULT_ARGS, *drivers]


def probes(k):
    tcp = c.ROOT / 'kernel/src/control/transports/tcp_server/tcp.c'
    result = [(tcp, 'return mapple_session_manager_register(', 'mapple_session_manager_register',
               str(c.ROOT / 'kernel/src/control/sessions/session_manager.c'))]
    source = kernels.path(k, 'source')
    if source:
        result += [(tcp, 'int ret = kernel_accept(', 'kernel_accept', str(source / 'net/socket.c')),
                   (source / 'net/socket.c', 'inode = new_inode_pseudo(', 'new_inode_pseudo', str(source / 'fs/inode.c'))]
    return result


def verify(client, checks, timeout=30):
    """Do not open destination implementations to make a lookup pass."""
    from urllib.parse import unquote, urlparse
    opened = set()
    rows = []
    for file, needle, token, expected in checks:
        if file not in opened:
            client.open(file)
            opened.add(file)
        lines = file.read_text().splitlines()
        line = next((i for i, text in enumerate(lines) if needle in text), None)
        if line is None:
            c.fail(f'Cannot verify {token} in supplied source {file}.')
        params = {'textDocument': {'uri': file.as_uri()},
                  'position': {'line': line, 'character': lines[line].index(token) + 1}}
        start = time.monotonic()
        paths = []
        while time.monotonic() - start < timeout:
            result = client.request('textDocument/definition', params) or []
            if isinstance(result, dict):
                result = [result]
            paths = [unquote(urlparse(x.get('uri', x.get('targetUri', ''))).path) for x in result]
            if expected in paths:
                break
            time.sleep(.1)
        else:
            c.fail(f'Navigation did not reach {expected}; got {paths}. See the clangd log.')
        first = (time.monotonic() - start) * 1000
        samples = []
        for _ in range(20):
            start = time.monotonic()
            result = client.request('textDocument/definition', params) or []
            if isinstance(result, dict):
                result = [result]
            paths = [unquote(urlparse(x.get('uri', x.get('targetUri', ''))).path) for x in result]
            if expected not in paths:
                c.fail(f'Navigation changed destination while checking {token}: {paths}')
            samples.append((time.monotonic() - start) * 1000)
        rows.append({'symbol': token, 'destination': expected, 'first_ms': first,
                     'median_ms': statistics.median(samples), 'p95_ms': sorted(samples)[18]})
    return rows


def index(k, record, checks=None):
    timeout = int(c.option('NAV_TIMEOUT', '1800'))
    workers = int(c.option('JOBS', '4'))
    if timeout < 1 or workers < 1:
        c.fail('NAV_TIMEOUT and JOBS must be positive integers.')
    directory = root(k)
    ready_file = directory / 'ready.json'
    # A failed refresh must not leave a previous successful receipt looking ready.
    with c.lock('navigation-' + k['id'], wait=30), open(directory / 'clangd.log', 'w') as log:
        ready_file.unlink(missing_ok=True)
        c.executable(record['clangd'])
        start = time.monotonic()
        client = Client([*arguments(record), '-j=' + str(workers)], log, c.ROOT,
                        env=kernels.toolenv(k), timeout=min(timeout, 60))
        try:
            client.request('initialize', {'processId': None, 'rootUri': c.ROOT.as_uri(),
                           'capabilities': {'window': {'workDoneProgress': True}}})
            client.notify('initialized', {})
            entries = c.load(directory / 'compile_commands.json')
            if not entries:
                c.fail('The navigation database contains no source files.')
            client.open(Path(entries[0]['file']))
            last_print = 0
            while time.monotonic() - start < timeout:
                client.request('workspace/symbol', {'query': 'mapple'})
                if any(p.get('token') == 'backgroundIndexProgress' and p.get('value', {}).get('kind') == 'end' for p in client.progress):
                    break
                if time.monotonic() - last_print > 10:
                    message = client.progress[-1].get('value', {}).get('message', '') if client.progress else ''
                    print(f'Indexing {k["name"]}: {message or "loading compiler context"}', flush=True)
                    last_print = time.monotonic()
                time.sleep(.2)
            else:
                c.fail(f'Navigation indexing timed out. See {directory / "clangd.log"}; retry to reuse completed shards.')
            index_seconds = time.monotonic() - start
            results = verify(client, probes(k) if checks is None else checks)
            receipt = {'fingerprint': record['fingerprint'], 'at': c.now(), 'index_seconds': index_seconds,
                       'clangd_version': c.run([record['clangd'], '--version'], capture_output=True).stdout.decode().strip(),
                       'scope': record['scope'], 'commands': record['commands'], 'checks': results}
            if c.load(directory / 'record.json')['fingerprint'] != record['fingerprint']:
                c.fail('Navigation compiler context changed during preparation; retry.')
            c.save(ready_file, receipt)
            print(f'Navigation prepared: {k["name"]} ({record["scope"]}, {record["commands"]} commands).', flush=True)
            return receipt
        finally:
            client.close()


def prepare(k):
    # Refresh actual module compiler commands without connecting to any machine.
    from . import runs
    runs.build(k, publish=False)
    record = c.load(root(k) / 'record.json')
    refresh_selected(k)
    return index(k, record)


def launch(extra):
    manifest = c.load(editor_file()) if editor_file().exists() else {}
    if manifest.get('directory') and manifest.get('fingerprint'):
        record = manifest
    else:
        directory = c.directory(c.STATE / 'navigation/empty')
        c.save(directory / 'compile_commands.json', [])
        record = {'directory': str(directory), 'clangd': c.executable(c.option('CLANGD', 'clangd-18'))}
    if any(x.startswith('--compile-commands-dir') for x in extra):
        c.fail('Mapple selects the compilation database. Use machine-use to change kernels.')
    # Native exec preserves normal LSP signals and transport, without a proxy.
    env = kernels.toolenv(c.get('kernels', record['kernel'])) if record.get('kernel') else os.environ
    os.execvpe(record['clangd'], [*arguments(record), *extra], env)


def install_extension():
    """Package the dependency-free local extension without npm or vsce."""
    source = c.ROOT / 'tools/vscode-navigation'
    package = json.loads((source / 'package.json').read_text())
    output = c.directory(c.STATE / 'tools') / 'mapple-navigation.vsix'
    manifest = f'''<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
<Metadata><Identity Language="en-US" Id="navigation" Version="{package['version']}" Publisher="mapple"/><DisplayName>Mapple Navigation</DisplayName><Description xml:space="preserve">Selected-kernel source navigation</Description><Tags>mapple</Tags><Categories>Other</Categories><Properties><Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.90.0"/><Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="llvm-vs-code-extensions.vscode-clangd"/></Properties></Metadata>
<Installation><InstallationTarget Id="Microsoft.VisualStudio.Code"/></Installation><Dependencies/>
<Assets><Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/></Assets></PackageManifest>'''
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('extension.vsixmanifest', manifest)
        archive.writestr('[Content_Types].xml', '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="json" ContentType="application/json"/><Default Extension="js" ContentType="application/javascript"/><Default Extension="vsixmanifest" ContentType="text/xml"/></Types>')
        for file in ('package.json', 'extension.js'):
            archive.write(source / file, 'extension/' + file)
    c.run(['code', '--install-extension', output, '--force'])
    print('Installed Mapple Navigation. Reload any already-open VS Code window once.')

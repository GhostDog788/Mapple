#!/usr/bin/env python3
"""Exercise offline indexing and real VS Code switching in a disposable checkout.

Requires VS Code, clangd-18, the clangd extension, and xvfb-run on a headless host.
Does not connect to machines or change the developer's selection/profile.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]


def fixture(repo):
    """Use current tooling in a new checkout, with small real C kernel fixtures."""
    subprocess.run(['git', 'clone', '--quiet', '--local', '--no-hardlinks', str(REPO), str(repo)], check=True)
    files = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=REPO).split(b'\0')
    for item in files:
        if not item: continue
        relative = Path(os.fsdecode(item)); source = REPO / relative; destination = repo / relative
        if not source.is_file():
            destination.unlink(missing_ok=True); continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    sys.path.insert(0, str(repo / 'scripts'))
    from mapple_dev import core as c, kernels, navigation, machines, ui
    c.ROOT, c.STATE = repo, repo / '.lab'
    sources = {
        'kernel/src/main.c': 'int mapple_entry(void) { return 0; }\n',
        'kernel/src/control/transports/tcp_server/tcp.c': '#include "api.h"\nint mapple_call(void) {\nint ret = kernel_accept(0, 0, 0);\nreturn mapple_session_manager_register(ret);\n}\n',
        'kernel/src/control/sessions/session_manager.c': 'int mapple_session_manager_register(int value) { return value; }\n',
    }
    for relative, text in sources.items():
        file = repo / relative; file.parent.mkdir(parents=True, exist_ok=True); file.write_text(text)
    for name in ['a', 'b']:
        kernel = {'name': name, 'id': name+'-fixture', 'release': 'fixture-'+name, 'toolchain': {},
                  'arch': 'x86_64', 'linux_arch': 'x86', 'artifacts': {'source': 'source', 'build': 'build'}}
        build = kernels.root(kernel)/'build'
        for relative, text in {'Makefile': '', 'scripts/Makefile.build': '',
            'include/config/kernel.release': kernel['release'], 'include/generated/autoconf.h': '#define CONFIG_MODULES 1\n'}.items():
            file = build/relative; file.parent.mkdir(parents=True, exist_ok=True); file.write_text(text)
        source = kernels.root(kernel) / 'source'
        for relative, text in {
            'api.h': 'int kernel_accept(int,int,int);\nint new_inode_pseudo(int);\nint mapple_session_manager_register(int);\n',
            'init/main.c': 'int start_kernel_fixture;\n',
            'net/socket.c': '#include "api.h"\nint kernel_accept(int a,int b,int c) {\nint inode;\ninode = new_inode_pseudo(a);\nreturn inode;\n}\n',
            'fs/inode.c': 'int new_inode_pseudo(int value) { return value + '+('1' if name=='a' else '2')+'; }\n',
        }.items():
            file = source / relative; file.parent.mkdir(parents=True, exist_ok=True); file.write_text(text)
        c.save(c.record('kernels', name), kernel)
        db = c.directory(c.STATE / 'builds/module' / kernel['id']) / 'compile_commands.json'
        c.save(db, [{'directory': str(repo), 'file': str(file), 'arguments': ['gcc', '-I'+str(source), '-c', str(file)]}
                    for file in [*(repo / relative for relative in sources), source/'net/socket.c', source/'fs/inode.c']])
        c.save(db.with_name('linux.origin.json'), {'kind': 'recorded'})
        record = navigation.capture(kernel, db)
        navigation.index(kernel, record)
        c.save(c.record('machines', name), {'name': name, 'id': name+'-machine', 'kernel': name, 'gdb': '127.0.0.1:12345'})
    headers = {'name': 'headers', 'id': 'headers-fixture', 'release': 'headers-only',
               'toolchain': {}, 'artifacts': {'build': 'headers'}}
    include = c.directory(kernels.root(headers) / 'headers')
    (include/'api.h').write_text('int kernel_accept(int,int,int);\nint mapple_session_manager_register(int);\n')
    c.save(c.record('kernels', 'headers'), headers)
    db = c.directory(c.STATE/'builds/module'/headers['id'])/'compile_commands.json'
    c.save(db, [{'directory': str(repo), 'file': str(repo/relative),
                'arguments': ['gcc', '-I'+str(include), '-c', str(repo/relative)]} for relative in sources])
    record = navigation.capture(headers, db)
    navigation.index(headers, record)
    c.save(c.record('machines', 'headers'), {'name': 'headers', 'id': 'headers-machine', 'kernel': 'headers'})
    # Selecting an unprepared kernel must clear old definitions too.
    c.save(c.record('kernels', 'missing'), {'name': 'missing', 'id': 'missing-fixture'})
    c.save(c.record('machines', 'missing'), {'name': 'missing', 'id': 'missing-machine', 'kernel': 'missing'})
    c.save(c.record('machines', 'alias'), {'name': 'alias', 'id': 'alias-machine', 'kernel': 'a'})
    machines.select('a')
    os.environ['NO_OPEN'] = '1'
    return ui.vscode(c.get('machines', 'a'))


def main():
    evidence = REPO / '.lab/navigation-verification'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='checkout-', dir=evidence) as temporary:
        repo = Path(temporary) / 'mapple'
        debug_workspace = fixture(repo)
        extensions = subprocess.check_output(['code', '--list-extensions'], text=True).splitlines()
        for reopen in [False, True]:
            name = 'reopen-after-clean' if reopen else 'switch-and-clean'
            command = ['code', '--wait', '--new-window', '--disable-gpu', '--skip-welcome', '--skip-release-notes',
                       '--disable-workspace-trust', '--user-data-dir='+str(Path(temporary)/name),
                       '--extensionDevelopmentPath='+str(repo/'tools/vscode-navigation'),
                       '--extensionTestsPath='+str(repo/'scripts/tests/navigation_vscode.js')]
            for extension in extensions:
                if extension not in ['mapple.navigation', 'llvm-vs-code-extensions.vscode-clangd']:
                    command += ['--disable-extension', extension]
            command += [str(debug_workspace if reopen else repo)]
            if not os.environ.get('DISPLAY'): command = ['xvfb-run', '-a', *command]
            env = dict(os.environ, MAPPLE_NAV_TEST_ROOT=str(repo), MAPPLE_LAB_DIR=str(repo/'.lab'),
                       MAPPLE_NAV_TEST_RESULTS=str(evidence/(name+'.json')), MAPPLE_NAV_TEST_REOPEN=str(int(reopen)))
            with (evidence/(name+'.log')).open('w') as log:
                try:
                    subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
                finally:
                    logs = Path(temporary)/name/'logs'
                    if logs.exists():
                        shutil.copytree(logs, evidence/('logs-'+name), dirs_exist_ok=True)
            result = json.loads((evidence/(name+'.json')).read_text())
            if result.get('error'): raise RuntimeError(result['error'])
            print(name+': '+', '.join(result['checks']), flush=True)
    print('Fresh-checkout navigation tests passed. Evidence: '+str(evidence))


if __name__ == '__main__': main()

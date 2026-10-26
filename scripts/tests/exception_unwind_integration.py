#!/usr/bin/env python3
"""Verify fault-frame source/locals in real VS Code using a userspace core.

Uses an isolated editor profile. Never connects to a VM or reloads a module.
Requires x86-64 GCC, Python-enabled GDB, code, C/C++, clangd and xvfb if headless.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_exception_unwind import ROOT, FIXTURE, HELPER, build_fixture


def main():
    evidence = ROOT / '.lab/unwind-verification'
    evidence.mkdir(parents=True, exist_ok=True)
    result_file = evidence / 'vscode.json'
    result_file.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='vscode-', dir=evidence) as tmp:
        directory = Path(tmp)
        binary = build_fixture(directory)
        core = directory / 'exception.core'
        subprocess.run(['gdb', '-nx', '-q', '-batch', str(binary),
                        '-ex', 'set confirm off', '-ex', 'set debuginfod enabled off',
                        '-ex', 'handle SIGFPE nostop noprint pass', '-ex', 'break die', '-ex', 'run',
                        '-ex', 'generate-core-file ' + str(core), '-ex', 'kill'],
                       check=True, capture_output=True, timeout=30)
        context = directory / 'context.json'
        context.write_text(json.dumps({'binary': str(binary), 'core': str(core),
                                      'helper': str(HELPER), 'source': str(FIXTURE)}))
        command = ['code', '--wait', '--new-window', '--disable-gpu', '--skip-welcome', '--skip-release-notes',
                   '--disable-workspace-trust', '--user-data-dir=' + str(directory / 'profile'),
                   '--extensionDevelopmentPath=' + str(ROOT / 'tools/vscode-navigation'),
                   '--extensionTestsPath=' + str(ROOT / 'scripts/tests/exception_unwind_vscode.js')]
        extensions = subprocess.check_output(['code', '--list-extensions'], text=True).splitlines()
        if 'ms-vscode.cpptools' not in extensions:
            raise RuntimeError('Install Microsoft C/C++ to run the editor test.')
        for extension in extensions:
            if extension not in ('ms-vscode.cpptools', 'llvm-vs-code-extensions.vscode-clangd', 'mapple.navigation'):
                command += ['--disable-extension', extension]
        command.append(str(ROOT))
        if not os.environ.get('DISPLAY'):
            command = ['xvfb-run', '-a', *command]
        env = dict(os.environ, MAPPLE_LAB_DIR=str(directory / 'lab'),
                   MAPPLE_UNWIND_CONTEXT=str(context), MAPPLE_UNWIND_RESULTS=str(result_file))
        with (evidence / 'vscode.log').open('w') as log:
            try:
                subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=120)
            finally:
                logs = directory / 'profile/logs'
                if logs.exists():
                    shutil.copytree(logs, evidence / 'editor-logs', dirs_exist_ok=True)
        result = json.loads(result_file.read_text())
        if result.get('result') != 'PASS':
            raise RuntimeError(json.dumps(result))
        print('VS Code exception frames passed: ' + ', '.join(result['checks']))
        print('Evidence: ' + str(result_file))


if __name__ == '__main__':
    main()

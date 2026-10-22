#!/usr/bin/env python3
"""Real VS Code Start/step/Stop/Start regression on a disposable prepared machine.

Reloads Mapple on --machine, which must have full kernel-source debug support.
Uses an isolated editor profile; needs Microsoft C/C++ and xvfb-run if headless.
MAPPLE_LAB_DIR can select a disposable lab without changing the normal selection.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts'))
from mapple_dev import core as c, kernels, runs
import go_toolchain


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--machine', required=True)
    args = parser.parse_args()
    evidence = c.directory(c.STATE / 'debugging-verification')
    result_file = evidence / 'vscode-restart.json'
    result_file.unlink(missing_ok=True)
    subprocess.run(['make', 'vscode-open', 'MACHINE=' + args.machine,
                    'MIN_DEBUG=kernel-source', 'NO_OPEN=1'], cwd=REPO, check=True)
    r = max((r for r in c.records('runs') if r['machine'] == args.machine and r['kind'] == 'module'),
            key=lambda r: r['created_at'])
    workspace = next(runs.root(r).glob('*.code-workspace'))
    config = c.load(workspace)['launch']['configurations'][0]
    # Use the actual F5 command and workspace configuration, with diagnostics
    # enabled only in this disposable generated workspace.
    data = c.load(workspace)
    data['launch']['configurations'][0]['logging'] = {'engineLogging': True, 'trace': True, 'traceResponse': True}
    c.save(workspace, data)
    context = {'root': str(REPO), 'lab': str(c.STATE), 'run': r['name'],
               'machine': r['machine_snapshot'], 'toolchain': go_toolchain.select(), 'config': config,
               'kernel_source': str(kernels.path(c.get('kernels', r['kernel']), 'source'))}
    with tempfile.TemporaryDirectory(prefix='vscode-', dir=evidence) as temporary:
        root = Path(temporary)
        context_file = root / 'context.json'
        c.save(context_file, context)
        command = ['code', '--wait', '--new-window', '--disable-gpu', '--skip-welcome', '--skip-release-notes',
                   '--disable-workspace-trust', '--user-data-dir=' + str(root / 'profile'),
                   '--extensionDevelopmentPath=' + str(REPO / 'tools/vscode-navigation'),
                   '--extensionTestsPath=' + str(REPO / 'scripts/tests/debugging_vscode.js')]
        extensions = subprocess.check_output(['code', '--list-extensions'], text=True).splitlines()
        if 'ms-vscode.cpptools' not in extensions:
            raise RuntimeError('Install the Microsoft C/C++ extension to test VS Code debugging.')
        for installed in extensions:
            if installed not in ('ms-vscode.cpptools', 'llvm-vs-code-extensions.vscode-clangd', 'mapple.navigation'):
                command += ['--disable-extension', installed]
        command.append(str(workspace))
        if not os.environ.get('DISPLAY'):
            command = ['xvfb-run', '-a', *command]
        env = dict(go_toolchain.environment(), MAPPLE_LAB_DIR=str(c.STATE),
                   MAPPLE_DEBUG_TEST_CONTEXT=str(context_file), MAPPLE_DEBUG_TEST_RESULTS=str(result_file))
        with (evidence / 'vscode-restart.log').open('w') as log:
            try:
                subprocess.run(command, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT,
                               timeout=420, check=True)
            finally:
                editor_logs = root / 'profile/logs'
                if editor_logs.exists():
                    shutil.copytree(editor_logs, evidence / 'editor-logs', dirs_exist_ok=True)
        # Code's CLI may exit successfully even when extension-host tests fail.
        result = json.loads(result_file.read_text())
        if result.get('result') != 'PASS':
            raise RuntimeError(json.dumps(result))
        print('VS Code source debugging and workspace restart passed. Evidence: ' + str(result_file))


if __name__ == '__main__':
    main()

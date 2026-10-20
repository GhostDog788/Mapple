#!/usr/bin/env python3
"""Mapple development CLI. Make is the public interface; private state is in .lab."""
import argparse
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
from mapple_dev import core as c, kernels, machines, installers, runs, ui

COMMANDS = ('setup', 'kernel-create', 'kernel-import', 'kernel-enrich', 'kernel-build',
    'kernel-list', 'kernel-show', 'kernel-remove', 'machine-create', 'machine-add',
    'machine-list', 'machine-show', 'machine-configure', 'machine-inspect', 'machine-use', 'machine-adopt',
    'machine-prepare', 'machine-start', 'machine-stop', 'machine-ssh', 'machine-remove',
    'machine-destroy', 'module-build', 'module-run', 'module-debug', 'server-build',
    'server-run', 'tui-build', 'tui-run', 'gdb-open', 'gdb-attach', 'vscode-open',
    'vscode-prepare', 'debug-list', 'doctor', 'clean')


def public(value):
    if isinstance(value, dict):
        return {k: ('<redacted>' if any(s in k for s in ('password', 'bearer_token')) else public(v))
                for k, v in value.items() if k != 'hashes'}
    if isinstance(value, list):
        return [public(v) for v in value]
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=COMMANDS)
    parser.add_argument('--run')
    parser.add_argument('debugger_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    extra = args.debugger_args
    # The debugger wrapper passes --run after the subcommand, followed by GDB args.
    if '--run' in extra:
        index = extra.index('--run')
        os.environ['RUN'] = extra[index + 1]
        extra = extra[:index] + extra[index + 2:]
    elif args.run:
        os.environ['RUN'] = args.run
    if extra and extra[0] == '--':
        extra = extra[1:]
    cmd = args.command
    if cmd == 'setup':
        return ui.setup()
    if cmd == 'kernel-create':
        return kernels.create()
    if cmd == 'kernel-import':
        return kernels.import_kernel()
    if cmd == 'kernel-list':
        print('KERNEL                ARCH       RELEASE                                TIER  INSTALL  STATE       USED BY')
        for k in c.records('kernels'):
            cap = k.get('capabilities', {})
            refs = ','.join(m['name'] for m in c.records('machines') if m.get('kernel') == k['name']) or '-'
            release = k.get('release') or k.get('recipe', {}).get('version') or 'not identified'
            print(f'{k["name"]:21} {k["arch"]:10} {release:38} {str(cap.get("tier", 0)):5} {str(cap.get("installable", False)):8} {k["state"]:11} {refs}')
        return
    if cmd.startswith('kernel-'):
        k = kernels.selected()
        return {'kernel-show': lambda: print(json.dumps(public(k), indent=2)),
                'kernel-build': lambda: kernels.build(k), 'kernel-enrich': lambda: kernels.enrich(k),
                'kernel-remove': lambda: kernels.remove(k)}[cmd]()
    if cmd == 'machine-create':
        with c.lock('catalog'):
            return machines.create_vm()
    if cmd == 'machine-add':
        return machines.add()
    if cmd == 'machine-use':
        return machines.select(c.name(c.option('NAME')))
    if cmd == 'machine-list':
        print('  MACHINE              KERNEL                RUNNING                      TIER  MATCH          BLOCKERS')
        selected = (c.STATE / 'active-machine').read_text().strip() if (c.STATE / 'active-machine').exists() else ''
        for m in c.records('machines'):
            k = c.get('kernels', m['kernel']) if m.get('kernel') else None
            observed = m.get('observed', {})
            cap = k.get('capabilities', {}) if k else {}
            blockers = list(cap.get('blockers', []))
            match = machines.match(k, observed) if k and observed else 'not-inspected'
            if match == 'mismatch': blockers.append('running kernel mismatch')
            if observed.get('modules_disabled') == '1': blockers.append('module loading disabled')
            print(f'{"*" if m["name"] == selected else " "} {m["name"]:20} {m.get("kernel") or "-":21} {observed.get("release", "not inspected"):28} {str(cap.get("tier", 0)):5} {match:14} {", ".join(blockers) or "-"}')
        print('RUNNING is the last observation; machine-inspect refreshes it without modifying the target.')
        return
    if cmd in ('server-build', 'tui-build'):
        print(ui.app_build(cmd.split('-')[0]))
        return
    if cmd == 'module-build':
        print(runs.build(kernels.selected(), publish=not c.option('MACHINE') and not c.option('KERNEL')))
        return
    if cmd == 'debug-list':
        for r in c.records('runs'):
            if r['kind'] == 'module':
                print(r['name'], r['machine'], r['kernel'], r['status'], r.get('debug_mode') or '-')
        return
    if cmd in ('gdb-open', 'gdb-attach'):
        r = runs.find_run()
        if cmd == 'gdb-open':
            return ui.terminal('Mapple GDB: ' + r['machine'],
                               [sys.executable, str(c.ROOT / 'scripts/lab.py'), 'gdb-attach', '--run', r['name'], '--', '-tui'])
        return runs.attach(r, extra)
    if cmd == 'vscode-prepare':
        r = runs.find_run()
        # Use the frozen machine connection; validate live record identities too.
        m = c.get('machines', r['machine'])
        k = c.get('kernels', r['kernel'])
        if m['id'] != r['machine_id'] or m.get('kernel') != r['kernel']:
            c.fail('Machine assignment changed. Open a new VS Code workspace.')
        return runs.prepare(r['machine_snapshot'], k, True, r)
    if cmd == 'clean':
        for directory in sorted((c.STATE / 'builds/module').glob('*')):
            if directory.is_dir():
                with c.lock('build-' + directory.name):
                    shutil.rmtree(directory)
        for component in ('server', 'tui'):
            with c.lock('go-build-' + component):
                (c.STATE / 'builds/go' / ('mappled' if component == 'server' else 'mapple')).unlink(missing_ok=True)
        (c.ROOT / 'compile_commands.json').unlink(missing_ok=True)
        print('Cleaned application builds; preserved kernels, machines, disks and frozen runs.')
        return
    m = c.machine()
    if cmd == 'machine-configure':
        return machines.configure(m)
    if cmd == 'machine-show':
        if m.get('managed'):
            pid = machines.owned_process(m)
            m = dict(m, qemu={'running': pid is not None, 'pid': pid,
                     'console': str(machines.slot_path(m['name']).parent / 'vm/console.log')})
        return print(json.dumps(public(m), indent=2))
    if cmd == 'machine-inspect':
        with c.lock('machine-' + m['name']):
            observed = machines.inspect(m)
            if not m.get('kernel'):
                with c.lock('catalog'):
                    m['kernel'] = machines.observed_kernel(m, observed)['name']
                    c.save(machines.slot_path(m['name']), m)
        return print(json.dumps(observed, indent=2))
    if cmd == 'machine-adopt':
        return machines.adoption(m)
    if cmd == 'machine-prepare':
        return installers.prepare(m, c.get('kernels', c.option('KERNEL') or m.get('requested_kernel') or m.get('kernel')))
    if cmd in ('machine-start', 'machine-stop'):
        with c.lock('machine-' + m['name']), c.lock(c.connection_key(m)):
            return machines.vm_command(m['name'], m, cmd)
    if cmd == 'machine-ssh':
        os.execvpe(c.ssh_args(m)[0], c.ssh_args(m), dict(os.environ, SSHPASS=m.get('ssh_password', '')))
    if cmd == 'machine-remove':
        return machines.remove(m)
    if cmd == 'machine-destroy':
        return machines.destroy(m)
    if cmd in ('module-run', 'module-debug'):
        k = kernels.selected()
        return runs.prepare(m, k, cmd == 'module-debug')
    if cmd == 'server-run':
        return ui.server(m)
    if cmd == 'tui-run':
        return ui.tui(m)
    if cmd == 'vscode-open':
        return ui.vscode(m)
    if cmd == 'doctor':
        observed = machines.inspect(m)
        k = c.get('kernels', m['kernel']) if m.get('kernel') else None
        blockers = []
        if k:
            kernels.validate(k)
            cap = kernels.characteristics(k)
            blockers += cap['blockers']
            if machines.match(k, observed) == 'mismatch': blockers.append('kernel mismatch')
            if not shutil.which(shlex.split(k.get('toolchain', {}).get('cc', 'gcc'))[0], path=kernels.toolenv(k).get('PATH')): blockers.append('compiler missing')
            try:
                if kernels.path(k, 'build'):
                    __import__('kernel_compat').check(kernels.path(k, 'build'))
            except RuntimeError as error:
                blockers.append(str(error))
        else:
            blockers.append('no assigned kernel')
        if observed['modules_disabled'] == '1': blockers.append('module loading disabled')
        try:
            policy = c.ssh(m, 'id -u; cat /sys/module/module/parameters/sig_enforce 2>/dev/null || true', sudo=True).stdout.decode().splitlines()
            if not policy or policy[0] != '0': blockers.append('privileged module loading unavailable')
            if len(policy) > 1 and policy[1].strip().lower() in ('y', '1') and not (c.option('SIGNER') or (k and k.get('signer'))):
                blockers.append('trusted signature required; configure SIGNER')
        except subprocess.CalledProcessError:
            blockers.append('privileged module loading unavailable')
        print(json.dumps({'machine': m['name'], 'observed': observed, 'kernel': m.get('kernel'),
                          'capabilities': kernels.characteristics(k) if k else {}, 'blockers': blockers,
                          'debug_endpoint': m.get('gdb') or 'not configured'}, indent=2))
        # Do not connect to a GDB stub merely to check it: that can pause a VM.
        if blockers:
            c.fail('Resolve the listed blockers before development.')
        return
    c.fail('Unknown command ' + cmd)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print('mapple dev: ' + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else error.stderr, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)

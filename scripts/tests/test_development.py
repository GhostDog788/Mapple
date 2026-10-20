"""Development tool unit and contract tests; real VM coverage is in fresh_clone.py."""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import api_verify
import compile_commands
import go_toolchain
import kernel_compat
import lab
from mapple_dev import core as c, kernels, machines, installers, runs, ui, navigation

class UtilityTests(unittest.TestCase):
        def test_go_selection_handles_ubuntu_bootstrap_and_preserves_newer_toolchains(self):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                (root / 'user').mkdir()
                (root / 'user/go.mod').write_text('module example.test/mapple\n\ngo 1.26\n')
                go = root / 'go'
                for installed, expected in (('1.22.2', 'go1.26.0+auto'), ('1.26.2', 'auto')):
                    go.write_text('#!/bin/sh\n[ "$GOTOOLCHAIN" = local ] || exit 1\n'
                                  f'echo "go version go{installed} linux/amd64"\n')
                    go.chmod(0o755)
                    with self.subTest(installed=installed), \
                         patch.object(go_toolchain, 'ROOT', root), \
                         patch.dict(os.environ, {'PATH': str(root)}):
                        self.assertEqual(go_toolchain.select(), expected)
                        with patch.dict(os.environ, {'GOTOOLCHAIN': 'local'}):
                            self.assertEqual(go_toolchain.environment()['GOTOOLCHAIN'], 'local')

        def test_api_ignores_compression_only_and_detects_actual_drift(self):
            def source(raw, level, code='same code'):
                compressor = zlib.compressobj(level, wbits=-15)
                blob = base64.b64encode(compressor.compress(raw) + compressor.flush()).decode()
                return f'{code}\nvar swaggerSpec = []string{{\n"{blob}",\n}}\n'
            raw = b'{"openapi":"3.0.3","paths":{}}' * 100
            self.assertEqual(api_verify.canonical(source(raw, 1)), api_verify.canonical(source(raw, 9)))
            self.assertNotEqual(api_verify.canonical(source(raw, 1)), api_verify.canonical(source(raw+b' ', 1)))
            self.assertNotEqual(api_verify.canonical(source(raw, 1)), api_verify.canonical(source(raw, 1, 'changed code')))

        def test_source_only_kernel_gets_navigation_without_stale_or_other_arch_entries(self):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                for name in ('module/src/main.c', 'source/kernel/fork.c', 'source/arch/x86/kernel/setup.c',
                             'source/arch/arm/kernel/setup.c', 'source/tools/tool.c'):
                    p = root / name
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.touch()
                module = root / 'module.json'
                source = root / 'module/src/main.c'
                module.write_text(json.dumps([{'directory': str(root), 'file': str(source),
                    'arguments': ['gcc', '-D__KERNEL__', '-DMODULE', '-fconserve-stack', '-c', '-o', 'module.o', str(source)]}]))
                database = root / 'kernel.json'
                compile_commands.kernel_source(argparse.Namespace(database=database, module_database=module, source=root/'source'))
                result = json.loads(database.read_text())
                self.assertEqual({Path(e['file']).name for e in result}, {'fork.c', 'setup.c'})
                self.assertEqual(len(result), 2)
                self.assertNotIn('-DMODULE', result[0]['arguments'])
                self.assertNotIn('-fconserve-stack', result[0]['arguments'])
                self.assertIn('-D__KERNEL__', result[0]['arguments'])

class ModelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for obj, key, value in ((c, 'STATE', self.root / '.lab'), (c, 'ROOT', self.root)):
            patcher = patch.object(obj, key, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        clean_env = patch.dict(os.environ, {}, clear=True)
        clean_env.start()
        self.addCleanup(clean_env.stop)
        os.environ['PATH'] = '/usr/bin:/bin'

    def kernel(self, name='base', config='#define CONFIG_MODULES 1\n'):
        k = {'name': name, 'id': name + '-identity', 'arch': 'x86_64', 'linux_arch': 'x86',
             'release': '6.6-test', 'origin': 'imported', 'toolchain': {'cc': 'gcc'}, 'artifacts': {'build': 'build'}}
        tree = kernels.root(k) / 'build'
        for file, text in [('Makefile', ''), ('scripts/Makefile.build', ''), ('include/config/kernel.release', k['release']),
                           ('include/generated/autoconf.h', config), ('include/linux/fs.h', 'int test;')]:
            p = tree / file
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        return kernels.seal(k)

    def machine(self, name='dev', kernel='base'):
        m = {'name': name, 'id': name + '-id', 'host': 'localhost', 'user': 'test',
             'kernel': kernel, 'gdb': '127.0.0.1:1234', 'module_port': 4444}
        c.save(machines.slot_path(name), m)
        return m

    def test_version_named_slots_remain_safe_filesystem_identifiers(self):
        self.assertEqual(c.name('6.6.6-debug-v1'), '6.6.6-debug-v1')
        for unsafe in ('.', '..', '../escape', '/absolute', 'slot/name', '-option'):
            with self.assertRaises(RuntimeError):
                c.name(unsafe)

    def test_selection_does_not_build_contact_or_change_existing_runs(self):
        first, second = self.kernel(), self.kernel('variant')
        a, b = self.machine('a'), self.machine('b', kernel='variant')
        for kernel in (first, second):
            database = c.directory(c.STATE / 'builds/module' / kernel['id']) / 'compile_commands.json'
            source = kernels.path(kernel, 'build') / 'include/linux/fs.h'
            database.write_text(json.dumps([{'file': str(source), 'directory': str(source.parent),
                                            'arguments': ['gcc', '-c', str(source)]}]))
            navigation.capture(kernel, database)
        r = runs.new(a, first)
        before = c.load(c.record('runs', r['name']))
        with patch.object(c, 'ssh', side_effect=AssertionError('selection must be offline')), patch.object(runs, 'build', side_effect=AssertionError('no build')):
            machines.select('a')
            self.assertEqual(c.load(c.ROOT / 'compile_commands.json')[0]['file'], str(kernels.path(first, 'build') / 'include/linux/fs.h'))
            os.environ['MACHINE'] = 'b'
            self.assertEqual(c.machine()['name'], 'b')
            self.assertEqual(c.active(), 'a')
            self.assertEqual(kernels.selected()['id'], second['id'])
            machines.select('b')
            self.assertEqual(c.load(c.ROOT / 'compile_commands.json')[0]['file'], str(kernels.path(second, 'build') / 'include/linux/fs.h'))
        self.assertEqual(before, c.load(c.record('runs', r['name'])))
        with self.assertRaises(RuntimeError): machines.select('../escape')
        self.assertEqual(c.active(), 'b')

    def test_explicit_kernel_build_needs_no_machine(self):
        self.kernel()
        os.environ['KERNEL'] = 'base'
        self.assertEqual(kernels.selected()['name'], 'base')
        with self.assertRaisesRegex(RuntimeError, 'No active'): c.active()

    def test_incomplete_import_does_not_claim_a_successful_kernel_build(self):
        k = self.kernel(config='#define CONFIG_MODVERSIONS 1\n')
        self.assertEqual(k['capabilities']['tier'], 0)
        with self.assertRaisesRegex(RuntimeError, 'Tier 0'):
            kernels.build(k)

    def test_tiers_and_module_versions_are_separate_from_machine_policy(self):
        k = self.kernel(config='#define CONFIG_MODVERSIONS 1\n')
        self.assertEqual(k['capabilities']['tier'], 0)
        (kernels.path(k, 'build') / 'Module.symvers').write_text('0x1234\tfunction\tvmlinux\n')
        k = kernels.seal(k)
        self.assertEqual(k['capabilities']['tier'], 1)
        observed = {'arch': 'x86_64', 'release': k['release'], 'modules_disabled': '1'}
        self.assertEqual(machines.match(k, observed), 'release-only')
        k['expected_build_id'] = 'abc'
        self.assertEqual(machines.match(k, dict(observed, build_id='abc')), 'verified')
        self.assertEqual(machines.match(k, dict(observed, build_id='bad')), 'mismatch')
        self.assertEqual(k['capabilities']['tier'], 1)

    def test_sealed_inputs_detect_header_changes_and_deletions(self):
        k = self.kernel()
        kernels.validate(k)
        header = kernels.path(k, 'build') / 'include/linux/fs.h'
        header.write_text('different layout')
        with self.assertRaisesRegex(RuntimeError, 'changed'): kernels.validate(k)
        header.unlink()
        with self.assertRaisesRegex(RuntimeError, 'changed'): kernels.validate(k)

    def test_import_closes_header_symlinks_and_is_independent(self):
        source = self.root / 'external'
        (source / 'include').mkdir(parents=True)
        outside = self.root / 'common.h'
        outside.write_text('original ABI')
        (source / 'include/test.h').symlink_to(outside)
        (source / 'optional-rust').symlink_to(self.root / 'not-installed')
        destination = self.root / 'snapshot'
        c.copy_tree(source, destination)
        outside.write_text('changed ABI')
        self.assertFalse((destination / 'optional-rust').is_symlink())
        self.assertFalse((destination / 'include/test.h').is_symlink())
        self.assertEqual((destination / 'include/test.h').read_text(), 'original ABI')

    def test_catalog_refuses_replacement_and_cascade_removes_only_records(self):
        k = self.kernel()
        m = self.machine()
        machines.select('dev')
        disk = machines.slot_path('dev').parent / 'vm/disk.qcow2'
        disk.parent.mkdir()
        disk.write_bytes(b'precious disk')
        with self.assertRaisesRegex(RuntimeError, 'referenced'): kernels.remove(k)
        os.environ['CASCADE'] = '1'
        kernels.remove(k)
        self.assertFalse(machines.slot_path('dev').exists())
        self.assertFalse(c.record('kernels', 'base').exists())
        self.assertEqual(disk.read_bytes(), b'precious disk')
        self.assertFalse((c.STATE / 'active-machine').exists())

    def test_cascade_does_not_partially_remove_active_debug_machine(self):
        k = self.kernel()
        m = self.machine()
        r = runs.new(m, k)
        r['status'] = 'debugging'
        c.save(c.record('runs', r['name']), r)
        os.environ['CASCADE'] = '1'
        with self.assertRaisesRegex(RuntimeError, 'Detach'): kernels.remove(k)
        self.assertTrue(machines.slot_path('dev').exists())
        self.assertTrue(c.record('kernels', 'base').exists())

    def test_enrich_checks_build_id_and_release_without_replacing_old_artifacts(self):
        k = self.kernel()
        wrong = self.root / 'wrong'
        (wrong / 'include/config').mkdir(parents=True)
        (wrong / 'include/config/kernel.release').write_text('wrong')
        os.environ['BUILD'] = str(wrong)
        with self.assertRaisesRegex(RuntimeError, 'already present'): kernels.enrich(k)
        self.assertEqual(c.get('kernels', 'base')['release'], '6.6-test')

    def test_locks_reject_overlapping_mutation_and_release_on_failure(self):
        with c.lock('machine-dev'):
            with self.assertRaisesRegex(RuntimeError, 'busy'):
                with c.lock('machine-dev'): pass
        with c.lock('machine-dev'): pass

    def test_credentials_are_private_and_show_redacts_them(self):
        m = self.machine()
        m['ssh_password'] = "a' b;$(touch never-created)\nsecond line"
        c.save(machines.slot_path('dev'), m)
        self.assertEqual(machines.slot_path('dev').stat().st_mode & 0o777, 0o600)
        self.assertEqual(lab.public(m)['ssh_password'], '<redacted>')
        with patch.object(c, 'run') as run:
            c.ssh(m, 'uname -r')
        args = run.call_args.args
        self.assertNotIn(m['ssh_password'], str(args))
        self.assertEqual(run.call_args.kwargs['env']['SSHPASS'], m['ssh_password'])
        self.assertFalse((self.root / 'never-created').exists())

    def test_prepare_failure_preserves_association_and_records_transition(self):
        k = self.kernel('new')
        k['capabilities']['installable'] = True
        m = self.machine(kernel='old')
        observed = {'arch': 'x86_64', 'release': 'old', 'boot_id': 'boot', 'cmdline': '', 'os_release': 'ID=ubuntu'}
        with patch.object(kernels, 'validate'), patch.object(kernels, 'characteristics', return_value={'installable': True, 'build_id': 'new-build'}), \
             patch.object(machines, 'inspect', return_value=observed), \
             patch.object(installers, 'install', side_effect=RuntimeError('installation failed')):
            with self.assertRaisesRegex(RuntimeError, 'installation failed'): installers.prepare(m, k)
        saved = c.get('machines', 'dev')
        self.assertEqual(saved['kernel'], 'old')
        self.assertEqual(saved['transition']['status'], 'failed')

    def test_clean_preserves_snapshots_records_disks_and_frozen_runs(self):
        k = self.kernel()
        m = self.machine()
        r = runs.new(m, k)
        frozen = runs.root(r) / 'mapple.ko'
        frozen.write_text('frozen artifact')
        output = c.directory(c.STATE / 'builds/module' / k['id']) / 'test.o'
        output.touch()
        orphan = c.directory(c.STATE / 'builds/module/removed-kernel-id') / 'mapple.ko'
        orphan.write_bytes(b'orphaned build output')
        lab.main(['clean'])
        self.assertFalse(output.exists())
        self.assertFalse(orphan.parent.exists())
        self.assertTrue(frozen.exists())
        kernels.validate(k)
        self.assertTrue(machines.slot_path('dev').exists())

    def test_grub_fallback_and_nested_selection(self):
        entry = "menuentry 'Ubuntu, with Linux 6.6-test' --class ubuntu $menuentry_id_option 'kernel-id' {\n"
        self.assertEqual(installers.grub_entry(entry, '6.6-test'), 'kernel-id')
        nested = "submenu 'Advanced options for Ubuntu' $menuentry_id_option 'submenu-id' {\n\t" + entry
        self.assertEqual(installers.grub_entry(nested, '6.6-test'), 'submenu-id>kernel-id')
        with self.assertRaises(RuntimeError): installers.grub_entry(entry, 'wrong')

    def test_verified_recovery_updates_assignment_and_editor_without_reinstall(self):
        k = self.kernel()
        k['expected_build_id'] = 'exact-build'
        k = kernels.seal(k)
        m = self.machine(kernel=None)
        m['transition'] = {'to': k['name'], 'status': 'failed'}
        c.save(machines.slot_path(m['name']), m)
        machines.select(m['name'])
        database = c.directory(c.STATE / 'builds/module' / k['id']) / 'compile_commands.json'
        database.write_text('[]')
        navigation.capture(k, database)
        with patch.object(machines, 'inspect', return_value={'arch': 'x86_64', 'release': k['release'], 'build_id': 'exact-build', 'cmdline': 'nokaslr'}), patch.object(installers, 'install') as install:
            installers.prepare(m, k)
            install.assert_not_called()
        saved = c.get('machines', m['name'])
        self.assertEqual(saved['kernel'], k['name'])
        self.assertEqual(saved['transition']['status'], 'complete')
        self.assertTrue(saved['transition']['recovered'])
        self.assertEqual(c.load(c.ROOT / 'compile_commands.json'), [])

    def test_gnu_notes_extract_exact_kernel_identity(self):
        import struct
        raw = struct.pack('<III', 4, 4, 3) + b'GNU\0' + bytes.fromhex('deadbeef')
        self.assertEqual(kernels.notes_id(raw), 'deadbeef')
        self.assertIsNone(kernels.notes_id(b'not an ELF note'))

    def test_tier0_adoption_retains_machine_without_fabricating_build_support(self):
        m = self.machine(kernel=None)
        observation = {'release': 'vendor', 'arch': 'aarch64', 'build_id': 'known'}
        k = machines.observed_kernel(m, observation)
        self.assertEqual(k['capabilities']['tier'], 0)
        self.assertEqual(k['expected_build_id'], 'known')
        self.assertEqual(k['arch'], 'aarch64')

    def test_inspection_does_not_publish_an_unprotected_kernel_reference(self):
        k = self.kernel()
        k['expected_build_id'] = 'known'
        kernels.seal(k)
        self.machine(kernel=None)
        os.environ['MACHINE'] = 'dev'
        observed = {'release': k['release'], 'arch': k['arch'], 'build_id': 'known'}
        with patch.object(machines, 'inspect', return_value=observed):
            with c.lock('catalog'):
                with self.assertRaisesRegex(RuntimeError, 'catalog is busy'):
                    lab.main(['machine-inspect'])
            self.assertIsNone(c.get('machines', 'dev')['kernel'])
            lab.main(['machine-inspect'])
        self.assertEqual(c.get('machines', 'dev')['kernel'], k['name'])

    def test_minimum_debug_mode_fails_before_build_or_upload(self):
        k = self.kernel()
        m = self.machine()
        observation = {'release': k['release'], 'arch': 'x86_64', 'boot_id': 'boot', 'modules_disabled': '0'}
        os.environ['MIN_DEBUG'] = 'kernel-source'
        with patch.object(machines, 'inspect', return_value=observation), patch.object(runs, 'check_endpoint'), patch.object(runs, 'build') as build, patch.object(c, 'send') as send:
            with self.assertRaisesRegex(RuntimeError, 'tier 1'): runs.prepare(m, k, True)
            build.assert_not_called()
            send.assert_not_called()

    def test_known_kernel_identity_is_reused_without_changing_target(self):
        k = self.kernel()
        k['expected_build_id'] = 'abc123'
        kernels.seal(k)
        m = self.machine(kernel=None)
        with patch.object(c, 'ssh', side_effect=AssertionError('read-only recognition')):
            found = machines.observed_kernel(m, {'release': k['release'], 'arch': k['arch'], 'build_id': 'abc123'})
        self.assertEqual(found['id'], k['id'])
        self.assertFalse(c.record('kernels', 'dev-observed').exists())

    def test_signer_failure_prevents_upload_and_module_reload(self):
        k = self.kernel()
        k['signer'] = '/bin/false'
        m = self.machine()
        m['gdb'] = ''
        observed = {'release': k['release'], 'arch': k['arch'], 'boot_id': 'boot', 'modules_disabled': '0'}
        def fake_build(kernel, **kwargs):
            Path(kwargs['freeze']).write_bytes(b'unsigned module')
        with patch.object(machines, 'inspect', return_value=observed), patch.object(runs, 'build', side_effect=fake_build), patch.object(c, 'send') as send, patch.object(c, 'ssh') as ssh:
            with self.assertRaises(subprocess.CalledProcessError):
                runs.prepare(m, k)
            send.assert_not_called()
            ssh.assert_not_called()
        record = c.records('runs')[0]
        self.assertEqual(record['status'], 'failed')

    def test_text_symbols_resolve_kaslr_without_a_linux_elf(self):
        k = self.kernel()
        symbol_map = kernels.root(k) / 'System.map'
        symbol_map.write_text('ffffffff81000000 T _text\nffffffff81000020 T known_function\n')
        k['artifacts']['system_map'] = 'System.map'
        k = kernels.seal(k)
        self.assertIsNone(kernels.debug_elf(k))
        runtime = b'ffffffff83000000 T _text\n'
        with patch.object(c, 'ssh', return_value=subprocess.CompletedProcess([], 0, runtime)):
            self.assertEqual(runs.kernel_offset(self.machine(), k, {'cmdline': ''}), 0x2000000)

    def test_detached_debug_run_rejects_a_different_load_receipt(self):
        k = self.kernel()
        m = self.machine()
        r = runs.new(m, k)
        r.update(status='detached', boot_id='boot', load_receipt='original receipt', sections={'.text': '0xffff1234'})
        with patch.object(machines, 'inspect', return_value={'boot_id': 'boot'}), patch.object(runs, 'check_endpoint', return_value='endpoint-test'), patch.object(c, 'ssh', return_value=subprocess.CompletedProcess([], 0, b'new receipt')), patch.object(subprocess, 'Popen') as spawn:
            with self.assertRaisesRegex(RuntimeError, 'Another run'):
                runs.attach(r)
            spawn.assert_not_called()

    def test_machine_override_build_does_not_publish_editor_navigation(self):
        k = self.kernel()
        self.machine()
        os.environ.update(MACHINE='dev', KERNEL='base')
        with patch.object(runs, 'build') as build:
            lab.main(['module-build'])
        self.assertFalse(build.call_args.kwargs['publish'])

    def test_cross_arch_source_navigation_does_not_index_x86(self):
        tree = self.root / 'linux'
        for f in ('init/main.c', 'arch/arm64/kernel/setup.c', 'arch/x86/kernel/setup.c'):
            p = tree / f
            p.parent.mkdir(parents=True, exist_ok=True)
            p.touch()
        module = self.root / 'module.json'
        src = self.root / 'module/src/main.c'
        src.parent.mkdir(parents=True)
        src.touch()
        module.write_text(json.dumps([{'file': str(src), 'directory': str(self.root), 'arguments': ['aarch64-linux-gnu-gcc', '-c', str(src)]}]))
        database = self.root / 'linux.json'
        compile_commands.kernel_source(argparse.Namespace(database=database, module_database=module, source=tree, arch='arm64'))
        entries = json.loads(database.read_text())
        self.assertTrue(any('/arch/arm64/' in e['file'] for e in entries))
        self.assertFalse(any('/arch/x86/' in e['file'] for e in entries))

    def test_vscode_workspace_freezes_machine_and_kernel_selection(self):
        k = self.kernel()
        m = self.machine('first')
        self.machine('second')
        os.environ['NO_OPEN'] = '1'
        workspace = ui.vscode(m)
        machines.select('second')
        data = c.load(workspace)
        launch = data['launch']['configurations'][0]
        run = c.load(workspace.parent / 'record.json')
        self.assertEqual(run['machine'], 'first')
        self.assertEqual(run['kernel_id'], k['id'])
        self.assertIn(run['name'], data['tasks']['tasks'][0]['args'])
        self.assertIn('gdb-wrapper', launch['miDebuggerPath'])

    @unittest.skipUnless(shutil.which('gcc') and shutil.which('gdb'), 'native ELF tools required')
    def test_module_only_debug_uses_real_elf_source_mappings_without_vmlinux(self):
        k = self.kernel()
        m = self.machine()
        r = runs.new(m, k)
        source = self.root / 'module.c'
        source.write_text('int mapple_fixture(void) { return 42; }\n')
        c.run(['gcc', '-g', '-c', source, '-o', runs.root(r) / 'mapple.ko'])
        r.update(debug_mode='module', sections={'.text': '0xffffffffc0100000'}, kernel_offset=None)
        runs.debug_script(r, k)
        script = (runs.root(r) / 'debug.gdb').read_text()
        offline = runs.root(r) / 'offline.gdb'
        offline.write_text('\n'.join(line for line in script.splitlines() if not line.startswith('target remote')) + '\n')
        result = c.run(['gdb', '-q', '-batch', '-x', offline, '-ex', 'info line mapple_fixture', '-ex', 'break mapple_fixture', '-ex', 'info breakpoints'], capture_output=True)
        self.assertIn(b'0xffffffffc010', result.stdout)
        self.assertIn(b'module.c', result.stdout)
        self.assertNotIn('vmlinux', script)
        self.assertNotIn(b'2 locations', result.stdout)
        self.assertIn(b'Breakpoint 1 at 0xffffffffc010', result.stdout)

    def test_shared_build_lock_waits_then_succeeds(self):
        import threading
        entered = threading.Event()
        def owner():
            with c.lock('shared-build'):
                entered.set()
                threading.Event().wait(.15)
        thread = threading.Thread(target=owner)
        thread.start()
        self.assertTrue(entered.wait(2))
        with c.lock('shared-build', wait=2):
            pass
        thread.join(2)
        self.assertFalse(thread.is_alive())

    def test_busy_second_machine_does_not_partially_apply_cascade(self):
        k = self.kernel()
        self.machine('a')
        self.machine('b')
        os.environ['CASCADE'] = '1'
        with c.lock('machine-b'):
            with self.assertRaisesRegex(RuntimeError, 'busy'):
                kernels.remove(k)
        self.assertTrue(c.record('machines', 'a').exists())
        self.assertTrue(c.record('machines', 'b').exists())

    def test_imported_build_rules_are_also_immutable(self):
        k = self.kernel()
        (kernels.path(k, 'build') / 'scripts/Makefile.build').write_text('different compiler flags')
        with self.assertRaisesRegex(RuntimeError, 'changed'):
            kernels.validate(k)

    def test_sdk_configuration_is_copied_and_used_to_find_compiler(self):
        sdk = self.root / 'sdk'
        sdk.mkdir()
        compiler = sdk / 'vendor-cc'
        compiler.write_text('#!/bin/sh\nexit 0\n')
        compiler.chmod(0o700)
        config = self.root / 'toolchain.json'
        config.write_text(json.dumps({'cc': 'vendor-cc', 'cross_compile': 'vendor-', 'env': {'PATH': str(sdk)}}))
        os.environ['TOOLCHAIN'] = str(config)
        data = kernels.toolchain('aarch64')
        self.assertEqual(c.executable(data['cc'], {'PATH': data['env']['PATH']}), str(compiler))
        config.write_text('{}')
        self.assertEqual(data['cc'], 'vendor-cc')

    def test_unfamiliar_architecture_has_explicit_recipe(self):
        config = self.root / 'mips.config'
        config.write_text('CONFIG_MODULES=y\n')
        os.environ.update(NAME='router', ARCH='mipsel', LINUX_ARCH='mips', CONFIG=str(config),
                          IMAGE_PATH='vmlinux', CC='mipsel-linux-gnu-gcc', CROSS_COMPILE='mipsel-linux-gnu-')
        kernels.create()
        k = c.get('kernels', 'router')
        self.assertEqual(k['linux_arch'], 'mips')
        self.assertEqual(k['recipe']['image_path'], 'vmlinux')
        config.write_text('changed')
        self.assertEqual((kernels.root(k) / k['recipe']['config']).read_text(), 'CONFIG_MODULES=y\n')

    def test_llvm_recipe_selects_clang_without_accidentally_forcing_gcc(self):
        os.environ['LLVM'] = '1'
        self.assertEqual(kernels.toolchain('aarch64')['cc'], 'clang')

    @unittest.skipUnless(shutil.which('aarch64-linux-gnu-gcc'), 'cross compiler required')
    def test_foreign_elf_architecture_is_detected_and_checked(self):
        file = self.root / 'foreign.c'
        file.write_text('int function(void) { return 42; }\n')
        elf = self.root / 'foreign.o'
        c.run(['aarch64-linux-gnu-gcc', '-g', '-c', file, '-o', elf])
        info = kernels.elf_info(elf)
        self.assertIn('AArch64', info['machine'])
        self.assertTrue(info['dwarf'])
        k = self.kernel()
        destination = kernels.root(k) / 'foreign.o'
        shutil.copyfile(elf, destination)
        k['artifacts']['vmlinux'] = 'foreign.o'
        with self.assertRaisesRegex(RuntimeError, 'architecture'):
            kernels.seal(k)

    def test_out_of_tree_build_import_closes_parent_source_cycle(self):
        source = self.root / 'linux'
        build = source / 'output'
        build.mkdir(parents=True)
        (source / 'Makefile').write_text('kernel source')
        (source / 'include').mkdir()
        (source / 'include/header.h').write_text('ABI')
        (build / 'source').symlink_to(source)
        (build / 'Makefile').write_text('include ' + str(source / 'Makefile'))
        destination = self.root / 'owned'
        mappings = c.copy_tree(build, destination)
        mapped_source = (destination / 'source').resolve()
        self.assertTrue(mapped_source.is_relative_to(destination))
        self.assertEqual((mapped_source / 'include/header.h').read_text(), 'ABI')
        self.assertEqual((mapped_source / 'output').resolve(), destination)
        self.assertIn(str(mapped_source / 'Makefile'), (destination / 'Makefile').read_text())
        self.assertEqual(mappings[source], mapped_source)
        shutil.rmtree(source)
        self.assertEqual((mapped_source / 'include/header.h').read_text(), 'ABI')

    @unittest.skipUnless(shutil.which('gcc') and shutil.which('gdb'), 'ELF tools required')
    @unittest.skipUnless(shutil.which('gcc') and shutil.which('objcopy') and shutil.which('gdb'), 'native ELF tools required')
    def test_symbol_only_debug_initializes_architecture_before_loading_linux(self):
        k = self.kernel()
        symbol_map = kernels.root(k) / 'System.map'
        symbol_map.write_text('ffffffff81000000 T named_kernel_function\n')
        k['artifacts']['system_map'] = 'System.map'
        k = kernels.seal(k)
        r = runs.new(self.machine(), k)
        source = self.root / 'module.c'
        source.write_text('int module_function(void) { return 1; }\n')
        subprocess.run(['gcc', '-c', '-g', str(source), '-o', str(runs.root(r) / 'mapple.ko')], check=True)
        r.update(debug_mode='kernel-symbol', kernel_offset=0, sections={'.text': '0xffffffffa0000000'})
        runs.debug_script(r, k)
        setup = (runs.root(r) / 'debug.gdb').read_text().split('target remote')[0]
        offline = self.root / 'offline.gdb'
        offline.write_text(setup + '\nshow architecture\ninfo address named_kernel_function\n')
        output = subprocess.run(['gdb', '-q', '-batch', '-x', str(offline)], capture_output=True, text=True, check=True).stdout
        self.assertIn('i386:x86-64', output)
        self.assertIn('0xffffffff81000000', output)

    def test_text_map_preserves_high_kernel_addresses_and_function_ranges(self):
        k = self.kernel()
        table = kernels.root(k) / 'System.map'
        table.write_text('ffffffff81012340 T kernel_sendmsg\nffffffff81012400 t other_function\n')
        k['artifacts']['system_map'] = 'System.map'
        directory = c.directory(self.root / 'run')
        source = directory / 'module.c'
        source.write_text('int own_function(void) { return 0; }\n')
        c.run(['gcc', '-g', '-c', source, '-o', directory / 'mapple.ko'])
        output = directory / 'symbols.elf'
        runs.make_symbol_elf(k, output, 0)
        result = c.run(['gdb', '-q', '-batch', '-ex', 'symbol-file ' + str(output),
                        '-ex', 'info symbol 0xffffffff81012341', '-ex', 'break kernel_sendmsg'], capture_output=True)
        self.assertIn(b'kernel_sendmsg + 1', result.stdout)
        self.assertIn(b'Breakpoint 1 at 0xffffffff81012340', result.stdout)


if __name__ == '__main__':
    unittest.main()

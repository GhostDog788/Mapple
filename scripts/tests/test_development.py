"""Development workflow regressions; all VM/SSH mutations use isolated fixtures."""
import argparse
import base64
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import api_verify
import compile_commands
import lab
import debug_kernel
import kernel_compat

REPO = Path(__file__).resolve().parents[2]


class LabTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.state = self.root / '.lab'
        self.patches = [patch.object(lab, 'ROOT', self.root), patch.object(lab, 'STATE', self.state)]
        for p in self.patches:
            p.start()
        self.addCleanup(self.temp.cleanup)
        for p in self.patches:
            self.addCleanup(p.stop)

    def slot(self, name, **extra):
        config = dict(host='127.0.0.1', user='mapple', ssh_port=2222, **extra)
        lab.save(lab.slot_path(name), config)
        return config

    def test_selection_persists_and_invalidates_previous_debug_symbols(self):
        self.slot('first')
        self.slot('second')
        lab.select('first')
        stale = self.root / 'kernel/.gdb/current-debug.gdb'
        stale.parent.mkdir(parents=True)
        stale.write_text('wrong VM symbols')
        lab.select('second')
        self.assertEqual(lab.load_slot()[0], 'second')
        self.assertEqual((self.root / 'kernel/kernel-cache/current').readlink(), Path('second'))
        self.assertFalse(stale.exists())
        with self.assertRaises(RuntimeError):
            lab.select('../escape')
        self.assertEqual(lab.load_slot()[0], 'second')

    def test_credentials_are_private_and_roundtrip_without_shell_execution(self):
        password = "a' b;$(touch never-created)\nsecond line"
        config = self.slot('dev', ssh_password=password, ssh_key='/tmp/key with spaces')
        env = lab.bridge('dev', config)
        path = Path(env['MAPPLE_LAB_ENV'])
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        result = subprocess.check_output(['bash', '-c', 'source "$1"; printf "%s" "${TARGET_SSH_PASS[dev]}"',
                                          'bash', str(path)], cwd=self.root)
        self.assertEqual(result.decode(), password)
        self.assertFalse((self.root / 'never-created').exists())

    def test_new_slot_does_not_change_active_vm(self):
        self.slot('first')
        lab.select('first')
        with patch.dict(os.environ, {'NAME': 'second', 'HOST': 'localhost', 'SSH_USER': 'mapple', 'ASK_PASS': ''}), \
             patch.object(lab, 'dev_config'):
            lab.add_slot()
        self.assertEqual(lab.load_slot()[0], 'first')
        self.assertEqual(lab.load_slot('second')[1]['host'], 'localhost')

    def test_legacy_import_preserves_selection_and_does_not_print_secrets(self):
        (self.root / 'lab.local.env').write_text("TARGETS=(old)\ndeclare -A TARGET_IP=([old]=localhost)\n"
            "declare -A TARGET_SSH_USER=([old]=ubuntu)\ndeclare -A TARGET_SSH_PASS=([old]='secret value')\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            lab.import_legacy()
        self.assertNotIn('secret value', output.getvalue())
        self.assertEqual(lab.load_slot()[1]['ssh_password'], 'secret value')
        self.slot('other')
        lab.select('other')
        lab.import_legacy()
        self.assertEqual(lab.load_slot()[0], 'other')

    @unittest.skipUnless(shutil.which('openssl'), 'openssl required')
    def test_dev_setup_is_idempotent_and_server_tui_credentials_match(self):
        self.slot('dev', module_port=14444)
        server, tui = lab.dev_config()
        before = {p.name: p.read_bytes() for p in server.parent.iterdir()}
        lab.dev_config()
        self.assertEqual(before, {p.name: p.read_bytes() for p in server.parent.iterdir()})
        s, t = json.loads(server.read_text()), json.loads(tui.read_text())
        import hashlib
        self.assertEqual(s['identities'][0]['token_sha256'], hashlib.sha256(t['profiles']['dev']['bearer_token'].encode()).hexdigest())
        self.assertEqual(t['module_profiles']['dev']['port'], 14444)
        self.assertTrue(all(p.stat().st_mode & 0o077 == 0 for p in server.parent.iterdir() if p.suffix != '.crt'))

    @unittest.skipUnless(all(shutil.which(t) for t in ('cloud-localds', 'qemu-img', 'qemu-system-x86_64', 'ssh-keygen')), 'VM creation tools required')
    def test_vm_creation_verifies_image_and_creates_real_overlay_and_seed(self):
        import hashlib
        base = self.root / 'base.qcow2'
        subprocess.run(['qemu-img', 'create', '-f', 'qcow2', str(base), '64M'], check=True, stdout=subprocess.DEVNULL)
        payload = base.read_bytes()
        checksum = hashlib.sha256(payload).hexdigest()
        def download(url, **_):
            data = f'{checksum} *{lab.IMAGE_NAME}\n'.encode() if url.endswith('SHA256SUMS') else payload
            return io.BytesIO(data)
        with patch.object(lab.urllib.request, 'urlopen', side_effect=download), \
             patch.dict(os.environ, {'NAME': 'created', 'MEMORY': '128', 'CPUS': '1', 'DISK': '128M'}):
            lab.create_vm()
        name, slot = lab.load_slot()
        self.assertEqual(name, 'created')
        self.assertEqual(slot['image_sha256'], checksum)
        self.assertNotEqual(slot['ssh_port'], slot['module_port'])
        vm = self.state / 'vms/created'
        chain = json.loads(subprocess.check_output(['qemu-img', 'info', '--backing-chain', '--output=json', str(vm/'disk.qcow2')]))
        self.assertEqual(len(chain), 2)
        self.assertEqual(Path(chain[1]['filename']).name, checksum + '.qcow2')
        self.assertTrue((vm/'seed.img').stat().st_size > 0)
        user = json.loads((vm/'user-data').read_text().split('\n', 1)[1])['users'][0]
        self.assertEqual(user['ssh_authorized_keys'], [(vm/'id_ed25519.pub').read_text().strip()])

    def test_vm_image_checksum_failure_does_not_register_a_slot(self):
        def download(url, **_):
            return io.BytesIO((b'0'*64 + b' *' + lab.IMAGE_NAME.encode() + b'\n') if url.endswith('SHA256SUMS') else b'corrupt image')
        with patch.object(lab.urllib.request, 'urlopen', side_effect=download), \
             patch.object(lab.shutil, 'which', return_value='/available'), \
             patch.dict(os.environ, {'NAME': 'bad'}):
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                lab.create_vm()
        self.assertFalse(lab.slot_path('bad').exists())


class GenerationTests(unittest.TestCase):
    def test_kernel_api_guard_accepts_old_signature_and_rejects_new_one(self):
        with tempfile.TemporaryDirectory() as d:
            headers = Path(d)
            file = headers / 'include/linux/fs.h'
            file.parent.mkdir(parents=True)
            file.write_text('static inline bool get_file_rcu(struct file *file);')
            kernel_compat.check(headers)
            file.write_text('struct file *get_file_rcu(struct file __rcu **f);')
            with self.assertRaisesRegex(RuntimeError, 'make vm-setup'):
                kernel_compat.check(headers)

    def test_guest_kernel_selection_handles_grub_with_and_without_submenu(self):
        entry = "menuentry 'Ubuntu, with Linux 6.6.6-mapple' --class ubuntu $menuentry_id_option 'kernel-id' {\n"
        self.assertEqual(debug_kernel.grub_selection(entry, '6.6.6-mapple'), 'kernel-id')
        nested = "submenu 'Advanced options for Ubuntu' $menuentry_id_option 'submenu-id' {\n\t" + entry
        self.assertEqual(debug_kernel.grub_selection(nested, '6.6.6-mapple'), 'submenu-id>kernel-id')
        with self.assertRaisesRegex(RuntimeError, 'missing from GRUB'):
            debug_kernel.grub_selection(entry, 'uninstalled')

    @unittest.skipUnless(shutil.which('make') and shutil.which('tar'), 'make/tar required')
    def test_kernel_package_preserves_merged_usr_symlinks(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / 'source'
            (source / 'include/config').mkdir(parents=True)
            (source / 'include/config/kernel.release').write_text('fixture')
            for name in ['.config', 'Module.symvers', 'System.map', 'arch/x86/boot/bzImage']:
                p = source / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.touch()
            (source / 'Makefile').write_text('modules_install:\n\tmkdir -p $(INSTALL_MOD_PATH)/lib/modules/fixture\n\ttouch $(INSTALL_MOD_PATH)/lib/modules/fixture/test.ko\n')
            def execute(argv, **kwargs):
                if argv[0] != 'rsync':
                    subprocess.run([str(a) for a in argv], check=True, stdout=subprocess.DEVNULL)
            fake = SimpleNamespace(STATE=root, run=execute, ssh=lambda *args, **kw: None,
                                   ssh_args=lambda c: ['ssh', 'fixture@localhost'])
            debug_kernel.install(fake, 'dev', {}, source)
            archive = next((root / 'kernel-install').glob('*/modules.tar.gz'))
            destination = root / 'guest'
            (destination / 'usr/lib/modules').mkdir(parents=True)
            (destination / 'lib').symlink_to('usr/lib')
            subprocess.run(['tar', '-xzf', str(archive), '-C', str(destination/'lib/modules')], check=True)
            self.assertTrue((destination / 'lib').is_symlink())
            self.assertTrue((destination / 'usr/lib/modules/fixture/test.ko').exists())

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


@unittest.skipUnless(all(shutil.which(t) for t in ('gcc', 'objcopy', 'gdb', 'readelf')), 'ELF/debug tools required')
class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.kernel = self.root / 'kernel'
        self.scripts = self.kernel / 'scripts'
        (self.scripts / 'lib').mkdir(parents=True)
        self.script = self.scripts / '04-deploy-and-prep-gdb.sh'
        shutil.copy(REPO / 'kernel/scripts/04-deploy-and-prep-gdb.sh', self.script)
        cache = self.kernel / 'kernel-cache/dev'
        cache.mkdir(parents=True)
        (cache / 'kernel.release').write_text('fixture\n')
        self.artifact = self.kernel / 'build/artifacts/dev/fixture/mapple.ko'
        self.artifact.parent.mkdir(parents=True)
        source = cache / 'source/init/main.c'
        source.parent.mkdir(parents=True)
        source.write_text('int start_kernel(void) { return 1; }\nint mapple_fixture(void) { return 42; }\n')
        subprocess.run(['gcc', '-g', '-c', str(source), '-o', str(self.artifact)], check=True)
        shutil.copy(self.artifact, cache / 'vmlinux')
        (self.root / 'lab.local.env').touch()
        common = r'''
+die() { echo "$*" >&2; exit 1; }
+validate_target() { :; }
+lab_env_file() { printf '%s/lab.local.env' "$1"; }
+lab_load_target() { LAB_SSH_TARGET=fixture; LAB_REMOTE_DIR=/tmp/fixture; }
+target_require_cfg() { printf 127.0.0.1:9; }
+lab_check_connection() { :; }
+lab_scp_to() { :; }
+lab_ssh() {
+    case "$*" in
+        'uname -r') echo fixture ;;
+        'cat /proc/cmdline') echo "${FIXTURE_CMDLINE:-nokaslr}" ;;
+        'stat -c %s '*) stat -c %s "$FIXTURE_ARTIFACT" ;;
+    esac
+    return 0
+}
+lab_ssh_sudo() {
+    case "$*" in
+        *sections*) printf '.text 0xffffffffc0100000\n.data 0xffffffffc0200000\n' ;;
+    esac
+    return 0
+}
+'''.replace('\n+', '\n')
        (self.scripts / 'lib/common.sh').write_text(common)
        self.env = dict(os.environ, FIXTURE_ARTIFACT=str(self.artifact))

    def deploy(self, mode, **env):
        return subprocess.run(['bash', str(self.script), 'dev', mode], env=dict(self.env, **env),
                              capture_output=True, text=True)

    def test_debug_upload_relocates_real_elf_and_keeps_source_breakpoints(self):
        result = self.deploy('qemu')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        gdb = self.kernel / '.gdb/current-debug.gdb'
        text = gdb.read_text()
        self.assertIn('target remote 127.0.0.1:9', text)
        self.assertIn('set substitute-path', text)
        self.assertIn('set schedule-multiple on', text)
        symbols = self.kernel / '.gdb/dev-qemu-module-symbols.gdb'
        # Execute the generated init too: GDB's `source` treats surrounding
        # quotes as literal filename characters, unlike add-symbol-file.
        offline = self.kernel / '.gdb/offline.gdb'
        offline.write_text('\n'.join(line for line in text.splitlines()
                                     if not line.startswith(('target remote ', 'symbol-file '))) + '\n')
        check = subprocess.run(['gdb', '-q', '-batch', '-ex', f'source {offline}',
                                '-ex', 'info line mapple_fixture'], capture_output=True, text=True, check=True)
        self.assertIn('starts at address 0xffffffffc010', check.stdout)
        self.assertIn('main.c', check.stdout)
        self.assertTrue((self.kernel / '.gdb/current-vmlinux').exists())

    def test_normal_upload_needs_no_symbols_and_invalidates_old_debug_run(self):
        self.assertEqual(self.deploy('qemu').returncode, 0)
        (self.kernel / 'kernel-cache/dev/vmlinux').unlink()
        result = self.deploy('none')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.kernel / '.gdb/current-debug.gdb').exists())

    def test_debug_rejects_aslr_before_upload(self):
        result = self.deploy('qemu', FIXTURE_CMDLINE='console=ttyS0')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nokaslr', result.stderr)
        self.assertNotIn('uploading', result.stdout)


if __name__ == '__main__':
    unittest.main()

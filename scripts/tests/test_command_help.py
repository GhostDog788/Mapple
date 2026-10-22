"""Exercise public Make help without running builds, installers or removal."""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lab
from mapple_dev import command_help, core as c, kernels, machines, runs, ui


ROOT = Path(__file__).resolve().parents[2]


class CommandHelpTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.state = Path(temp.name) / 'lab'
        env = patch.dict(os.environ, {'PATH': '/usr/bin:/bin', 'GOTOOLCHAIN': 'local',
                                     'MAPPLE_LAB_DIR': str(self.state)}, clear=True)
        env.start()
        self.addCleanup(env.stop)
        state = patch.object(c, 'STATE', self.state)
        state.start()
        self.addCleanup(state.stop)

    def make(self, *args):
        return subprocess.run(['make', '--no-print-directory', *args], cwd=ROOT,
                              capture_output=True, text=True, timeout=15)

    def seed_selection(self):
        c.save(c.record('kernels', 'base'), {'name': 'base', 'id': 'kernel-id'})
        c.save(c.record('machines', 'dev'), {'name': 'dev', 'kernel': 'base'})
        (self.state / 'active-machine').write_text('dev\n')
        return {p.relative_to(self.state): p.read_bytes() for p in self.state.rglob('*') if p.is_file()}

    def test_every_public_make_target_has_help_without_dependencies_or_state(self):
        # Make itself lists its targets; this catches new public targets missing help.
        targets = subprocess.run(['make', '-qp'], cwd=ROOT, env=dict(os.environ, HELP='1'),
                                 capture_output=True, text=True, timeout=15)
        phony = next(line for line in targets.stdout.splitlines() if line.startswith('.PHONY:'))
        public = set(phony.split()[1:])
        self.assertEqual(public, set(command_help.COMMANDS))
        self.assertEqual(set(lab.COMMANDS) - {'vscode-prepare', 'vscode-finish'},
                         public - {'help', 'check', 'test', 'test-tools', 'test-navigation', 'api', 'api-verify'})
        for name in sorted(public):
            with self.subTest(command=name):
                result = self.make(name, 'HELP=1')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('Learn a command:' if name == 'help' else f'Usage: make {name}', result.stdout)
                self.assertEqual(result.stderr, '')
        self.assertFalse(self.state.exists(), 'Help must not create lab state')

    def test_bare_destructive_commands_only_show_help_even_with_active_selection(self):
        before = self.seed_selection()
        for command in ('kernel-remove', 'machine-remove', 'machine-destroy'):
            result = self.make(command, 'CASCADE=1')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Required:', result.stdout)
            self.assertIn(f'Usage: make {command}', result.stdout)
        after = {p.relative_to(self.state): p.read_bytes() for p in self.state.rglob('*') if p.is_file()}
        self.assertEqual(before, after)

    def test_help_with_complete_arguments_never_calls_operations(self):
        self.seed_selection()
        with patch.dict(os.environ, {'HELP': '1', 'KERNEL': 'base', 'MACHINE': 'dev', 'NAME': 'new'}), \
             patch.object(kernels, 'remove', side_effect=AssertionError('removed kernel')), \
             patch.object(machines, 'destroy', side_effect=AssertionError('destroyed VM')), \
             patch.object(c, 'run', side_effect=AssertionError('ran external command')), \
             patch.object(c, 'ssh', side_effect=AssertionError('contacted machine')), \
             patch.object(ui, 'setup', side_effect=AssertionError('installed tools')), \
             patch.object(runs, 'build', side_effect=AssertionError('built module')):
            for command in set(lab.COMMANDS) - {'vscode-prepare', 'vscode-finish'}:
                with redirect_stdout(io.StringIO()):
                    lab.main([command])

    def test_bare_creation_and_partial_input_have_actionable_help(self):
        for command in ('kernel-create', 'kernel-import', 'machine-create', 'machine-add', 'machine-use'):
            result = self.make(command)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f'Usage: make {command}', result.stdout)
        result = self.make('machine-create', 'NAME=dev')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing required options: KERNEL', result.stderr)
        self.assertIn('Usage: make machine-create NAME=<machine> KERNEL=<kernel>', result.stderr)
        self.assertFalse(self.state.exists())

    def test_discovery_and_empty_machine_errors_offer_next_steps(self):
        for args in ((), ('help',), ('help', 'COMMAND=kernel-remove')):
            result = self.make(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('make kernel-remove' if len(args) == 2 else 'Learn a command:', result.stdout)
        result = self.make('machine-list')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('No machines registered.', result.stdout)
        result = self.make('machine-start')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('make machine-create NAME=dev KERNEL=<kernel>', result.stderr)
        self.assertIn('make machine-start HELP=1', result.stderr)
        result = self.make('help', 'COMMAND=not-a-command')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unknown command: not-a-command', result.stderr)

    def test_python_command_help_and_debugger_argument_boundary(self):
        with redirect_stdout(io.StringIO()) as output:
            lab.main(['kernel-remove', '--help'])
        self.assertIn('Usage: make kernel-remove', output.getvalue())
        with patch.object(runs, 'find_run', return_value={'name': 'frozen'}), \
             patch.object(runs, 'attach') as attach:
            lab.main(['gdb-attach', '--', '--help'])
        attach.assert_called_once_with({'name': 'frozen'}, ['--help'])

    def test_explicit_removal_still_dispatches_to_the_existing_implementation(self):
        self.seed_selection()
        with patch.dict(os.environ, {'KERNEL': 'base'}), patch.object(kernels, 'remove') as remove:
            lab.main(['kernel-remove'])
        self.assertEqual(remove.call_args.args[0]['name'], 'base')
        with patch.dict(os.environ, {'MACHINE': 'dev'}), patch.object(machines, 'remove') as remove:
            lab.main(['machine-remove'])
        self.assertEqual(remove.call_args.args[0]['name'], 'dev')


if __name__ == '__main__':
    unittest.main()

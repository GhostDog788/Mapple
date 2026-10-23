"""Navigation lifecycle contracts; full editor switching is in navigation_integration.py."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compile_commands
import lab
from mapple_dev import core as c, kernels, machines, navigation, runs


class NavigationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo = Path(temp.name)
        for module, name, value in [(c, 'ROOT', self.repo), (c, 'STATE', self.repo / '.lab')]:
            p = patch.object(module, name, value)
            p.start(); self.addCleanup(p.stop)
        p = patch.dict(os.environ, {'PATH': '/usr/bin:/bin'}, clear=True)
        p.start(); self.addCleanup(p.stop)

    def kernel(self, name='a'):
        k = {'name': name, 'id': name + '-id', 'release': '6.6-' + name, 'toolchain': {}, 'artifacts': {}}
        c.save(c.record('kernels', name), k)
        source = self.repo / name / 'file.c'
        source.parent.mkdir(); source.write_text('int value(void) { return 1; }\n')
        db = c.directory(c.STATE / 'builds/module' / k['id']) / 'compile_commands.json'
        c.save(db, [{'directory': str(self.repo), 'file': str(source),
                     'arguments': ['gcc', '-D__KERNEL__', '-DMODULE', '-Wp,-MMD,missing/out.d',
                                   '-c', '-o', 'missing/out.o', str(source)]}])
        navigation.capture(k, db)
        return k, db

    def machine(self, name, kernel):
        m = {'name': name, 'id': name + '-machine', 'kernel': kernel}
        c.save(c.record('machines', name), m)
        return m

    def test_compiler_context_survives_clean_and_preserves_index_and_receipt(self):
        k, db = self.kernel()
        self.machine('dev', 'a'); machines.select('dev')
        cache = c.directory(navigation.root(k) / '.cache/clangd/index') / 'fixture.idx'
        cache.write_bytes(b'index')
        record = c.load(navigation.root(k) / 'record.json')
        c.save(navigation.root(k) / 'ready.json', {'fingerprint': record['fingerprint'], 'at': 'now'})
        before = (c.ROOT / 'compile_commands.json').read_bytes()
        lab.main(['clean'])
        self.assertFalse(db.exists())
        self.assertEqual((c.ROOT / 'compile_commands.json').read_bytes(), before)
        self.assertEqual(cache.read_bytes(), b'index')
        self.assertEqual(navigation.status()['preparation'], 'ready')
        args = c.load(navigation.root(k) / 'compile_commands.json')[0]['arguments']
        self.assertIn('-D__KERNEL__', args)
        self.assertIn('-DMODULE', args)
        self.assertFalse(any('missing/out' in arg for arg in args))

    def test_generation_changes_only_for_new_kernel_context(self):
        a, _ = self.kernel('a'); b, _ = self.kernel('b')
        for name, k in [('first', a), ('second', a), ('other', b)]: self.machine(name, k['name'])
        with patch.object(c, 'ssh', side_effect=AssertionError('offline selection')):
            machines.select('first'); first = c.load(navigation.editor_file())
            machines.select('second'); second = c.load(navigation.editor_file())
            self.assertEqual(first['generation'], second['generation'])
            self.assertEqual(second['machine'], 'second')
            machines.select('other'); third = c.load(navigation.editor_file())
        self.assertNotEqual(second['generation'], third['generation'])
        self.assertEqual(third['directory'], str(navigation.root(b)))

    def test_selection_without_database_retires_old_navigation_context(self):
        k, _ = self.kernel()
        self.machine('ready', 'a'); machines.select('ready')
        c.save(c.record('kernels', 'missing'), {'name': 'missing', 'id': 'missing-id'})
        self.machine('missing', 'missing'); machines.select('missing')
        manifest = c.load(navigation.editor_file())
        self.assertEqual(manifest['phase'], 'missing-database')
        self.assertNotIn('directory', manifest)
        self.assertFalse((c.ROOT / 'compile_commands.json').exists())

    def test_database_change_invalidates_prepared_receipt_but_code_edit_does_not(self):
        k, db = self.kernel()
        first = c.load(navigation.root(k) / 'record.json')
        receipt = navigation.root(k) / 'ready.json'
        c.save(receipt, {'fingerprint': first['fingerprint'], 'at': 'now'})
        source = Path(c.load(db)[0]['file']); source.write_text('int value(void) { return 2; }\n')
        navigation.capture(k, db)
        self.assertTrue(receipt.exists())  # clangd incrementally refreshes changed source.
        data = c.load(db); data[0]['arguments'].insert(1, '-DCONTEXT_CHANGED'); c.save(db, data)
        navigation.capture(k, db)
        self.assertFalse(receipt.exists())
        self.assertNotEqual(first['fingerprint'], c.load(navigation.root(k) / 'record.json')['fingerprint'])

    def test_explicit_kernel_preparation_is_offline_and_does_not_select_it(self):
        a, _ = self.kernel('a'); b, _ = self.kernel('b')
        self.machine('dev', 'a'); machines.select('dev')
        before = c.load(navigation.editor_file())
        os.environ['KERNEL'] = 'b'
        with patch.object(c, 'ssh', side_effect=AssertionError('no SSH')), \
             patch.object(runs, 'build') as build, patch.object(navigation, 'index', return_value={'ready': True}):
            self.assertEqual(lab.main(['navigation-prepare']), {'ready': True})
        build.assert_called_once_with(b, publish=False)
        self.assertEqual(c.load(navigation.editor_file()), before)
        self.assertEqual(c.active(), 'dev')

    def test_status_without_machine_is_offline_and_explicit_kernel_can_be_inspected(self):
        k, _ = self.kernel()
        with patch.object(c, 'ssh', side_effect=AssertionError('no SSH')):
            self.assertEqual(navigation.status()['phase'], 'no-active-machine')
            self.assertEqual(navigation.status(k)['scope'], 'headers-only')

    def test_capturing_build_context_does_not_require_an_installed_language_server(self):
        os.environ['CLANGD'] = '/missing/clangd'
        k, db = self.kernel()
        record = navigation.capture(k, db)
        self.assertEqual(record['clangd'], '/missing/clangd')
        c.save(navigation.root(k) / 'ready.json', {'fingerprint': record['fingerprint'], 'at': 'before-engine-removal'})
        with self.assertRaisesRegex(RuntimeError, 'Missing executable'):
            navigation.index(k, record, [])
        self.assertFalse((navigation.root(k) / 'ready.json').exists())

    def test_retiring_active_machine_clears_editor_but_keeps_kernel_cache(self):
        k, _ = self.kernel(); m = self.machine('dev', 'a'); machines.select('dev')
        machines.retire(m)
        self.assertEqual(c.load(navigation.editor_file())['phase'], 'no-active-machine')
        self.assertTrue((navigation.root(k) / 'compile_commands.json').exists())

    def test_custom_sdk_driver_is_absolute_and_query_is_restricted_to_that_driver(self):
        k, db = self.kernel()
        sdk = c.directory(self.repo / 'sdk'); cc = sdk / 'vendor-gcc'
        cc.write_text('#!/bin/sh\nexit 0\n'); cc.chmod(0o700)
        k['toolchain']['env'] = {'PATH': str(sdk) + ':/usr/bin:/bin'}
        entries = c.load(db); entries[0]['arguments'][0] = 'vendor-gcc'; c.save(db, entries)
        record = navigation.capture(k, db)
        self.assertEqual(c.load(navigation.root(k) / 'compile_commands.json')[0]['arguments'][0], str(cc))
        self.assertIn('--query-driver=' + str(cc), navigation.arguments(record))

    @unittest.skipUnless(shutil.which('clangd-18'), 'clangd-18 required')
    def test_real_index_preparation_reuses_cache_and_checks_unopened_implementation(self):
        k, db = self.kernel()
        directory = Path(c.load(db)[0]['file']).parent
        header = directory / 'api.h'; header.write_text('int api(void);\n')
        caller = directory / 'caller.c'; caller.write_text('#include "api.h"\nint call(void) { return api(); }\n')
        callee = directory / 'callee.c'; callee.write_text('int api(void) { return 7; }\n')
        c.save(db, [{'directory': str(directory), 'file': str(f), 'arguments': ['gcc', '-I'+str(directory), '-c', str(f)]}
                    for f in (caller, callee)])
        record = navigation.capture(k, db)
        checks = [(caller, 'return api();', 'api', str(callee))]
        receipt = navigation.index(k, record, checks)
        self.assertEqual(receipt['checks'][0]['destination'], str(callee))
        shards = list((navigation.root(k) / '.cache/clangd/index').glob('*.idx'))
        self.assertTrue(shards)
        before = {p: p.stat().st_mtime_ns for p in shards}
        navigation.index(k, record, checks)
        self.assertEqual({p: p.stat().st_mtime_ns for p in shards}, before)


if __name__ == '__main__': unittest.main()

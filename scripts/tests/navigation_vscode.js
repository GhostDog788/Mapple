// Run only inside VS Code's extension-test host; no npm dependencies.
'use strict';
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {execFileSync} = require('child_process');
const {performance} = require('perf_hooks');

exports.run = async function () {
    const root = process.env.MAPPLE_NAV_TEST_ROOT;
    const lab = path.join(root, '.lab');
    const resultFile = process.env.MAPPLE_NAV_TEST_RESULTS;
    const result = {checks: [], timings: []};
    const extension = await vscode.extensions.getExtension('mapple.navigation').activate();
    const clangd = vscode.extensions.getExtension('llvm-vs-code-extensions.vscode-clangd').exports;
    function command(name, variables = {}) { return execFileSync('python3', [path.join(root, 'scripts/lab.py'), name],
        {cwd: root, env: {...process.env, ...variables, MAPPLE_LAB_DIR: lab}, encoding: 'utf8'}); }
    async function ready(kernel, phase = 'ready') {
        const deadline = Date.now() + 45000;
        while (Date.now() < deadline) {
            await extension.settled();
            const state = extension.status();
            if (state.kernel === kernel && state.phase === phase) return state;
            if (state.phase === 'error') throw new Error(JSON.stringify(state));
            await new Promise(resolve => setTimeout(resolve, 100));
        }
        throw new Error('Navigation not ready: ' + JSON.stringify(extension.status()));
    }
    async function lookup(kernel) {
        const file = path.join(root, 'kernel/src/control/transports/tcp_server/tcp.c');
        const document = await vscode.workspace.openTextDocument(file);
        const lines = document.getText().split('\n');
        const line = lines.findIndex(text => text.includes('int ret = kernel_accept('));
        const position = new vscode.Position(line, lines[line].indexOf('kernel_accept') + 1);
        const expected = path.join(lab, 'kernels', kernel, 'source/net/socket.c');
        const start = performance.now();
        const locations = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', document.uri, position);
        const paths = (locations || []).map(location => (location.uri || location.targetUri).fsPath);
        assert.deepStrictEqual([...new Set(paths)], [expected], 'No definitions from another kernel');
        result.timings.push({kernel, milliseconds: performance.now() - start});
    }
    try {
        await ready('a');
        await lookup('a');
        await vscode.window.showTextDocument(vscode.Uri.file(path.join(lab, 'kernels/a/source/net/socket.c')), {preview: false});
        result.checks.push('A: unopened Linux implementation');
        if (process.env.MAPPLE_NAV_TEST_REOPEN !== '1') {
            let oldClient = clangd.getApi(1).languageClient;
            for (const kernel of ['b', 'a']) {
                command('machine-use', {NAME: kernel});
                await ready(kernel);
                assert(vscode.window.tabGroups.all.flatMap(group => group.tabs).some(tab =>
                    tab.input?.uri?.fsPath === path.join(lab, 'kernels/a/source/net/socket.c')),
                    'The old source tab should remain open');
                assert.notStrictEqual(clangd.getApi(1).languageClient, oldClient, 'Kernel change must replace the server');
                oldClient = clangd.getApi(1).languageClient;
                await lookup(kernel);
                result.checks.push(kernel.toUpperCase()+': automatic restart and exclusive matching definition');
            }
            result.checks.push('Old kernel source tab stays open without contaminating the new index');
            await vscode.commands.executeCommand('clangd.restart');
            await new Promise(resolve => setTimeout(resolve, 700));
            await ready('a'); await lookup('a');
            result.checks.push('Manual clangd restart restores live status and kernel isolation');
            oldClient = clangd.getApi(1).languageClient;
            command('machine-use', {NAME: 'alias'});
            await new Promise(resolve => setTimeout(resolve, 500));
            await extension.settled();
            assert.strictEqual(clangd.getApi(1).languageClient, oldClient, 'Same-kernel machine switch should retain the server');
            assert.strictEqual(extension.status().machine, 'alias');
            result.checks.push('Same-kernel selection retains the language server');
            command('clean');
            assert(fs.existsSync(path.join(root, 'compile_commands.json')));
            for (const kernel of ['a', 'b', 'headers']) {
                const directory = path.join(lab, 'navigation', kernel+'-fixture');
                assert(fs.existsSync(path.join(directory, 'compile_commands.json')));
                assert(fs.existsSync(path.join(directory, 'ready.json')));
                assert(fs.readdirSync(path.join(directory, '.cache/clangd/index')).length > 0);
                assert(!fs.existsSync(path.join(lab, 'builds/module', kernel+'-fixture')));
            }
            await lookup('a');
            result.checks.push('Clean preserves databases, indexes, receipts and live navigation');
            command('machine-use', {NAME: 'missing'});
            await ready('missing', 'missing-database');
            assert(!fs.existsSync(path.join(root, 'compile_commands.json')));
            assert.notStrictEqual(clangd.getApi(1).languageClient, oldClient);
            result.checks.push('Unprepared kernel clears the previous server and reports missing database');
            command('machine-use', {NAME: 'headers'});
            await ready('headers', 'limited');
            assert.strictEqual(extension.status().scope, 'headers-only');
            result.checks.push('Headers-only kernel reports limited navigation without GDB or Linux symbols/source');
            command('machine-use', {NAME: 'a'});
            await ready('a'); await lookup('a');
        } else {
            assert(!fs.existsSync(path.join(lab, 'builds/module/a-fixture')));
            result.checks.push('New editor session navigates after clean without rebuilding Mapple');
        }
    } catch (error) {
        result.error = error.stack;
        fs.writeFileSync(resultFile, JSON.stringify(result, null, 2));
        throw error;
    }
    fs.writeFileSync(resultFile, JSON.stringify(result, null, 2));
};

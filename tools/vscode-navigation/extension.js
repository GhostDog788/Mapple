'use strict';
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {KernelDebugAdapter} = require('./debug-adapter');

async function activate(context) {
    const folder = vscode.workspace.workspaceFolders?.find(f =>
        fs.existsSync(path.join(f.uri.fsPath, 'scripts/lab.py')));
    if (!folder || !vscode.workspace.isTrusted) return;
    const root = folder.uri.fsPath;
    const debugOutput = vscode.window.createOutputChannel('Mapple Debug');
    context.subscriptions.push(debugOutput, vscode.debug.registerDebugAdapterDescriptorFactory('mapple-kernel', {
        createDebugAdapterDescriptor() {
            const cpp = vscode.extensions.getExtension('ms-vscode.cpptools');
            if (!cpp) throw new Error('Install Microsoft C/C++ (ms-vscode.cpptools) for Mapple kernel debugging.');
            const executable = path.join(cpp.extensionPath, 'debugAdapters/bin/OpenDebugAD7');
            return new vscode.DebugAdapterInlineImplementation(new KernelDebugAdapter(executable, debugOutput));
        }
    }));
    const configured = vscode.workspace.getConfiguration('mapple.navigation', folder.uri).get('labDirectory', '.lab');
    const lab = path.resolve(process.env.MAPPLE_LAB_DIR || path.resolve(root, configured));
    const manifestPath = path.join(lab, 'navigation/editor.json');
    const sessionPath = path.join(lab, 'navigation/sessions', crypto.randomUUID() + '.json');
    const bar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 80);
    bar.command = 'mapple.navigation.status';
    const output = vscode.window.createOutputChannel('Mapple Navigation');
    context.subscriptions.push(bar, output);
    let current = null, phase = 'loading', detail = '', client = null;
    let activeGeneration = null, seen = '', disposed = false, chain = Promise.resolve();
    let progressGeneration = null, checking = false, starting = null, ended = null;
    let clangdApi = null, refreshing = false;

    function inside(file, directory) {
        const relative = path.relative(directory, file);
        return relative === '' || (!relative.startsWith('..' + path.sep) && relative !== '..' && !path.isAbsolute(relative));
    }
    function inactive(document, selected) {
        if (document.uri.scheme !== 'file') return false;
        const file = document.uri.fsPath;
        return inside(file, path.join(lab, 'kernels')) &&
            (!selected.kernel_root || !inside(file, selected.kernel_root));
    }

    function render() {
        const label = current?.machine ? `${current.machine} · ${current.kernel || 'no kernel'}` : 'No active machine';
        const names = {loading: 'Loading index', indexing: 'Indexing', ready: 'Ready', limited: 'Limited navigation',
            switching: 'Switching kernel', 'no-active-machine': 'Select a machine',
            'no-assigned-kernel': 'Assign a kernel', 'missing-database': 'Prepare navigation', error: 'Navigation error'};
        const busy = ['loading', 'indexing', 'switching'].includes(phase);
        bar.text = `${busy ? '$(sync~spin)' : phase === 'ready' ? '$(check)' : '$(info)'} ${label} · ${names[phase] || phase}`;
        bar.tooltip = [`Machine: ${current?.machine || 'none'}`, `Kernel: ${current?.kernel || 'none'}`,
            `Release: ${current?.release || 'unknown'}`, `State: ${phase}`, detail,
            'Click for navigation details or preparation.'].filter(Boolean).join('\n');
        bar.show();
        if (current) {
            fs.mkdirSync(path.dirname(sessionPath), {recursive: true});
            const record = {pid: process.pid, generation: current.generation, machine: current.machine,
                kernel: current.kernel, phase, detail, at: new Date().toISOString()};
            const temporary = sessionPath + '.tmp';
            fs.writeFileSync(temporary, JSON.stringify(record));
            fs.renameSync(temporary, sessionPath);
        }
    }

    async function definition(targetClient, file, needle, token, expected) {
        const uri = vscode.Uri.file(file);
        const document = await vscode.workspace.openTextDocument(uri);
        const lines = document.getText().split('\n');
        const line = lines.findIndex(text => text.includes(needle));
        if (line < 0) throw new Error(`Cannot check ${token} in ${file}`);
        // Use the same client as the editor. Do not open the destination to pass.
        const result = await targetClient.sendRequest('textDocument/definition', {
            textDocument: {uri: uri.toString()}, position: {line, character: lines[line].indexOf(token) + 1}});
        const locations = Array.isArray(result) ? result : result ? [result] : [];
        if (!locations.some(x => vscode.Uri.parse(x.uri || x.targetUri).fsPath === expected)) {
            throw new Error(`${token} did not resolve to ${expected}`);
        }
    }

    async function verifyReady(generation) {
        const targetClient = client;
        if (checking === targetClient || starting === generation || disposed || generation !== activeGeneration) return;
        checking = targetClient;
        const selected = current;
        try {
            const tcp = path.join(root, 'kernel/src/control/transports/tcp_server/tcp.c');
            await definition(targetClient, tcp, 'return mapple_session_manager_register(', 'mapple_session_manager_register',
                path.join(root, 'kernel/src/control/sessions/session_manager.c'));
            if (selected.source && generation === activeGeneration && targetClient === client) {
                await definition(targetClient, tcp, 'int ret = kernel_accept(', 'kernel_accept', path.join(selected.source, 'net/socket.c'));
            }
            if (generation !== activeGeneration || targetClient !== client) return;
            phase = current.scope === 'headers-only' || current.commands !== 'recorded' ? 'limited' : 'ready';
            detail = current.scope === 'headers-only' ? 'Linux implementation source is unavailable; Mapple and supplied headers are indexed.' :
                current.commands !== 'recorded' ? 'Linux compiler commands were derived; coverage depends on supplied source and flags.' :
                'Index complete; Mapple and matching Linux implementation checks passed.';
        } catch (error) {
            if (generation !== activeGeneration || targetClient !== client) return;
            phase = 'error'; detail = error.message; output.appendLine(error.stack || String(error));
        } finally {
            if (checking === targetClient) checking = false;
            if (generation === activeGeneration && targetClient === client) render();
        }
    }

    async function refresh() {
        let manifest;
        try { manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8')); }
        catch (error) {
            if (error.code !== 'ENOENT') throw error;
            manifest = {phase: 'no-active-machine', generation: 'none'};
        }
        current = manifest;
        if (activeGeneration === manifest.generation) { render(); return; }
        activeGeneration = manifest.generation;
        const generation = activeGeneration;
        checking = false;
        progressGeneration = null;
        starting = generation;
        ended = null;
        phase = 'switching'; detail = 'Refreshing clangd for the selected kernel.'; render();
        const extension = vscode.extensions.getExtension('llvm-vs-code-extensions.vscode-clangd');
        const api = await extension.activate();
        clangdApi = api;
        client = api.getApi(1).languageClient;
        // Await shutdown: old definitions/providers must be gone before restart.
        if (client) {
            const startupDeadline = Date.now() + 30000;
            while (client.state === 3 && Date.now() < startupDeadline) {
                await new Promise(resolve => setTimeout(resolve, 50));
            }
            await client.stop();
        }
        if (disposed) return;
        await vscode.commands.executeCommand('clangd.restart');
        client = api.getApi(1).languageClient;
        if (!client) throw new Error('clangd did not start. Check the clangd extension settings.');
        const middleware = client.clientOptions.middleware || (client.clientOptions.middleware = {});
        const boundClient = client;
        // Old Linux tabs remain viewable, but cannot contribute to this kernel's
        // dynamic index. Preserve existing clangd middleware where present.
        const previousOpen = middleware.didOpen;
        middleware.didOpen = async (document, next) => {
            if (!inactive(document, manifest)) return previousOpen ? previousOpen(document, next) : next(document);
        };
        const previousChange = middleware.didChange;
        middleware.didChange = async (event, next) => {
            if (!inactive(event.document, manifest)) return previousChange ? previousChange(event, next) : next(event);
        };
        const previousDefinition = middleware.provideDefinition;
        middleware.provideDefinition = async (document, position, token, next) => {
            if (inactive(document, manifest)) return [];
            return previousDefinition ? previousDefinition(document, position, token, next) : next(document, position, token);
        };
        for (const name of ['provideHover', 'provideCompletionItem', 'provideSignatureHelp',
            'provideReferences', 'provideDocumentSymbols', 'provideDocumentHighlights',
            'provideTypeDefinition', 'provideImplementation', 'provideDeclaration',
            'provideDocumentLinks', 'provideCodeActions', 'provideFoldingRanges',
            'provideInlayHints', 'provideDocumentSemanticTokens', 'provideDocumentRangeSemanticTokens']) {
            const previous = middleware[name];
            middleware[name] = async (document, ...args) => {
                if (inactive(document, manifest)) return null;
                return previous ? previous(document, ...args) : args.pop()(document, ...args);
            };
        }
        const previousSave = middleware.didSave;
        middleware.didSave = async (document, next) => {
            if (!inactive(document, manifest)) return previousSave ? previousSave(document, next) : next(document);
        };
        const previousProgress = middleware.handleWorkDoneProgress;
        middleware.handleWorkDoneProgress = (token, value, next) => {
            if (generation === activeGeneration && client === boundClient && token === 'backgroundIndexProgress') {
                progressGeneration = generation;
                if (value.kind === 'begin' || value.kind === 'report') {
                    phase = 'indexing'; detail = value.message || 'Building or restoring the selected kernel index.'; render();
                } else if (value.kind === 'end') {
                    ended = generation;
                    void verifyReady(generation);
                }
            }
            if (previousProgress) previousProgress(token, value, next); else next();
        };
        const deadline = Date.now() + 30000;
        while (!client.isRunning() && Date.now() < deadline) {
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        if (!client.isRunning()) throw new Error('clangd startup timed out. See the clangd output channel.');
        // Initial synchronization can precede middleware attachment. Explicitly
        // remove any inactive documents already sent during that narrow window.
        for (const document of vscode.workspace.textDocuments) {
            if (inactive(document, manifest)) {
                await client.sendNotification('textDocument/didClose', {textDocument: {uri: document.uri.toString()}});
            }
        }
        starting = null;
        if (manifest.phase !== 'loading') {
            phase = manifest.phase; detail = 'Run make navigation-prepare after selecting a kernel with build support.'; render(); return;
        }
        if (!['ready', 'limited', 'error', 'indexing'].includes(phase)) {
            phase = 'loading'; detail = 'Loading the selected kernel compiler context.';
        }
        render();
        await vscode.workspace.openTextDocument(vscode.Uri.file(path.join(root, 'kernel/src/main.c')));
        await client.sendRequest('workspace/symbol', {query: 'mapple'});
        if (ended === generation) await verifyReady(generation);
        // Progress is authoritative; never call a cached receipt live readiness.
        if (progressGeneration === generation && phase === 'ready') render();
        output.appendLine(`Selected ${manifest.machine}: ${manifest.kernel} (${manifest.fingerprint})`);
    }

    function schedule(force = false) {
        if (disposed) return chain;
        let contents;
        try { contents = fs.readFileSync(manifestPath, 'utf8'); }
        catch (error) { if (error.code !== 'ENOENT') output.appendLine(String(error)); contents = ''; }
        if (!force && contents === seen) return chain;
        seen = contents;
        chain = chain.then(async () => {
            refreshing = true;
            try { await refresh(); } finally { refreshing = false; }
        }).catch(error => {
            phase = 'error'; detail = error.message; output.appendLine(error.stack || String(error)); render();
        });
        return chain;
    }

    context.subscriptions.push(vscode.commands.registerCommand('mapple.navigation.prepare', async () => {
        const task = new vscode.Task({type: 'mapple-navigation'}, folder, 'Prepare source navigation', 'Mapple',
            new vscode.ProcessExecution('python3', ['scripts/lab.py', 'navigation-prepare'],
                {cwd: root, env: {MAPPLE_LAB_DIR: lab}}));
        await vscode.tasks.executeTask(task);
    }));
    context.subscriptions.push(vscode.commands.registerCommand('mapple.navigation.status', async () => {
        const choice = await vscode.window.showInformationMessage(bar.tooltip,
            'Prepare navigation', 'Show output');
        if (choice === 'Prepare navigation') await vscode.commands.executeCommand('mapple.navigation.prepare');
        if (choice === 'Show output') output.show();
    }));
    // Exact-file polling also works when .lab is excluded by editor file watchers.
    const watch = () => void schedule();
    fs.watchFile(manifestPath, {interval: 200}, watch);
    const monitor = setInterval(() => {
        if (disposed || refreshing || !clangdApi) return;
        if (clangdApi.getApi(1).languageClient !== client) {
            // A manual upstream restart replaces its public LanguageClient too.
            activeGeneration = null;
            void schedule(true);
        } else if (client && !client.isRunning() && ['ready', 'limited'].includes(phase)) {
            phase = 'loading'; detail = 'Waiting for clangd to reconnect.'; render();
        }
    }, 500);
    context.subscriptions.push({dispose() {
        disposed = true; fs.unwatchFile(manifestPath, watch);
        clearInterval(monitor);
        try { fs.unlinkSync(sessionPath); } catch (_) { /* Already removed. */ }
    }});
    await schedule(true);
    return {settled: () => chain, status: () => ({...current, phase, detail})};
}

module.exports = {activate};

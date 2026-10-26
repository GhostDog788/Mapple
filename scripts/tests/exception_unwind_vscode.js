const vscode = require('vscode');
const fs = require('fs');
const assert = require('assert');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

exports.run = async function () {
    const context = JSON.parse(fs.readFileSync(process.env.MAPPLE_UNWIND_CONTEXT, 'utf8'));
    const result = {checks: []};
    const messages = [];
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('mapple-kernel', {
        createDebugAdapterTracker() { return {
            onDidSendMessage(message) { messages.push(message); },
        }; },
    });
    let session;
    async function until(predicate, label) {
        const deadline = Date.now() + 20000;
        while (Date.now() < deadline) {
            const value = predicate();
            if (value) return value;
            await delay(50);
        }
        throw new Error('Timed out: ' + label);
    }
    try {
        await vscode.extensions.getExtension('mapple.navigation').activate();
        const started = await vscode.debug.startDebugging(vscode.workspace.workspaceFolders[0], {
            name: 'Read-only exception fixture', type: 'mapple-kernel', request: 'launch',
            program: context.binary, coreDumpPath: context.core,
            cwd: vscode.workspace.workspaceFolders[0].uri.fsPath, MIMode: 'gdb',
            miDebuggerPath: '/usr/bin/gdb', targetArchitecture: 'x64', externalConsole: false,
            customLaunchSetupCommands: [
                {text: 'set debuginfod enabled off'},
                {text: 'file ' + context.binary},
                {text: 'core-file ' + context.core},
                {text: 'source ' + context.helper},
            ],
            launchCompleteCommand: 'None',
        });
        assert(started);
        session = await until(() => vscode.debug.activeDebugSession, 'debug session');
        const stopped = await until(() => messages.find(m => m.event === 'stopped'), 'core loaded');
        const frames = (await session.customRequest('stackTrace', {threadId: stopped.body.threadId})).stackFrames;
        const fault = frames.find(f => f.name.includes('divide_by_zero'));
        const caller = frames.find(f => f.name.includes('command'));
        assert(fault && caller, JSON.stringify(frames));
        assert.strictEqual(fs.realpathSync(fault.source.path), fs.realpathSync(context.source));
        const line = fs.readFileSync(context.source, 'utf8').split('\n').findIndex(l => l.includes('FAULT_LINE')) + 1;
        assert.strictEqual(fault.line, line);
        result.checks.push('faulting function, exact source line and caller in Call Stack');
        const scopes = await session.customRequest('scopes', {frameId: fault.id});
        const variables = [];
        for (const scope of scopes.scopes) {
            if (!scope.expensive) variables.push(...(await session.customRequest('variables', {
                variablesReference: scope.variablesReference,
            })).variables);
        }
        assert(variables.some(v => v.name === 'divisor' && v.value === '0'), JSON.stringify(variables));
        assert(variables.some(v => v.name === 'marker' && v.value === '1234'), JSON.stringify(variables));
        result.checks.push('Variables panel exposes divisor=0 and marker=1234');
        for (const evaluation of ['watch', 'hover']) {
            const value = await session.customRequest('evaluate', {
                expression: 'divisor', frameId: fault.id, context: evaluation,
            });
            assert.strictEqual(value.result, '0');
        }
        const path = await session.customRequest('evaluate', {
            expression: 'path', frameId: caller.id, context: 'watch',
        });
        assert(path.result.includes('/fixture/file'), path.result);
        result.checks.push('Watch, hover and caller variables use the recovered frame');
        // Exercise actual Call Stack navigation, not just manually opening the
        // source file. VS Code should select the frame and highlight its line.
        for (let i = 0; i < frames.indexOf(fault); i++) {
            await vscode.commands.executeCommand('workbench.action.debug.callStackDown');
            await delay(100);
        }
        await until(() => vscode.window.activeTextEditor?.document.uri.fsPath === fault.source.path &&
                          vscode.window.activeTextEditor.selection.active.line === line - 1,
                    'fault line selected in editor');
        result.checks.push('Call Stack navigation selects the fault line in the editor');
        result.checks.push('automatic startup loading');
        result.result = 'PASS';
    } catch (error) {
        result.result = 'FAIL'; result.error = error.stack;
    } finally {
        if (session) await vscode.debug.stopDebugging(session);
        tracker.dispose();
        fs.writeFileSync(process.env.MAPPLE_UNWIND_RESULTS, JSON.stringify(result, null, 2));
        fs.writeFileSync(process.env.MAPPLE_UNWIND_RESULTS + '.dap.json', JSON.stringify(messages, null, 2));
    }
    if (result.result !== 'PASS') throw new Error(result.error);
};

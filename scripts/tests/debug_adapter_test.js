'use strict';
const assert = require('assert');
const {EventEmitter} = require('events');
const {PassThrough} = require('stream');
const {KernelDebugAdapter} = require('../../tools/vscode-navigation/debug-adapter');

const child = new EventEmitter();
Object.assign(child, {stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(), exitCode: null});
let killed = false;
child.kill = () => {killed = true;};
const adapter = new KernelDebugAdapter('unused', {appendLine() {}}, () => child);
const sent = [], received = [];
child.stdin.on('data', b => sent.push(JSON.parse(b.subarray(b.indexOf('\r\n\r\n') + 4).toString())));
adapter.onDidSendMessage(message => received.push(message));
function backend(message) {
    const body = Buffer.from(JSON.stringify(message));
    const frame = Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]);
    // Pipes can split headers and UTF-8 bodies at arbitrary boundaries.
    for (let i = 0; i < frame.length; i += 7) child.stdout.write(frame.subarray(i, i + 7));
}
function request(seq, command, args) {adapter.handleMessage({type: 'request', seq, command, arguments: args});}

request(1, 'initialize', {adapterID: 'mapple-kernel'});
assert.strictEqual(sent.at(-1).arguments.adapterID, 'cppdbg');
request(2, 'launch', {type: 'mapple-kernel', program: '/frozen/mapple.ko', launchCompleteCommand: 'exec-continue'});
assert.strictEqual(sent.at(-1).arguments.type, 'cppdbg');
assert.strictEqual(sent.at(-1).arguments.program, '/frozen/mapple.ko');
backend({type: 'event', seq: 99, event: 'thread', body: {reason: 'started', threadId: 1}});
assert.strictEqual(received.at(-1).event, 'continued');
assert.strictEqual(received.at(-2).event, 'thread');
backend({type: 'event', seq: 100, event: 'stopped', body: {threadId: 2, reason: 'pause'}});
request(3, 'evaluate', {expression: '-exec continue', context: 'repl', frameId: 23});
assert.strictEqual(sent.at(-1).command, 'continue');
assert.strictEqual(sent.at(-1).arguments.threadId, 2);
assert.strictEqual(sent.at(-1).arguments.singleThread, false);
backend({type: 'response', seq: 101, request_seq: 3, command: 'continue', success: true, body: {}});
assert.strictEqual(received.at(-2).event, 'continued');
assert.strictEqual(received.at(-2).body.allThreadsContinued, true);
assert.strictEqual(received.at(-1).command, 'evaluate');
assert.strictEqual(received.at(-1).body.variablesReference, 0);
request(20, 'stackTrace', {threadId: 9, levels: 1});
backend({type: 'response', seq: 106, request_seq: 20, command: 'stackTrace', success: true,
         body: {stackFrames: [{id: 42, name: 'selected CPU'}]}});
request(21, 'evaluate', {expression: '-exec next', context: 'repl', frameId: 42});
assert.strictEqual(sent.at(-1).arguments.threadId, 9, 'Console stepping must honor the selected frame/CPU');

for (const [expression, command, granularity] of [
    ['-exec c', 'continue'], ['-exec n', 'next'], ['-exec s', 'stepIn'],
    ['-exec finish', 'stepOut'], ['-exec ni', 'next', 'instruction'],
    ['-exec si', 'stepIn', 'instruction'], ['-exec interrupt', 'pause'],
]) {
    request(4, 'evaluate', {expression, context: 'repl'});
    assert.strictEqual(sent.at(-1).command, command);
    assert.strictEqual(sent.at(-1).arguments.granularity, granularity);
}
// A quick step may stop before its response arrives: keep that final state.
request(5, 'evaluate', {expression: '-exec next', context: 'repl'});
backend({type: 'event', seq: 102, event: 'stopped', body: {threadId: 1, reason: 'step'}});
const length = received.length;
backend({type: 'response', seq: 103, request_seq: 5, command: 'next', success: true});
assert.strictEqual(received.length, length + 1);
assert.strictEqual(received.at(-1).command, 'evaluate');

request(6, 'evaluate', {expression: '-exec continue', context: 'repl'});
backend({type: 'response', seq: 104, request_seq: 6, command: 'continue', success: false, message: 'Cannot continue'});
assert.strictEqual(received.at(-1).success, false);
assert.strictEqual(received.at(-1).message, 'Cannot continue');
for (const args of [{expression: '-exec bt', context: 'repl'}, {expression: '-exec continue', context: 'watch'},
                    {expression: '-exec continue *0xffff', context: 'repl'}]) {
    request(7, 'evaluate', args);
    assert.strictEqual(sent.at(-1).command, 'evaluate');
    assert.deepStrictEqual(sent.at(-1).arguments, args);
}
request(8, 'continue', {threadId: 1});
backend({type: 'response', seq: 105, request_seq: 8, command: 'continue', success: true});
assert.strictEqual(received.at(-1).command, 'continue');
backend({type: 'request', seq: 200, command: 'runInTerminal', arguments: {}});
const remapped = received.at(-1).seq;
adapter.handleMessage({type: 'response', seq: 9, request_seq: remapped, command: 'runInTerminal', success: true});
assert.strictEqual(sent.at(-1).request_seq, 200);
assert.strictEqual(new Set(received.map(m => m.seq)).size, received.length);
// A broken adapter transport must end this session, not crash the extension host.
child.stdin.emit('error', new Error('closed pipe'));
assert.strictEqual(received.at(-1).event, 'terminated');
const endedCount = received.length;
child.emit('error', new Error('already closed'));
assert.strictEqual(received.length, endedCount);
adapter.dispose();
assert(killed);
console.log('Debug adapter control/state/framing contracts passed.');

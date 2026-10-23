'use strict';
// Keep Microsoft's C/C++ adapter for GDB. Normalize execution commands entered
// in the Debug Console into DAP controls so the editor sees the state change.
const {spawn} = require('child_process');
const {EventEmitter} = require('events');

const controls = {
    continue: ['continue'], c: ['continue'],
    next: ['next'], n: ['next'], step: ['stepIn'], s: ['stepIn'],
    finish: ['stepOut'], stepi: ['stepIn', 'instruction'], si: ['stepIn', 'instruction'],
    nexti: ['next', 'instruction'], ni: ['next', 'instruction'], interrupt: ['pause'],
};

class KernelDebugAdapter {
    constructor(executable, output, start = spawn) {
        this.output = output;
        this.events = new EventEmitter();
        this.sequence = 0;
        this.thread = 1;
        this.stopVersion = 0;
        this.pending = new Map();
        this.stackRequests = new Map();
        this.frames = new Map();
        this.reverse = new Map();
        this.buffer = Buffer.alloc(0);
        this.disposed = false;
        this.terminated = false;
        this.autoContinue = false;
        this.announcedStart = false;
        this.onDidSendMessage = listener => {
            this.events.on('message', listener);
            return {dispose: () => this.events.off('message', listener)};
        };
        this.child = start(executable, [], {stdio: ['pipe', 'pipe', 'pipe']});
        this.child.stdout.on('data', chunk => {
            try { this.receive(chunk); } catch (error) { this.fail(error); }
        });
        this.child.stderr.on('data', chunk => output.appendLine(chunk.toString()));
        this.child.on('error', error => this.fail(error));
        this.child.stdin.on('error', error => this.fail(error));
        this.child.on('exit', () => {
            if (!this.disposed && !this.terminated) {
                this.terminated = true;
                this.emit({type: 'event', event: 'terminated'});
            }
        });
    }

    fail(error) {
        if (this.disposed || this.terminated) return;
        this.terminated = true;
        this.output.appendLine('C/C++ adapter failed: ' + error.message);
        this.emit({type: 'event', event: 'output', body: {category: 'stderr', output: error.message + '\n'}});
        this.emit({type: 'event', event: 'terminated'});
        if (this.child.exitCode === null) this.child.kill();
    }

    emit(message) {
        const original = message.seq;
        message = {...message, seq: ++this.sequence};
        if (message.type === 'request') this.reverse.set(message.seq, original);
        this.events.emit('message', message);
    }

    handleMessage(message) {
        message = {...message};
        if (message.type === 'response' && this.reverse.has(message.request_seq)) {
            message.request_seq = this.reverse.get(message.request_seq);
            this.reverse.delete(message.request_seq);
        }
        if (message.type === 'request') {
            const args = {...message.arguments};
            if (message.command === 'initialize') args.adapterID = 'cppdbg';
            if (message.command === 'launch' || message.command === 'attach') {
                args.type = 'cppdbg';
                this.autoContinue = args.launchCompleteCommand === 'exec-continue';
            }
            if (message.command === 'stackTrace') this.stackRequests.set(message.seq, args.threadId);
            if (message.command === 'evaluate' && args.context === 'repl') {
                const match = /^\s*-exec\s+([a-z]+)(?:\s+(.*?))?\s*$/.exec(args.expression || '');
                const control = match && controls[match[1]];
                if (control) {
                    const raw = Boolean(match[2]);
                    const thread = this.frames.get(args.frameId) || this.thread;
                    this.pending.set(message.seq, {action: control[0], stopVersion: this.stopVersion, raw, thread});
                    if (raw) message.arguments = args; // Preserve GDB's argument syntax.
                    else {
                        message.command = control[0];
                        message.arguments = {threadId: thread, singleThread: false};
                        if (control[1]) message.arguments.granularity = control[1];
                    }
                    this.output.appendLine(`Debug Console ${match[1]} -> ${control[0]} (CPU ${thread})`);
                }
            }
            if (!this.pending.has(message.seq)) message.arguments = args;
        }
        const body = Buffer.from(JSON.stringify(message));
        this.child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]));
    }

    receive(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (true) {
            const end = this.buffer.indexOf('\r\n\r\n');
            if (end < 0) return;
            const header = this.buffer.subarray(0, end).toString();
            const match = /(?:^|\r\n)Content-Length:\s*(\d+)/i.exec(header);
            if (!match) throw new Error('Invalid C/C++ adapter message header');
            const length = Number(match[1]);
            if (this.buffer.length < end + 4 + length) return;
            const message = JSON.parse(this.buffer.subarray(end + 4, end + 4 + length).toString());
            this.buffer = this.buffer.subarray(end + 4 + length);
            this.forward(message);
        }
    }

    forward(message) {
        if (message.type === 'event' && message.event === 'terminated') this.terminated = true;
        if (message.type === 'event' && message.event === 'thread' && message.body.reason === 'started' &&
            this.autoContinue && !this.announcedStart && this.stopVersion === 0) {
            this.announcedStart = true;
            this.thread = message.body.threadId;
            this.emit(message);
            // The Microsoft adapter does not announce initial execution. On a
            // restarted VS Code session, explicitly refresh the new CPU list
            // and clear the previous adapter's stopped state.
            this.emit({type: 'event', event: 'continued', body: {threadId: this.thread, allThreadsContinued: true}});
            return;
        }
        if (message.type === 'event' && message.event === 'stopped') {
            this.thread = message.body.threadId || this.thread;
            this.stopVersion++;
            this.frames.clear();
        }
        if (message.type === 'response' && this.stackRequests.has(message.request_seq)) {
            const thread = this.stackRequests.get(message.request_seq);
            this.stackRequests.delete(message.request_seq);
            if (message.success) for (const frame of message.body?.stackFrames || []) this.frames.set(frame.id, thread);
        }
        const control = message.type === 'response' && this.pending.get(message.request_seq);
        if (control) {
            this.pending.delete(message.request_seq);
            // A Continue response normally updates VS Code's state by itself.
            // The client sent Evaluate here, so send the missing state event.
            // Never overwrite a stop that already arrived during a quick step.
            if (message.success && control.action !== 'pause' && control.stopVersion === this.stopVersion) {
                this.emit({type: 'event', event: 'continued', body: {threadId: control.thread, allThreadsContinued: true}});
            }
            message = {...message, command: 'evaluate'};
            if (message.success && !control.raw) message.body = {result: control.action === 'pause' ? 'Pausing execution.' : 'Resuming execution.', variablesReference: 0};
        }
        this.emit(message);
    }

    dispose() {
        this.disposed = true;
        this.child.stdin.end();
        // Normal Stop/Restart has already disconnected GDB before disposal.
        // A lost editor must also close the adapter and its debugger transport.
        if (this.child.exitCode === null) this.child.kill();
        this.events.removeAllListeners();
    }
}

module.exports = {KernelDebugAdapter};

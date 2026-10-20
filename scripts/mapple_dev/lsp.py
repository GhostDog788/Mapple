"""Small stdio LSP client for offline navigation preparation and verification."""
import json
import queue
import subprocess
import threading
import time


class Client:
    def __init__(self, argv, log, cwd, env=None, timeout=60):
        self.process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=log)
        self.messages = queue.Queue()
        self.progress = []
        self.sequence = 0
        self.timeout = timeout
        self.opened = set()
        self.write_lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def send(self, message):
        data = json.dumps(message).encode()
        with self.write_lock:
            self.process.stdin.write(f'Content-Length: {len(data)}\r\n\r\n'.encode() + data)
            self.process.stdin.flush()

    def _read(self):
        try:
            while True:
                headers = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        return
                    if line in (b'\r\n', b'\n'):
                        break
                    key, value = line.decode().split(':', 1)
                    headers[key.lower()] = value.strip()
                message = json.loads(self.process.stdout.read(int(headers['content-length'])))
                if 'method' in message and 'id' in message:
                    self.send({'jsonrpc': '2.0', 'id': message['id'], 'result': None})
                else:
                    self.messages.put(message)
        except (OSError, ValueError, KeyError) as error:
            self.messages.put({'closed': str(error)})
        finally:
            self.messages.put({'closed': 'clangd closed its output'})

    def request(self, method, params, timeout=None):
        self.sequence += 1
        identity = self.sequence
        self.send({'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': params})
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            try:
                message = self.messages.get(timeout=max(.001, deadline - time.monotonic()))
            except queue.Empty:
                raise RuntimeError(f'Timed out waiting for clangd: {method}') from None
            if 'closed' in message:
                raise RuntimeError(message['closed'])
            if message.get('method') == '$/progress':
                self.progress.append(message['params'])
            if message.get('id') == identity:
                if 'error' in message:
                    raise RuntimeError(f'clangd {method}: {message["error"]}')
                return message.get('result')

    def notify(self, method, params):
        self.send({'jsonrpc': '2.0', 'method': method, 'params': params})

    def open(self, file):
        if file in self.opened:
            return
        self.opened.add(file)
        self.notify('textDocument/didOpen', {'textDocument': {'uri': file.as_uri(),
                    'languageId': 'c', 'version': 1, 'text': file.read_text()}})

    def close(self):
        try:
            if self.process.poll() is None:
                self.request('shutdown', None, timeout=5)
                self.notify('exit', None)
                self.process.wait(timeout=5)
        except (RuntimeError, OSError, subprocess.SubprocessError):
            pass
        finally:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.reader.join(timeout=2)
            for stream in (self.process.stdin, self.process.stdout):
                stream.close()

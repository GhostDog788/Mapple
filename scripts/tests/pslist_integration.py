#!/usr/bin/env python3
"""Check a live module's pslist stream; --workload runs a controlled target process."""

import argparse
from collections import Counter
from contextlib import ExitStack
import json
import mmap
import os
from pathlib import Path
import signal
import socket
import struct
import tempfile
import threading
import time


ALL_GROUPS = (1 << 10) - 1
FDS, DETAILS, NETWORK, MAPS, ELF = (1 << n for n in range(5, 10))
LEAF_GROUPS = {5: 1, 6: FDS, 7: DETAILS, 8: NETWORK, 11: 4, 12: 8,
               13: 16, 14: 2, 15: MAPS, 16: MAPS, 17: ELF, 18: ELF, 19: ELF}
OBJECT_TYPES = {1: 1, 2: 1, 4: 3, 8: 3, 16: 3, FDS: 2, NETWORK: 2, MAPS: 1, ELF: 4}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def receive(connection, length):
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        require(chunk, 'connection closed before response completed')
        data.extend(chunk)
    return bytes(data)


def request(connection, command, body=b''):
    connection.sendall(struct.pack('!IHHIIQ', 0x4D41504C, 3, command, 0, 0, len(body)) + body)
    magic, version, reply, flags, status, length = struct.unpack('!IHHIIQ', receive(connection, 24))
    require((magic, version, reply, status) == (0x4D41504C, 3, command, 0), 'invalid response header')
    return flags, length


def record(connection):
    prefix = receive(connection, 4)
    size = struct.unpack('!I', prefix)[0]
    require(16 <= size <= 16 * 1024 * 1024, f'invalid record size {size}')
    data = prefix + receive(connection, size - 4)
    _, kind, reserved, group = struct.unpack_from('!IHHQ', data)
    require(reserved == 0, 'nonzero reserved header field')
    return kind, group, data[16:]


def snapshot(connection, requested):
    require(request(connection, 2, struct.pack('!Q', requested)) == (1, 0), 'expected stream')
    kind, group, body = record(connection)
    require((kind, group, len(body)) == (1, 0, 32), 'missing snapshot begin')
    got_request, effective, supported, start = struct.unpack('!QQQQ', body)
    expected = requested | (FDS if requested & (DETAILS | NETWORK) else 0)
    expected |= MAPS if requested & ELF else 0
    require((got_request, effective, supported) == (requested, expected, ALL_GROUPS), 'wrong group masks')
    tasks, links, objects, opened = {}, {}, {}, set()
    outcomes = {}
    counts = Counter()
    previous = None
    last_fd = None
    elf_references = set()
    while True:
        kind, group, body = record(connection)
        if kind == 10:
            require(group == 0 and len(body) == 24, 'invalid snapshot end')
            status, reserved, task_count, object_count, reserved2, end = struct.unpack('!HHIIIQ', body)
            require(status in (1, 2) and not reserved and not reserved2, 'invalid snapshot status')
            require(task_count == len(tasks) and object_count == len(objects), 'wrong snapshot counts')
            require(end >= start and not opened, 'unfinished snapshot')
            require(all(key in objects for refs in links.values() for key in refs.values()), 'dangling task link')
            require(all((ELF, ref) in objects for ref in elf_references), 'dangling ELF reference')
            return tasks, links, objects, outcomes, status
        require(group & effective == group, 'unrequested group emitted')
        if kind == 2:
            require(group == 0 and len(body) == 44, 'invalid task')
            pid, tgid = struct.unpack_from('!II', body, 4)
            require(pid not in tasks, 'duplicate task')
            tasks[pid] = tgid
            links[pid] = {}
        elif kind == 3:
            require(len(body) == 16, 'invalid task link')
            pid, object_type, reserved, object_id = struct.unpack('!IHHQ', body)
            require(pid in tasks and group not in links[pid], 'invalid link owner')
            require(object_type == OBJECT_TYPES.get(group) and object_id > 0 and not reserved, 'invalid link type')
            links[pid][group] = (group, object_id)
        elif kind == 4:
            require(len(body) == 16, 'invalid object begin')
            object_id, object_type, reserved, reserved2 = struct.unpack('!QHHI', body)
            key = (group, object_id)
            require(key not in objects and object_id > 0, 'duplicate/zero object ID')
            require(object_type == OBJECT_TYPES.get(group) and not reserved and not reserved2, 'invalid object type')
            objects[key] = []
            opened.add(key)
        elif kind == 9:
            require(len(body) == 24, 'invalid object end')
            object_id, object_type, status, error, count, reserved = struct.unpack('!QHHIII', body)
            key = (group, object_id)
            require(key in opened and object_type == OBJECT_TYPES.get(group), 'invalid object closure')
            require(status in range(1, 6) and not reserved, 'invalid object status')
            require(counts[key] == count, f'wrong record count for {key}: {counts[key]} != {count}')
            outcomes[key] = (status, error)
            opened.remove(key)
        else:
            require(LEAF_GROUPS.get(kind) == group and len(body) >= 8, 'invalid group record')
            object_id = struct.unpack_from('!Q', body)[0]
            key = (FDS if group == DETAILS else group, object_id)
            require(key in opened, 'record outside its object')
            objects[key].append((kind, body))
            if kind != 7:
                counts[key] += 1
            if kind == 6:
                last_fd = (object_id, struct.unpack_from('!I', body, 8)[0])
            elif kind in (7, 8):
                require(previous in ((6,) if kind == 7 else (6, 7)), 'FD enrichment out of order')
                require(last_fd is not None and struct.unpack_from('!I', body, 8)[0] == last_fd[1], 'FD enrichment mismatch')
                if kind == 7:
                    require(object_id == last_fd[0], 'FD details object mismatch')
            elif kind == 16:
                elf_id = struct.unpack_from('!Q', body, 8)[0]
                if elf_id:
                    elf_references.add(elf_id)
        previous = kind


def check_fixture(result, fixture):
    tasks, links, objects, outcomes, snapshot_status = result
    tids = [fixture['pid'], *fixture['threads']]
    for tid in tids:
        require(tasks.get(tid) == fixture['pid'], 'fixture task missing')
    for group in (1, 2, FDS, NETWORK, MAPS):
        require(len({links[tid][group] for tid in tids}) == 1, 'shared object collected more than once')
    for group in (4, 8, 16):
        require(len({links[tid][group] for tid in tids}) == len(tids), 'per-task object incorrectly shared')
    fd_records = objects[links[fixture['pid']][FDS]]
    fds = {struct.unpack_from('!I', body, 8)[0] for kind, body in fd_records if kind == 6}
    details = {struct.unpack_from('!I', body, 8)[0] for kind, body in fd_records if kind == 7}
    require(set(fixture['fds']) <= fds and details == fds, 'fixture FD/details missing')
    sockets = {struct.unpack_from('!I', body, 8)[0]
               for kind, body in objects[links[fixture['pid']][NETWORK]] if kind == 8}
    require(set(fixture['sockets']) <= sockets and fixture['file'] not in sockets, 'network inventory incorrect')
    require(any(kind == 16 for kind, body in objects[links[fixture['pid']][MAPS]]), 'no fixture mappings')
    for kind, body in fd_records:
        if kind == 7 and struct.unpack_from('!I', body, 8)[0] in fixture['elf_fds']:
            require(struct.unpack_from('!q', body, 24)[0] == 123, 'ELF inspection advanced file offset')
    for expected in fixture['elf']:
        matches = [(key, body) for key, records in objects.items() if key[0] == ELF
                   for kind, body in records if kind == 17
                   and struct.unpack_from('!Q', body, 96)[0] == expected['inode']]
        require(len(matches) == 1, 'mapped ELF was missing or collected twice')
        key, metadata = matches[0]
        require(outcomes[key] == (expected['status'], expected['error']), 'wrong ELF outcome')
        if expected['status'] == 1:
            require(tuple(metadata[8:10]) == (expected['class'], expected['encoding']), 'wrong ELF decoding')
        else:
            require(len(objects[key]) == 1, 'failed ELF leaked partial detail records')
    require(snapshot_status == 2, 'malformed and oversized ELF did not mark snapshot partial')


def exercise(args):
    fixture = json.loads(Path(args.fixture).read_text()) if args.fixture else None
    with socket.create_connection((args.host, args.port), timeout=60) as connection:
        for groups in [0, *(1 << n for n in range(10)), DETAILS | NETWORK, 1 | 2 | MAPS, ALL_GROUPS, ALL_GROUPS]:
            result = snapshot(connection, groups)
            if fixture and groups == ALL_GROUPS:
                check_fixture(result, fixture)
            print(f'PASS groups={groups:#05x}: {len(result[0])} tasks, {len(result[2])} objects', flush=True)
        flags, length = request(connection, 2, struct.pack('!Q', 1 << 63))
        require((flags, length) == (0, 16), 'invalid mask did not return fixed error')
        error, reserved, supported = struct.unpack('!IIQ', receive(connection, length))
        require(error == 22 and not reserved and supported == ALL_GROUPS, 'wrong invalid-mask error')
        require(request(connection, 1, b'still-alive') == (0, 11), 'session lost after invalid mask')
        require(receive(connection, 11) == b'still-alive', 'echo corrupted after invalid mask')
    for _ in range(3):
        with socket.create_connection((args.host, args.port), timeout=60) as connection:
            require(request(connection, 2, struct.pack('!Q', ALL_GROUPS)) == (1, 0), 'expected stream')
            record(connection)
    time.sleep(0.2)
    with socket.create_connection((args.host, args.port), timeout=60) as connection:
        snapshot(connection, ALL_GROUPS)
    print('PASS invalid-mask recovery, repeated requests and interrupted streams')


def mapped_elf_fixtures(directory, stack):
    fixtures, fds = [], []
    for elf_class, encoding, status, error in [
        (1, 1, 1, 0), (1, 2, 1, 0), (2, 1, 1, 0), (2, 2, 1, 0),
        (0, 0, 3, 8), (2, 1, 5, 7),
    ]:
        ident = b'\x7fELF' + bytes([elf_class, encoding, 1]) + bytes(9)
        order = '<' if encoding == 1 else '>'
        if elf_class == 1:
            data = ident + struct.pack(order + 'HHIIIIIHHHHHH', 3, 3, 1, 0, 0, 0, 0,
                                       52, 32, 0, 40, 0, 0)
        else:
            data = ident + struct.pack(order + 'HHIQQQIHHHHHH', 3, 62, 1, 0,
                                       64 if status == 5 else 0, 0, 0,
                                       64, 56, 4097 if status == 5 else 0, 64, 0, 0)
        path = Path(directory) / f'elf-{len(fixtures)}'
        file = stack.enter_context(open(path, 'w+b'))
        file.write(data)
        file.truncate(4096)
        file.flush()
        file.seek(123)
        stack.enter_context(mmap.mmap(file.fileno(), 4096, access=mmap.ACCESS_READ))
        fixtures.append({'inode': os.fstat(file.fileno()).st_ino, 'class': elf_class,
                         'encoding': encoding, 'status': status, 'error': error})
        fds.append(file.fileno())
    return fixtures, fds


def workload(churn):
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    signal.signal(signal.SIGINT, lambda *_: stopped.set())
    with tempfile.TemporaryDirectory(prefix='mapple-pslist-') as directory, ExitStack() as stack:
        elf, elf_fds = mapped_elf_fixtures(directory, stack)
        with open(Path(directory) / 'mapped-file', 'w+b') as file, socket.socket() as listener:
            file.truncate(4096)
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            pair = socket.socketpair()
            pipe = os.pipe()
            with mmap.mmap(file.fileno(), 4096) as mapping:
                mapping[0] = 42
                def worker():
                    while not stopped.wait(0.002 if churn else 1):
                        if churn:
                            with open('/dev/null', 'rb'), mmap.mmap(-1, 4096):
                                child = threading.Thread(target=stopped.wait, args=(0.001,))
                                child.start()
                                child.join()
                threads = [threading.Thread(target=worker) for _ in range(2)]
                for thread in threads:
                    thread.start()
                sockets = [listener.fileno(), *(s.fileno() for s in pair)]
                print(json.dumps({'pid': os.getpid(), 'threads': [t.native_id for t in threads],
                                  'file': file.fileno(), 'sockets': sockets, 'elf': elf, 'elf_fds': elf_fds,
                                  'fds': [file.fileno(), *sockets, *pipe]}), flush=True)
                stopped.wait(600)
                stopped.set()
                for thread in threads:
                    thread.join()
            for endpoint in pair:
                endpoint.close()
            for fd in pipe:
                os.close(fd)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=4444)
    parser.add_argument('--fixture', help='JSON emitted by --workload on the target machine')
    parser.add_argument('--workload', action='store_true')
    parser.add_argument('--churn', action='store_true')
    arguments = parser.parse_args()
    if arguments.workload:
        workload(arguments.churn)
    else:
        exercise(arguments)

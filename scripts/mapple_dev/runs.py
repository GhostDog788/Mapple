"""Isolated module builds and frozen deployment/debug sessions."""
from argparse import Namespace
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import uuid
import compile_commands
import kernel_compat
from . import core as c, kernels, machines

MODES = {'module': 1, 'kernel-symbol': 2, 'kernel-source': 3}


def process_identity(pid):
    """Identify a live Linux process without confusing a reused PID with its owner."""
    try:
        fields = Path(f'/proc/{int(pid)}/stat').read_text().rsplit(')', 1)[1].split()
        if fields[0] in ('Z', 'X'):
            return None
        return {'pid': int(pid), 'start_time': fields[19],
                'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
    except (FileNotFoundError, ProcessLookupError):
        return None


def reconcile_unlocked(r):
    """Caller holds the machine, connection and run locks; never terminate a process."""
    if r['status'] != 'debugging':
        return r
    for role in ('supervisor', 'debugger'):
        identity = r.get(role + '_identity')
        pid = identity['pid'] if identity else r.get(role + '_pid')
        current = process_identity(pid) if pid else None
        if current and (identity is None or current == identity):
            c.fail('Run is currently attached to a live debugger or its supervisor.')
    r.update(status='detached', detached_at=c.now())
    c.save(c.record('runs', r['name']), r)
    return r


def finish_debug(r):
    """Reconcile after Stop, including launchers killed before their finally block."""
    m = r['machine_snapshot']
    with c.lock('machine-' + m['name'], wait=5), c.lock(c.connection_key(m), wait=5), c.lock('run-' + r['name'], wait=5):
        return reconcile_unlocked(c.get('runs', r['name']))


def publish_navigation(m):
    with c.lock('editor', wait=30):
        publish_navigation_unlocked(m)


def publish_navigation_unlocked(m):
    m = c.get('machines', m['name'])
    try:
        if c.active() != m['name']:
            return
    except RuntimeError:
        return
    from . import navigation
    navigation.selection(m)


def build(k, publish=True, freeze=None):
    with c.lock('kernel-' + k['name'], wait=300), c.lock('build-' + k['id'], wait=300):
        current = c.get('kernels', k['name'])
        if current['id'] != k['id']:
            c.fail('Kernel slot was replaced while waiting for its build.')
        k = current
        kernels.validate(k)
        if not kernels.characteristics(k)['build']:
            c.fail('Tier 0: ' + ', '.join(kernels.characteristics(k)['blockers']))
        build = kernels.path(k, 'build')
        kernel_compat.check(build)
        t = k.get('toolchain', {})
        c.executable(shlex.split(t.get('cc', 'gcc'))[0], env=kernels.toolenv(k))
        directory = c.directory(c.STATE / 'builds/module' / k['id'])
        c.logged(['make', '-C', c.ROOT / 'kernel', *kernels.make_args(k),
               f'KDIR={build}', f'INTERMEDIATE_DIR={directory / "intermediate"}',
               f'ARTIFACT_DIR={directory / "artifacts"}', f'MODULE_DIR={c.ROOT / "kernel"}',
               f'MODULE_COMPILE_COMMANDS={directory / "module.json"}',
               f'LINUX_COMPILE_COMMANDS={directory / "linux.json"}',
               f'COMPILE_COMMANDS={directory / "compile_commands.json"}',
               f'KERNEL_SOURCE={kernels.path(k, "source") or ""}', 'modules'], directory / 'build.log', env=kernels.toolenv(k))
        artifact = directory / 'artifacts/mapple.ko'
        if not artifact.is_file():
            c.fail('Module build did not produce mapple.ko.')
        expected = {'x86_64': 'X86-64', 'aarch64': 'AArch64', 'arm': 'ARM', 'riscv64': 'RISC-V'}.get(k['arch'])
        if expected and expected not in kernels.elf_info(artifact).get('machine', ''):
            c.fail('Compiler produced a module for another architecture. Review the kernel toolchain.')
        from . import navigation
        navigation.capture(k, directory / 'compile_commands.json')
        if freeze is not None:
            # Freeze under the build lock: a second machine may rebuild the
            # same kernel slot as soon as this lock is released.
            shutil.copyfile(artifact, freeze)
            shutil.copyfile(directory / 'build.log', Path(freeze).parent / 'build.log')
        if publish:
            try:
                m = c.get('machines', c.active())
                if m.get('kernel') == k['name']:
                    publish_navigation(m)
            except RuntimeError:
                pass
    return artifact


def new(m, k, kind='module'):
    name = 'run-' + uuid.uuid4().hex[:16]
    r = {'name': name, 'kind': kind, 'machine': m['name'], 'machine_id': m['id'],
         'machine_snapshot': dict(m), 'kernel': k['name'], 'kernel_id': k['id'],
         'status': 'created', 'created_at': c.now()}
    c.save(c.record('runs', name), r)
    return r


def root(r):
    return c.record('runs', r['name']).parent


def prepare(m, k, debug=False, existing=None):
    r = existing or new(m, k)
    try:
        return prepare_run(m, k, debug, r)
    except BaseException as error:
        try:
            with c.lock('run-' + r['name']):
                current = c.get('runs', r['name'])
                if current.get('status') != 'debugging':
                    r.update(status='failed', error=str(error))
                    c.save(c.record('runs', r['name']), r)
                    detail = getattr(error, 'stderr', b'') or b''
                    if isinstance(detail, bytes):
                        detail = detail.decode(errors='replace')
                    (root(r) / 'deployment.log').write_text(str(error) + '\n' + detail)
        except RuntimeError:
            pass  # A debugger may already own this run; never overwrite it.
        raise


def prepare_run(m, k, debug=False, existing=None):
    if m.get('kernel') != k['name']:
        c.fail('Deployment kernel must be the machine assignment. Use machine-prepare or machine-adopt first.')
    r = existing or new(m, k)
    with c.lock('machine-' + m['name']), c.lock(c.connection_key(m)):
        # Recover before SSH/building: a live or orphaned debugger must also
        # protect the frozen ELF and a guest whose CPUs may be paused.
        with c.lock('run-' + r['name']):
            r = reconcile_unlocked(c.get('runs', r['name']))
        if m.get('gdb'):
            check_endpoint(m)
        observed = machines.inspect(m)
        status = machines.match(k, observed)
        if status == 'mismatch':
            c.fail('Running kernel differs from the assigned kernel. Inspect/adopt or explicitly prepare the machine.')
        if observed.get('modules_disabled') == '1':
            c.fail('Target has disabled module loading.')
        if debug and not m.get('gdb'):
            c.fail('No debugger endpoint configured. Build/run remain available.')
        if debug:
            minimum = c.option('MIN_DEBUG', 'module')
            if minimum not in MODES:
                c.fail('MIN_DEBUG must be module, kernel-symbol, or kernel-source.')
            tier = kernels.characteristics(k)['tier']
            if tier < MODES[minimum]:
                c.fail(f'Requested {minimum}; kernel only has tier {tier}.')
        destination = root(r) / 'mapple.ko'
        build(k, publish=not c.option('MACHINE') and not c.option('KERNEL'), freeze=destination)
        if r['kernel_id'] != k['id'] or r['machine_id'] != m['id']:
            c.fail('Frozen run no longer matches machine/kernel records.')
        with c.lock('run-' + r['name']):
            if r['status'] == 'debugging':
                c.fail('Run is currently attached to a debugger.')
            signer = c.option('SIGNER') or k.get('signer')
            if signer:
                c.run([signer, destination, c.record('kernels', k['name']), c.record('machines', m['name'])], timeout=120)
            r['artifact_sha256'] = c.digest(destination)
            r['boot_id'], r['match'] = observed['boot_id'], status
            if status != 'verified':
                print('Kernel identity: release/architecture match only; no matching runtime build ID available.')
            mode = min(tier, 3) if debug else 0
            offset = kernel_offset(m, k, observed) if debug and mode >= 2 else None
            if debug and mode >= 2 and offset is None:
                if MODES[minimum] >= 2:
                    c.fail('Cannot establish Linux runtime relocation. Provide accessible kernel addresses or boot with nokaslr.')
                print('Linux relocation unavailable; using module debugging with raw kernel assembly.')
                mode = 1
            if debug:
                check_endpoint(m)
            remote = '/tmp/mapple-' + r['name']
            c.ssh(m, 'mkdir -p ' + shlex.quote(remote))
            c.send(m, destination, remote + '/mapple.ko')
            try:
                remote_sha = c.ssh(m, 'sha256sum ' + remote + '/mapple.ko').stdout.decode().split()[0]
            except subprocess.CalledProcessError:
                remote_sha = __import__('hashlib').sha256(c.ssh(m, 'cat ' + remote + '/mapple.ko').stdout).hexdigest()
            if remote_sha != r['artifact_sha256']:
                c.fail('Uploaded module checksum mismatch.')
            params = shlex.split(c.option('MODULE_PARAMS', ''))
            if not all(re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_,.:/+%-]*', p) for p in params):
                c.fail('Module parameters must be key=value without shell syntax.')
            # Run-only loading never requests section addresses or debug data.
            machines.invalidate_runs(m['name'])
            c.ssh(m, 'if [ -d /sys/module/mapple ]; then rmmod mapple; fi; insmod ' + remote + '/mapple.ko ' + shlex.join(params),
                  sudo=True, timeout=60)
            # A workspace reuses its run ID across Restart/F5. Distinguish each
            # load even if the ELF, boot and allocated section addresses repeat.
            receipt = f'{r["name"]} {r["artifact_sha256"]} {r["boot_id"]} {uuid.uuid4().hex}'
            c.ssh(m, "umask 077; printf '%s\\n' " + shlex.quote(receipt) +
                  ' > /tmp/mapple-receipt-' + r['name'] + '; mv -f /tmp/mapple-receipt-' + r['name'] +
                  ' /tmp/mapple-loaded-receipt', sudo=True)
            r['load_receipt'] = receipt
            r.update(status='loaded', loaded_at=c.now(), debug_mode=next((n for n, t in MODES.items() if t == mode), None))
            if debug:
                output = c.ssh(m, 'for f in /sys/module/mapple/sections/.* /sys/module/mapple/sections/*; do '
                               '[ -f "$f" ] || continue; printf "%s %s\\n" "${f##*/}" "$(cat "$f")"; done', sudo=True).stdout.decode()
                sections = {}
                for line in output.splitlines():
                    fields = line.split()
                    if len(fields) == 2 and re.fullmatch(r'0x[0-9a-fA-F]+', fields[1]) and int(fields[1], 16):
                        sections[fields[0]] = fields[1]
                if '.text' not in sections:
                    c.fail('Module loaded, but readable nonzero section addresses are unavailable for debugging.')
                r['sections'] = sections
                try:
                    r['module_build_id'] = kernels.notes_id(c.ssh(m, 'cat /sys/module/mapple/notes/.note.gnu.build-id', sudo=True).stdout)
                except subprocess.CalledProcessError:
                    r['module_build_id'] = None
                r['kernel_offset'] = offset
                debug_script(r, k)
                r['status'] = 'prepared'
            c.save(c.record('runs', r['name']), r)
            (root(r) / 'deployment.log').write_text(f'{m["name"]}: loaded {r["artifact_sha256"]}; {status}\n')
    print(f'{r["name"]}: {m["name"]} / {k["name"]}; ' + (r['debug_mode'] if debug else 'loaded without debugger'))
    return r


def check_endpoint(m):
    endpoint = m.get('gdb', '')
    serial = endpoint.startswith('/')
    if serial:
        device = Path(endpoint)
        if not re.fullmatch(r'[A-Za-z0-9_./-]+', endpoint) or not device.is_char_device() or not os.access(device, os.R_OK | os.W_OK):
            c.fail('Serial GDB endpoint must be a readable/writable character device on the host.')
    else:
        if not re.fullmatch(r'[A-Za-z0-9_.:%\[\]-]+:[0-9]+', endpoint):
            c.fail('GDB_ENDPOINT must be host:port or an absolute serial-device path.')
        if not 1 <= int(endpoint.rsplit(':', 1)[1]) <= 65535:
            c.fail('GDB endpoint port must be between 1 and 65535.')
    key = 'endpoint-' + __import__('hashlib').sha256(endpoint.encode()).hexdigest()[:24]
    with c.lock(key):
        pass
    # Inspect local TCP state without connecting to (and pausing) a GDB stub.
    if not serial and shutil.which('ss'):
        result = subprocess.run(['ss', '-H', '-tn', 'dst', endpoint], capture_output=True, text=True)
        if result.returncode == 0 and 'ESTAB' in result.stdout:
            c.fail('A debugger already owns this endpoint. Detach it before reloading or attaching.')
    return key


def kernel_offset(m, k, observed):
    if 'nokaslr' in observed['cmdline'].split():
        return 0
    elf = kernels.debug_elf(k)
    if elf:
        local = c.run([kernels.binary_tool(k, 'nm'), '-n', elf], capture_output=True, env=kernels.toolenv(k)).stdout.decode(errors='replace')
    elif kernels.path(k, 'system_map'):
        local = kernels.path(k, 'system_map').read_text()
    else:
        return None
    try:
        remote = c.ssh(m, "grep -E ' (_text|_stext)$' /proc/kallsyms", sudo=True).stdout.decode()
    except subprocess.CalledProcessError:
        return None
    for symbol in ('_text', '_stext'):
        a = re.search(r'(?m)^([0-9a-fA-F]+) \w ' + symbol + '$', local)
        b = re.search(r'(?m)^([0-9a-fA-F]+) \w ' + symbol + '$', remote)
        if a and b and int(b[1], 16):
            return int(b[1], 16) - int(a[1], 16)
    return None


def debug_script(r, k):
    directory = root(r)
    module = directory / 'mapple.ko'
    relocated = directory / 'mapple-relocated.ko'
    args = [kernels.binary_tool(k, 'objcopy'), '--no-change-warnings']
    # sh_addr relocation preserves source line breakpoints with cppdbg.
    sections = c.run([kernels.binary_tool(k, 'objdump'), '-h', module], capture_output=True, env=kernels.toolenv(k)).stdout.decode()
    names = {line.split()[1] for line in sections.splitlines() if line.split() and line.split()[0].isdigit()}
    for sec, address in r['sections'].items():
        if sec in names:
            args.append(f'--change-section-address={sec}={address}')
    c.run([*args, module, relocated], env=kernels.toolenv(k))
    lines = ['set confirm off', 'set pagination off', 'set breakpoint pending on', 'set mi-async on',
             'set print pretty on', 'set print thread-events off', 'set schedule-multiple on',
             'set remotetimeout 60', 'set tcp connect-timeout 60',
             'define kill', '  detach', 'end',
             f'directory "{c.ROOT / "kernel"}"',
             f'file "{relocated}"']
    # Initialize architecture from our ELF before reading minimal Linux symbols.
    # symbol-file alone leaves native GDB in its default 32-bit architecture.
    if r['debug_mode'] != 'module':
        elf = kernels.debug_elf(k)
        if elf:
            helper = None
            if kernels.elf_info(elf).get('dwarf'):
                candidates = [Path(str(elf) + '-gdb.py')]
                candidates += [tree / 'vmlinux-gdb.py' for key in ('build', 'source')
                               if (tree := kernels.path(k, key))]
                helper = next((p for p in candidates if p.is_file() and
                               p.resolve().is_relative_to(kernels.root(k).resolve())), None)
            if helper:
                # Trust only this kernel's loader, before symbol-file can auto-load it.
                # This command takes the entire path verbatim, including spaces.
                lines.append(f'add-auto-load-safe-path {helper.resolve()}')
            lines.append(f'symbol-file -o {r["kernel_offset"] or 0} "{elf}"')
            if helper and helper != Path(str(elf) + '-gdb.py'):
                # Separate debug ELFs are commonly renamed; GDB won't discover
                # vmlinux-gdb.py beside them. Keep the loader's original path so
                # its relative scripts/gdb imports still work.
                lines.append(f'source {helper}')
        else:
            elf = directory / 'kernel-symbols.elf'
            make_symbol_elf(k, elf, r['kernel_offset'] or 0)
            lines.append(f'symbol-file "{elf}"')
        r['program'] = str(elf)
        source = kernels.path(k, 'source')
        if source and kernels.debug_elf(k):
            symbol = c.run([kernels.binary_tool(k, 'nm'), kernels.debug_elf(k)], capture_output=True, env=kernels.toolenv(k)).stdout.decode(errors='replace')
            entry = re.search(r'(?m)^([0-9a-f]+) \w start_kernel$', symbol)
            if entry:
                location = c.run([kernels.binary_tool(k, 'addr2line'), '-e', kernels.debug_elf(k), entry[1]], capture_output=True, env=kernels.toolenv(k)).stdout.decode().strip().split(':')[0]
                if location.endswith('/init/main.c'):
                    lines.append(f'set substitute-path "{location[:-len("/init/main.c")]}" "{source}"')
            lines.append(f'directory "{source}"')
    else:
        r['program'] = str(relocated)
    lines.append(f'target remote {r["machine_snapshot"]["gdb"]}')
    if r['debug_mode'] != 'module':
        lines.append(f'add-symbol-file "{relocated}" {r["sections"][".text"]}')
    (directory / 'debug.gdb').write_text('\n'.join(lines) + '\n')


def make_symbol_elf(k, output, offset):
    """Turn text-map function addresses into bounded ELF code symbols.

    Keep architecture/endianness from our actual module. Section-relative values
    avoid objcopy's signed absolute-symbol limit for high kernel addresses.
    NOBITS sections give GDB address ranges without allocating kernel-sized data.
    """
    import struct
    template = (output.parent / 'mapple.ko').read_bytes()
    if template[:4] != b'\x7fELF' or template[4] not in (1, 2):
        c.fail('Module is not a supported ELF file.')
    wide = template[4] == 2
    order = '<' if template[5] == 1 else '>'
    header_format = order + ('HHIQQQIHHHHHH' if wide else 'HHIIIIIHHHHHH')
    section_format = order + ('IIQQQQIIQQ' if wide else 'IIIIIIIIII')
    header = list(struct.unpack_from(header_format, template, 16))
    header_size = 16 + struct.calcsize(header_format)
    section_size = struct.calcsize(section_format)
    strings = b'\0.text\0.shstrtab\0'
    section_offset = (header_size + len(strings) + 7) & ~7
    header[0], header[3], header[4], header[5] = 1, 0, 0, section_offset
    header[7:] = [header_size, 0, 0, section_size, 3, 2]
    content = bytearray(template[:16] + struct.pack(header_format, *header))
    content += strings + bytes(section_offset - len(content) - len(strings))
    for fields in ([0] * 10, [1, 1, 6, 0, header_size, 0, 0, 0, 16, 0],
                   [7, 3, 0, 0, header_size, len(strings), 0, 0, 1, 0]):
        content += struct.pack(section_format, *fields)
    empty = output.with_suffix('.empty')
    empty.write_bytes(content)
    raw = output.with_suffix('.raw')
    raw.touch()
    symbols = []
    groups = {}
    for line in kernels.path(k, 'system_map').read_text().splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[1] not in ('t', 'T') or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.$]*', fields[2]):
            continue
        address = int(fields[0], 16) + offset
        base = address & ~((1 << 28) - 1)
        section = groups.setdefault(base, ['.text' if not groups else '.text' + str(len(groups)), 0])
        section[1] = max(section[1], address - base + 4096)
        symbols.append((fields[2], section[0], address - base))
    if not symbols:
        c.fail('System.map contains no usable text symbols.')
    arguments = []
    for base, (section, size) in groups.items():
        if section != '.text':
            arguments += ['--add-section', f'{section}={raw}', '--set-section-flags', f'{section}=alloc,code']
        arguments += ['--change-section-address', f'{section}=0x{base:x}']
    for name, section, value in symbols:
        arguments += ['--add-symbol', f'{name}={section}:0x{value:x},function,global']
    response = output.with_suffix('.args')
    response.write_text('\n'.join(arguments + [str(empty), str(output)]))
    tool = kernels.binary_tool(k, 'objcopy')
    c.run([tool, '@' + str(response)], env=kernels.toolenv(k))
    data = bytearray(output.read_bytes())
    header = struct.unpack_from(header_format, data, 16)
    headers = [list(struct.unpack_from(section_format, data, header[5] + i * header[10])) for i in range(header[11])]
    string_header = headers[header[12]]
    names = data[string_header[4]:string_header[4] + string_header[5]]
    for i, fields in enumerate(headers):
        name = bytes(names[fields[0]:]).split(b'\0', 1)[0].decode()
        group = next(((base, size) for base, (section, size) in groups.items() if section == name), None)
        if group:
            fields[1], fields[2], fields[3], fields[5] = 8, 6, group[0], group[1]
            struct.pack_into(section_format, data, header[5] + i * header[10], *fields)
    output.write_bytes(data)


def find_run():
    if c.option('RUN'):
        return c.get('runs', c.option('RUN'))
    m = c.machine()
    candidates = []
    for r in c.records('runs'):
        if r['kind'] == 'module' and r['machine'] == m['name']:
            if r['status'] == 'debugging':
                r = finish_debug(r)
            if r['status'] in ('prepared', 'detached'):
                candidates.append(r)
    if len(candidates) != 1:
        c.fail('Choose a prepared run explicitly with RUN=<id>; inspect make debug-list.')
    return candidates[0]


def attach(r, args=()):
    m = r['machine_snapshot']
    current_m = c.get('machines', r['machine'])
    current_k = c.get('kernels', r['kernel'])
    if current_m['id'] != r['machine_id'] or current_k['id'] != r['kernel_id']:
        c.fail('Machine/kernel slot was replaced; prepare a new run.')
    kernels.validate(current_k)
    with c.lock('machine-' + m['name']) as machine_lock, c.lock(c.connection_key(m)) as connection_lock, \
         c.lock('run-' + r['name']) as run_lock, c.lock(check_endpoint(m)) as endpoint_lock:
        r = reconcile_unlocked(c.get('runs', r['name']))
        if r['status'] not in ('prepared', 'detached'):
            c.fail('Run is not prepared; use module-debug again.')
        observed = machines.inspect(m, save=False)
        if observed['boot_id'] != r['boot_id']:
            c.fail('Machine rebooted; prepare a new debug run.')
        receipt = c.ssh(m, 'cat /tmp/mapple-loaded-receipt', sudo=True).stdout.decode().strip()
        if receipt != r.get('load_receipt'):
            c.fail('Another run reloaded this module. Prepare a new debug run.')
        if r.get('module_build_id'):
            current_id = kernels.notes_id(c.ssh(m, 'cat /sys/module/mapple/notes/.note.gnu.build-id', sudo=True).stdout)
            if current_id != r['module_build_id']:
                c.fail('Loaded module build ID changed. Prepare a new debug run.')
        # Confirm reloads have not moved the module, even within the same boot.
        current = c.ssh(m, 'cat /sys/module/mapple/sections/.text', sudo=True).stdout.decode().strip()
        if current != r['sections']['.text']:
            c.fail('Module was reloaded; prepare fresh runtime addresses.')
        r.update(status='debugging', supervisor_identity=process_identity(os.getpid()), debugger_identity=None)
        r.pop('debugger_pid', None)
        c.save(c.record('runs', r['name']), r)
        process = None
        try:
            gdb = c.option('GDB', 'gdb-multiarch' if k_arch(r) != kernels.normalize(os.uname().machine) else 'gdb')
            command = [c.executable(gdb), '-q']
            if not any('--interpreter' in arg for arg in args):
                command += ['-x', str(root(r) / 'debug.gdb')]
            command += list(args)
            # GDB retains ownership if VS Code kills this supervising process.
            # OS locks survive until GDB also exits, even before its PID is saved.
            process = subprocess.Popen(command, cwd=c.ROOT,
                                       pass_fds=(machine_lock, connection_lock, run_lock, endpoint_lock))
            r['debugger_pid'] = process.pid
            r['debugger_identity'] = process_identity(process.pid)
            c.save(c.record('runs', r['name']), r)
            previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP)}
            for sig in previous:
                signal.signal(sig, lambda signum, frame: process.terminate() if process.poll() is None else None)
            try:
                while True:
                    try:
                        code = process.wait()
                        break
                    except KeyboardInterrupt:
                        # Ctrl+C interrupts GDB/the guest, not this supervising lock.
                        continue
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            if code:
                c.fail('GDB exited with status ' + str(code))
        finally:
            # Setup/recording errors must not release ownership of a GDB child
            # that is still running. Forced launcher death is covered by its FDs.
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            r.update(status='detached', detached_at=c.now())
            c.save(c.record('runs', r['name']), r)


def k_arch(r):
    return c.get('kernels', r['kernel'])['arch']

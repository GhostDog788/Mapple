"""SSH machines, read-only adoption, managed QEMU and explicit kernel preparation."""
import getpass
import grp
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import socket
import subprocess
import time
import urllib.request
import uuid
from . import core as c, kernels

IMAGE_BASE = "https://cloud-images.ubuntu.com/releases/noble/release"
IMAGE_NAME = "ubuntu-24.04-server-cloudimg-amd64.img"
RESERVED_PORTS = set()
fail, run, save, ssh, wait_ssh, validate_name = c.fail, c.run, c.save, c.ssh, c.wait_ssh, c.name

def slot_path(name):
    return c.record('machines', name)

def invalidate_runs(name):
    for r in c.records('runs'):
        if r.get('machine') == name and r.get('kind') == 'module':
            r['status'] = 'stale'
            c.save(c.record('runs', r['name']), r)

def select(name):
    with c.lock('editor'):
        m = c.get('machines', name)
        c.directory(c.STATE)
        temporary = c.STATE / ('selection-' + uuid.uuid4().hex)
        temporary.write_text(name + '\n')
        temporary.replace(c.STATE / 'active-machine')
        from . import runs
        runs.publish_navigation_unlocked(m)
    print('Active machine: ' + name)


def free_port(preferred):
    reserved = set(RESERVED_PORTS)
    for path in (c.STATE / "machines").glob("*/record.json"):
        slot = json.loads(path.read_text())
        if slot.get("managed"):
            reserved.update((slot["ssh_port"], slot["module_port"], int(slot["gdb"].rsplit(":", 1)[1])))
        if slot["host"] in ("127.0.0.1", "localhost", "::1"):
            reserved.add(slot.get("ssh_port", 22))
            reserved.add(slot.get("module_port", 4444))
        endpoint = slot.get("gdb", "")
        if endpoint.startswith(("127.0.0.1:", "localhost:")):
            reserved.add(int(endpoint.rsplit(":", 1)[1]))
    reserved.update(r["port"] for r in c.records("runs") if r.get("kind") == "server" and r.get("status") in ("configured", "running"))
    with socket.socket() as sock:
        try:
            if preferred in reserved:
                raise OSError("port belongs to another slot")
            sock.bind(("127.0.0.1", preferred))
        except OSError:
            sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    if port in reserved:
        return free_port(0)
    RESERVED_PORTS.add(port)
    return port

def create_vm():
    name = c.name(c.option('NAME'))
    vm = slot_path(name).parent / 'vm'
    if slot_path(name).exists() or vm.exists():
        c.fail('Machine or VM assets already exist; choose another NAME.')
    try:
        return create_owned_vm()
    except BaseException:
        if not slot_path(name).exists():
            shutil.rmtree(vm, ignore_errors=True)
        raise


def create_owned_vm():
    kernel = c.get("kernels", c.option("KERNEL") or c.fail("Supply KERNEL=<slot>"))
    if kernel["arch"] != "x86_64":
        c.fail("Managed QEMU creation uses x86_64 Ubuntu. Register other architectures with machine-add.")
    name = validate_name(c.option("NAME"))
    if slot_path(name).exists() or (c.STATE / "machines" / name / "vm").exists():
        fail(f"Slot or VM directory {name} already exists; choose a new NAME.")
    for tool in ("cloud-localds", "qemu-img", "qemu-system-x86_64", "ssh-keygen"):
        if not shutil.which(tool):
            fail(f"Missing {tool}; run make setup.")
    images = c.STATE / "downloads/images"
    images.mkdir(parents=True, exist_ok=True)
    print("Fetching Ubuntu 24.04 image checksums…", flush=True)
    with urllib.request.urlopen(IMAGE_BASE + "/SHA256SUMS", timeout=60) as response:
        sums = response.read().decode()
    checksum = next((line.split()[0] for line in sums.splitlines()
                     if len(line.split()) == 2 and line.split()[-1].lstrip("*") == IMAGE_NAME), None)
    if not checksum:
        fail("Ubuntu image checksum is missing from SHA256SUMS.")
    image = images / f"{checksum}.qcow2"
    if not image.exists() or c.digest(image) != checksum:
        print("Downloading Ubuntu cloud image (first use only)…", flush=True)
        temporary = images / f"{checksum}.part"
        with urllib.request.urlopen(IMAGE_BASE + "/" + IMAGE_NAME, timeout=120) as response, temporary.open("wb") as out:
            shutil.copyfileobj(response, out)
        with temporary.open("rb") as downloaded:
            image_hash = hashlib.file_digest(downloaded, "sha256").hexdigest()
        if image_hash != checksum:
            temporary.unlink()
            fail("Ubuntu image checksum mismatch; retry make machine-create.")
        temporary.replace(image)
    vm = c.STATE / "machines" / name / "vm"
    vm.mkdir(parents=True, mode=0o700)
    key = vm / "id_ed25519"
    run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key])
    run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b", image,
         vm / "disk.qcow2", os.environ.get("DISK") or "30G"])
    userdata = {"hostname": f"mapple-{name}", "ssh_pwauth": False,
                "users": [{"name": "mapple", "shell": "/bin/bash", "sudo": "ALL=(ALL) NOPASSWD:ALL",
                           "ssh_authorized_keys": [key.with_suffix(".pub").read_text().strip()]}]}
    (vm / "user-data").write_text("#cloud-config\n" + json.dumps(userdata))
    (vm / "meta-data").write_text(json.dumps({"instance-id": f"mapple-{name}", "local-hostname": f"mapple-{name}"}))
    run(["cloud-localds", vm / "seed.img", vm / "user-data", vm / "meta-data"])
    config = {"host": "127.0.0.1", "user": "mapple", "ssh_key": str(key),
              "ssh_port": free_port(2222), "module_port": free_port(4444),
              "gdb": f"127.0.0.1:{free_port(1234)}", "method": "qemu", "managed": True,
              "memory": int(os.environ.get("MEMORY") or 4096), "cpus": int(os.environ.get("CPUS") or 2),
              "image_sha256": checksum}
    config.update(name=name, id=uuid.uuid4().hex, requested_kernel=kernel["name"], kernel=None, created_at=c.now())
    if (c.STATE / "known_hosts").exists():
        run(["ssh-keygen", "-q", "-R", f"[127.0.0.1]:{config["ssh_port"]}", "-f", c.STATE / "known_hosts"], capture_output=True)
    save(slot_path(name), config)
    if not (c.STATE / "active-machine").exists():
        select(name)
    print(f"Created {name}. Run make machine-use NAME={name}, make machine-start, then make machine-prepare.")

def kvm_group_member():
    try:
        return pwd.getpwuid(os.getuid()).pw_name in grp.getgrnam("kvm").gr_mem
    except KeyError:
        return False

def monitor_command(path, command):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(str(path))

        def prompt():
            response = bytearray()
            while not response.endswith(b"(qemu) "):
                data = sock.recv(4096)
                if not data:
                    fail("QEMU monitor closed before acknowledging the command.")
                response.extend(data)
            return bytes(response)

        prompt()  # Drain the greeting and wait for QEMU to accept input.
        sock.sendall((command + "\n").encode())
        return prompt()

def vm_command(name, config, action):
    if not config.get("managed"):
        fail("This slot is an existing VM. Start/stop it with its QEMU launcher; make machine-ssh still works.")
    vm = c.STATE / "machines" / name / "vm"
    monitor = vm / "monitor.sock"
    if action == "machine-stop":
        monitor_command(monitor, "system_powerdown")
        invalidate_runs(name)
        print("Guest shutdown requested. Use make machine-show to check it has stopped.")
        return
    if monitor.exists():
        try:
            with socket.socket(socket.AF_UNIX) as sock:
                sock.connect(str(monitor))
            print(f"VM {name} is already running.")
            wait_ssh(config)
            return
        except (ConnectionRefusedError, FileNotFoundError):
            monitor.unlink(missing_ok=True)
    invalidate_runs(name)
    accelerated = os.access("/dev/kvm", os.R_OK | os.W_OK)
    # usermod updates /etc/group, but this shell retains its old groups until
    # login. sg lets the very next make machine-start use KVM without restarting it.
    refresh_group = (not accelerated and Path("/dev/kvm").exists()
                     and kvm_group_member() and shutil.which("sg"))
    accelerated = accelerated or bool(refresh_group)
    args = ["qemu-system-x86_64", "-name", f"mapple-{name}", "-machine", "q35",
            "-accel", "kvm" if accelerated else "tcg", "-cpu", "host" if accelerated else "max",
            "-m", str(config["memory"]), "-smp", str(config["cpus"]),
            "-drive", f"file={vm / 'disk.qcow2'},if=virtio,format=qcow2",
            "-drive", f"file={vm / 'seed.img'},if=virtio,format=raw,readonly=on",
            "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{config['ssh_port']}-:22,hostfwd=tcp:127.0.0.1:{config['module_port']}-:4444",
            "-device", "virtio-net-pci,netdev=net0", "-gdb", "tcp:" + config["gdb"],
            "-display", "none", "-serial", f"file:{vm / 'console.log'}", "-monitor",
            f"unix:{monitor},server=on,wait=off", "-pidfile", vm / "qemu.pid", "-daemonize"]
    if refresh_group:
        run(["sg", "kvm", "-c", shlex.join(str(a) for a in args)])
    else:
        run(args)
    print(f"Waiting for {name} SSH; console: {vm / 'console.log'}", flush=True)
    wait_ssh(config)
    ssh(config, "cloud-init status --wait", timeout=600)


def inspect(m, save=True):
    command = '''uname -r
uname -m
cat /proc/sys/kernel/random/boot_id
cat /proc/cmdline
cat /proc/sys/kernel/modules_disabled 2>/dev/null || echo unknown
cat /etc/os-release 2>/dev/null || true
'''
    lines = c.ssh(m, command).stdout.decode(errors='replace').splitlines()
    if len(lines) < 5:
        c.fail('Incomplete machine inspection response.')
    observation = dict(zip(('release', 'arch', 'boot_id', 'cmdline', 'modules_disabled'), lines[:5]))
    observation['arch'] = kernels.normalize(observation['arch'])
    observation['os_release'] = '\n'.join(lines[5:])
    observation['at'] = c.now()
    try:
        observation['build_id'] = kernels.notes_id(c.ssh(m, 'cat /sys/kernel/notes').stdout)
    except subprocess.CalledProcessError:
        observation['build_id'] = None
    if save:
        m['observed'] = observation
        c.save(slot_path(m['name']), m)
    return observation


def observed_kernel(m, observed):
    if not c.option('KERNEL'):
        candidates = [k for k in c.records('kernels') if match(k, observed) == 'verified']
        if candidates:
            chosen = max(candidates, key=lambda k: (k['name'] == m.get('kernel'), k.get('capabilities', {}).get('tier', 0)))
            print('Recognized kernel: ' + chosen['name'])
            return chosen
    name = c.name(c.option('KERNEL') or (m['name'] + '-observed'))
    if c.record('kernels', name).exists():
        k = c.get('kernels', name)
        if k.get('release') != observed['release'] or k['arch'] != observed['arch']:
            c.fail('Observed kernel changed. Choose a new KERNEL name for adoption.')
        expected = k.get('capabilities', {}).get('build_id') or k.get('expected_build_id')
        if expected and observed.get('build_id') and expected != observed['build_id']:
            c.fail('Observed build ID changed. Choose a new KERNEL name.')
        return k
    k = {'name': name, 'id': uuid.uuid4().hex, 'origin': 'observed', 'state': 'incomplete',
         'release': observed['release'], 'arch': observed['arch'],
         'linux_arch': c.option('LINUX_ARCH', kernels.ARCHES.get(observed['arch'], (observed['arch'],))[0]),
         'expected_build_id': observed.get('build_id'), 'toolchain': {
             'cc': c.option('CC', m.get('compiler', 'gcc')), 'cross_compile': c.option('CROSS_COMPILE', m.get('cross_compile', ''))},
         'artifacts': {}, 'created_at': c.now(), 'provenance': [{'machine': m['name'], **observed}]}
    return kernels.seal(k)


def add():
    name = c.name(c.option('NAME'))
    host = c.option('HOST') or input('Machine address: ').strip()
    user = c.option('SSH_USER') or input('SSH user: ').strip()
    if not re.fullmatch(r'[A-Za-z0-9_.:%-]+', host) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]*', user):
        c.fail('Invalid SSH host/user.')
    with c.lock('catalog'):
        if slot_path(name).exists():
            c.fail('Machine already exists.')
        m = {'name': name, 'id': uuid.uuid4().hex, 'host': host, 'user': user,
             'ssh_port': int(c.option('SSH_PORT', '22')), 'module_port': int(c.option('MODULE_PORT', '4444')),
             'gdb': c.option('GDB_ENDPOINT', ''), 'created_at': c.now(),
             'compiler': c.option('CC', 'gcc'), 'cross_compile': c.option('CROSS_COMPILE', '')}
        if c.option('SSH_KEY'):
            m['ssh_key'] = str(Path(c.option('SSH_KEY')).expanduser().resolve(strict=True))
        if c.option('ASK_PASS') == '1':
            m['ssh_password'] = getpass.getpass('SSH password (empty for key): ')
            m['sudo_password'] = getpass.getpass('Sudo password (empty to reuse SSH password): ')
        if c.option('INSTALLER'):
            m['installer'] = str(Path(c.option('INSTALLER')).expanduser().resolve(strict=True))
        for key in ('ssh_port', 'module_port'):
            if not 1 <= m[key] <= 65535:
                c.fail(key + ' must be between 1 and 65535.')
        c.save(slot_path(name), m)
        try:
            observed = inspect(m)
            k = observed_kernel(m, observed)
            m['kernel'] = k['name']
            c.save(slot_path(name), m)
        except (subprocess.SubprocessError, OSError) as error:
            m['inspection_error'] = str(error)
            c.save(slot_path(name), m)
            print('Registered; inspection unavailable. Start it and run machine-inspect.')
        if not (c.STATE / 'active-machine').exists():
            select(name)
    print('Registered ' + name + ' without changing its kernel.')


def adoption(m):
    with c.lock('machine-' + m['name']), c.lock(c.connection_key(m)):
        observed = inspect(m)
        with c.lock('catalog'):
            k = observed_kernel(m, observed)
        with c.lock('kernel-' + k['name']):
            current = c.get('kernels', k['name'])
            if current['id'] != k['id']:
                c.fail('Kernel slot changed during adoption; inspect and try again.')
            if current.get('hashes'):
                kernels.validate(current)
            return adopt_artifacts(m, observed, current)


def adopt_artifacts(m, observed, k):
    directory = c.directory(kernels.root(k) / ('adoption-' + uuid.uuid4().hex[:8]))
    release = observed['release']
    if not re.fullmatch(r'[A-Za-z0-9._+-]+', release):
        c.fail('Unsupported kernel release characters.')
    # Copy pre-existing target files first. Adoption never installs packages
    # or changes the target boot configuration.
    remote = '/tmp/mapple-adopt-' + uuid.uuid4().hex
    c.ssh(m, 'mkdir -p ' + shlex.quote(remote))
    try:
        if not kernels.path(k, 'build'):
            probe = c.ssh(m, f'readlink -f /lib/modules/{shlex.quote(release)}/build || true').stdout.decode().strip()
            have = probe and c.ssh(m, 'test -f ' + shlex.quote(probe + '/Makefile') + ' && echo yes || true').stdout.strip()
            if have:
                # tar -h closes /usr/src/common links in distribution headers.
                # Ubuntu headers may contain a dangling optional Rust SDK
                # link. Mapple is C; do not dereference that unrelated SDK.
                packed = c.ssh(m, f'tar -chzf {remote}/headers.tar.gz --exclude=./rust -C {shlex.quote(probe)} .', timeout=300)
                if packed.stderr:
                    print('Header snapshot warnings: ' + packed.stderr.decode(errors='replace').strip())
                archive = directory / 'headers.tar.gz'
                c.receive(m, remote + '/headers.tar.gz', archive)
                with __import__('tarfile').open(archive) as tar:
                    tar.extractall(c.directory(directory / 'build'), filter='data')
                k['artifacts']['build'] = str((directory / 'build').relative_to(kernels.root(k)))
            elif c.option('OFFLINE') != '1':
                # Download on the target using its configured package sources;
                # extract on the host, leaving installed packages unchanged.
                common = re.sub(r'-(generic|lowlatency|amd64|arm64)$', '', release)
                command = (f'cd {remote}; command -v apt-get >/dev/null && '
                           f'apt-get download linux-headers-{shlex.quote(release)} linux-headers-{shlex.quote(common)} '
                           f'> download.log 2>&1 && tar -czf packages.tar.gz ./*.deb')
                try:
                    c.ssh(m, command, timeout=300)
                    archive = directory / 'packages.tar.gz'
                    c.receive(m, remote + '/packages.tar.gz', archive)
                    packages = c.directory(directory / 'packages')
                    with __import__('tarfile').open(archive) as tar:
                        tar.extractall(packages, filter='data')
                    extracted = c.directory(directory / 'extracted')
                    for deb in packages.glob('*.deb'):
                        c.run(['dpkg-deb', '-x', deb, extracted])
                    candidates = list((extracted / 'usr/src').glob('*' + release + '*'))
                    if not candidates:
                        c.fail('Downloaded packages did not contain matching prepared headers.')
                    # Debian headers can link to a sibling common package,
                    # or use /usr/src absolute links. Rebase the package root
                    # before taking a closed, independent SDK snapshot.
                    for file in extracted.rglob('*'):
                        if file.is_symlink():
                            target = Path(os.readlink(file))
                            relocated = extracted / str(target).lstrip('/')
                            if target.is_absolute() and (relocated.exists() or relocated.is_symlink()):
                                file.unlink()
                                file.symlink_to(os.path.relpath(relocated, file.parent))
                        elif file.is_file() and (file.name == 'Makefile' or file.name.endswith('.cmd')):
                            raw = file.read_bytes()
                            file.write_bytes(raw.replace(b'/usr/src/', str(extracted / 'usr/src').encode() + b'/'))
                    owned = directory / 'build'
                    c.copy_tree(candidates[0], owned)
                    k['artifacts']['build'] = str(owned.relative_to(kernels.root(k)))
                except (subprocess.SubprocessError, OSError) as error:
                    print('No matching package acquired: ' + str(error))
        for key, remote_path in (('vmlinux', '/usr/lib/debug/boot/vmlinux-' + release),
                                 ('system_map', '/boot/System.map-' + release),
                                 ('config', '/proc/config.gz'), ('btf', '/sys/kernel/btf/vmlinux')):
            if kernels.path(k, key):
                continue
            readable = c.ssh(m, 'test -r ' + shlex.quote(remote_path) + ' && echo yes || true').stdout.strip()
            if not readable:
                continue
            try:
                local = directory / key
                c.receive(m, remote_path, local)
                if key == 'vmlinux' and observed.get('build_id'):
                    if kernels.elf_info(local).get('build_id') != observed['build_id']:
                        c.fail('Target debug ELF differs from running kernel build ID.')
                k['artifacts'][key] = str(local.relative_to(kernels.root(k)))
            except subprocess.SubprocessError:
                pass
        if c.option('FETCHER'):
            c.run([str(Path(c.option('FETCHER')).resolve(strict=True)),
                   c.record('machines', m['name']), c.record('kernels', k['name']), directory], timeout=600)
            # Fetcher supplies a JSON mapping to files inside its output dir.
            additions = c.load(directory / 'artifacts.json')
            for key, value in additions.items():
                if key not in ('build', 'source', 'vmlinux', 'debug_info', 'system_map', 'image', 'modules', 'config', 'btf'):
                    c.fail('Unknown fetcher artifact: ' + key)
                candidate = (directory / value).resolve(strict=True)
                if not candidate.is_relative_to(directory.resolve()):
                    c.fail('Fetcher artifact escaped its output directory.')
                if candidate.is_dir() != (key in ('build', 'source')):
                    c.fail('Fetcher artifact has the wrong type: ' + key)
                if candidate.is_dir() and any(p.is_symlink() and not p.resolve().is_relative_to(kernels.root(k).resolve()) for p in candidate.rglob('*')):
                    c.fail('Fetcher must materialize external symlink targets in its output.')
                if key in k['artifacts']:
                    c.fail('Fetcher cannot replace existing artifacts.')
                k['artifacts'][key] = str(candidate.relative_to(kernels.root(k)))
        k = kernels.seal(k)
        m['kernel'] = k['name']
        m.pop('requested_kernel', None)
        m['match'] = match(k, observed)
        c.save(slot_path(m['name']), m)
        from . import runs
        runs.publish_navigation(m)
    finally:
        c.ssh(m, 'rm -rf ' + shlex.quote(remote))
    print(f'Adopted {k["name"]}: tier {k["capabilities"]["tier"]}; match {m["match"]}.')
    if k['capabilities']['blockers']:
        print('Missing: ' + ', '.join(k['capabilities']['blockers']))

def match(k, observed):
    if k.get('release') != observed.get('release') or k['arch'] != observed.get('arch'):
        return 'mismatch'
    expected = k.get('capabilities', {}).get('build_id') or k.get('expected_build_id')
    actual = observed.get('build_id')
    if expected and actual:
        return 'verified' if expected == actual else 'mismatch'
    return 'release-only'


def retire(m):
    if any(r.get('machine') == m['name'] and r.get('status') == 'debugging' for r in c.records('runs')):
        c.fail('Detach active debugger sessions before removing this machine.')
    with c.lock('editor', wait=30):
        record = slot_path(m['name'])
        record.rename(record.with_name('retired.json'))
        selected = c.STATE / 'active-machine'
        if selected.exists() and selected.read_text().strip() == m['name']:
            selected.unlink()
            (c.ROOT / 'compile_commands.json').unlink(missing_ok=True)
    print('Removed machine record; preserved VM assets: ' + m['name'])


def remove(m):
    with c.lock('machine-' + m['name']):
        retire(m)


def owned_process(m):
    """Inspect only a process whose command names this owned VM disk."""
    if not m.get('managed'):
        return None
    vm = slot_path(m['name']).parent / 'vm'
    try:
        pid = int((vm / 'qemu.pid').read_text().strip())
        if str(vm / 'disk.qcow2').encode() in Path(f'/proc/{pid}/cmdline').read_bytes():
            return pid
    except (OSError, ValueError):
        pass
    return None


def destroy(m):
    if not m.get('managed'):
        c.fail('machine-destroy only deletes QEMU VMs created by this repository.')
    with c.lock('machine-' + m['name']), c.lock(c.connection_key(m)):
        vm = slot_path(m['name']).parent / 'vm'
        monitor = vm / 'monitor.sock'
        pid = owned_process(m)
        if monitor.exists():
            try:
                monitor_command(monitor, 'quit')
            except (OSError, RuntimeError):
                pass
        # Do not unlink an image still held by a live QEMU process. A stale
        # monitor/socket is not evidence that the VM exited. Never kill by PID.
        deadline = time.monotonic() + 15
        while pid and time.monotonic() < deadline:
            try:
                command = Path(f'/proc/{pid}/cmdline').read_bytes()
            except FileNotFoundError:
                break
            if str(vm / 'disk.qcow2').encode() not in command:
                break
            time.sleep(.1)
        else:
            if pid:
                c.fail('QEMU has not exited; VM assets retained. Stop it and retry machine-destroy.')
        shutil.rmtree(vm, ignore_errors=True)
        invalidate_runs(m['name'])
    remove(m)


def configure(m):
    """Update connection/debug settings, never the kernel association."""
    with c.lock('machine-' + m['name']):
        if m.get('managed') and any(c.option(key) for key in ('HOST', 'SSH_USER', 'SSH_PORT', 'SSH_KEY', 'MODULE_PORT', 'GDB_ENDPOINT')):
            c.fail('QEMU connection settings are owned by machine-create. Configure an existing-machine record for a custom connection.')
        for option, key in (('HOST', 'host'), ('SSH_USER', 'user'), ('GDB_ENDPOINT', 'gdb')):
            value = c.option(option)
            if value:
                m[key] = '' if option == 'GDB_ENDPOINT' and value == 'none' else value
        if not re.fullmatch(r'[A-Za-z0-9_.:%-]+', m['host']) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]*', m['user']):
            c.fail('Invalid SSH host/user.')
        for option, key in (('SSH_PORT', 'ssh_port'), ('MODULE_PORT', 'module_port')):
            if c.option(option):
                value = int(c.option(option))
                if not 1 <= value <= 65535:
                    c.fail(option + ' must be between 1 and 65535.')
                m[key] = value
        for option, key in (('SSH_KEY', 'ssh_key'), ('INSTALLER', 'installer')):
            value = c.option(option)
            if value == 'none':
                m.pop(key, None)
            elif value:
                m[key] = str(Path(value).expanduser().resolve(strict=True))
        if c.option('ASK_PASS') == '1':
            m['ssh_password'] = getpass.getpass('SSH password (empty for key): ')
            m['sudo_password'] = getpass.getpass('Sudo password (empty to reuse SSH): ')
        c.save(slot_path(m['name']), m)
    print('Updated machine settings; kernel association unchanged: ' + m['name'])

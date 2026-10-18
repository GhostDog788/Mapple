#!/usr/bin/env python3
"""Repository-local development lab. No third-party Python dependencies."""

import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.request

import debug_kernel
import kernel_compat

ROOT = Path(__file__).resolve().parents[1]
STATE = Path(os.environ.get("MAPPLE_LAB_DIR", ROOT / ".lab")).resolve()
IMAGE_BASE = "https://cloud-images.ubuntu.com/releases/noble/release"
IMAGE_NAME = "ubuntu-24.04-server-cloudimg-amd64.img"
RESERVED_PORTS = set()


def fail(message):
    raise RuntimeError(message)


def run(argv, **kwargs):
    kwargs.setdefault("cwd", ROOT)
    return subprocess.run([str(a) for a in argv], check=True, **kwargs)


def save(path, data):
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    STATE.chmod(0o700)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_suffix(path.suffix + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as output:
        os.fchmod(output.fileno(), 0o600)
        output.write(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def validate_name(name):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name):
        fail("Slot names must start with a letter and contain only letters, digits, _ or -.")
    return name


def slot_path(name):
    return STATE / "slots" / (validate_name(name) + ".json")


def load_slot(name=None):
    if name is None:
        active = STATE / "active"
        if not active.exists():
            fail("No active VM. Run make vm-create, or make vm-add NAME=dev HOST=... SSH_USER=...")
        name = active.read_text().strip()
    path = slot_path(name)
    if not path.exists():
        fail(f"Unknown VM slot {name!r}. Run make vm-list.")
    return name, json.loads(path.read_text())


def select(name):
    load_slot(name)
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    (STATE / "active").write_text(name + "\n")
    cache = ROOT / "kernel/kernel-cache"
    cache.mkdir(parents=True, exist_ok=True)
    link = cache / "current"
    if link.exists() and not link.is_symlink():
        fail(f"Expected a symlink at {link}; move that directory before selecting a VM.")
    if link.is_symlink() or not link.exists():
        link.unlink(missing_ok=True)
        link.symlink_to(name)
    # Never leave navigation or debugger symbols from a different VM active.
    for filename in ("compile_commands.json", "kernel/compile_commands.json",
                     "kernel/module_compile_commands.json", "kernel/linux_compile_commands.json",
                     "kernel/.gdb/current-debug.gdb", "kernel/.gdb/current-vmlinux"):
        (ROOT / filename).unlink(missing_ok=True)
    release = cache / name / "kernel.release"
    if release.exists() and (cache / name / "build").exists():
        build_kernel(name)
    print(f"Active VM: {name}")


def import_legacy():
    """Read the old Bash config once; never print credentials or overwrite slots."""
    legacy = ROOT / "lab.local.env"
    if not legacy.exists():
        return
    script = r'''
source "$1"
for name in "${TARGETS[@]}"; do
    printf '%s\0' "$name"
    for key in IP SSH_USER SSH_PORT SSH_PASS SUDO_PASS DEBUG_ENDPOINT_QEMU DEBUG_ENDPOINT_KGDB REMOTE_DIR SSH_KEY; do
        map="TARGET_$key"
        if declare -p "$map" &>/dev/null; then
            declare -n value="$map"
            printf '%s\0' "${value[$name]:-}"
            unset -n value
        else
            printf '\0'
        fi
    done
done
'''
    fields = run(["bash", "-c", script, "bash", legacy], capture_output=True).stdout.decode().split("\0")[:-1]
    keys = ("host", "user", "ssh_port", "ssh_password", "sudo_password", "gdb", "kgdb", "remote_dir", "ssh_key")
    names = []
    for i in range(0, len(fields), len(keys) + 1):
        name = validate_name(fields[i])
        names.append(name)
        if not slot_path(name).exists():
            config = dict(zip(keys, fields[i + 1:i + 1 + len(keys)]))
            config["ssh_port"] = int(config["ssh_port"] or 22)
            config["module_port"] = 4444
            config["method"] = "qemu"
            cache = ROOT / "kernel/kernel-cache" / name
            # Preserve locally built kernels and their exact source/symbols.
            for key, filename in (("source", "source"), ("vmlinux", "vmlinux")):
                p = cache / filename
                if p.exists():
                    config[key] = str(p.resolve())
            save(slot_path(name), config)
            print(f"Imported VM slot: {name}")
    if names and not (STATE / "active").exists():
        current = ROOT / "kernel/kernel-cache/current"
        name = current.resolve().name if current.exists() else names[0]
        (STATE / "active").write_text((name if name in names else names[0]) + "\n")


def bridge(name, config):
    """Use the existing proven build/debug scripts with a private generated config."""
    mappings = {
        "OS": "ubuntu", "IP": config["host"], "SSH_USER": config["user"],
        "SSH_PORT": config.get("ssh_port", 22), "SSH_KEY": config.get("ssh_key", ""),
        "SSH_PASS": config.get("ssh_password", ""), "SUDO_PASS": config.get("sudo_password", ""),
        "DEBUG_ENDPOINT_QEMU": config.get("gdb", ""), "DEBUG_ENDPOINT_KGDB": config.get("kgdb", ""),
        "REMOTE_DIR": config.get("remote_dir", ""), "KGDB_TTY": "ttyS0", "KGDB_BAUD": 115200,
        "LOCAL_SOURCE": config.get("source", ""), "LOCAL_VMLINUX": config.get("vmlinux", ""),
        "KNOWN_HOSTS": str(STATE / "known_hosts"),
    }
    path = STATE / f"{name}.env"
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    content = f"TARGETS=({shlex.quote(name)})\n"
    for key, value in mappings.items():
        content += f"declare -A TARGET_{key}=([{name}]={shlex.quote(str(value))})\n"
    path.touch(mode=0o600, exist_ok=True)
    path.chmod(0o600)
    path.write_text(content)
    return dict(os.environ, MAPPLE_LAB_ENV=str(path))


def kernel_script(number, name, config, *args):
    script = next((ROOT / "kernel/scripts").glob(number + "-*.sh"))
    run(["bash", script, name, *args], env=bridge(name, config) if config else None)


def ssh_args(config):
    args = ["ssh", "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=3",
            "-o", "ServerAliveInterval=15", "-o", f"UserKnownHostsFile={STATE / 'known_hosts'}",
            "-p", str(config.get("ssh_port", 22))]
    if config.get("ssh_key"):
        args += ["-i", config["ssh_key"], "-o", "IdentitiesOnly=yes"]
    if config.get("ssh_password"):
        args = ["sshpass", "-e", *args]
    else:
        args += ["-o", "BatchMode=yes"]
    return args + [f"{config['user']}@{config['host']}"]


def ssh(config, command, **kwargs):
    env = dict(os.environ, SSHPASS=config.get("ssh_password", ""))
    return run([*ssh_args(config), command], env=env, **kwargs)


def wait_ssh(config, seconds=180, old_boot=None):
    deadline = time.monotonic() + seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            result = ssh(config, "cat /proc/sys/kernel/random/boot_id", capture_output=True, timeout=8)
            if old_boot is None or result.stdout.decode().strip() != old_boot:
                return
        except subprocess.CalledProcessError as error:
            last_error = (error.stderr or b"").decode(errors="replace").strip()
            if "Host key verification failed" in last_error:
                fail(last_error)
        except subprocess.TimeoutExpired:
            last_error = "SSH timed out"
        time.sleep(2)
    fail("VM did not become ready over SSH. Check make vm-status and the guest console.\n" + last_error)


def build_kernel(name):
    kernel_script("03", name, {})


def terminal(title, argv):
    if os.environ.get("FOREGROUND") == "1":
        os.execvp(str(argv[0]), [str(a) for a in argv])
    custom = os.environ.get("TERMINAL")
    if custom:
        prefix = shlex.split(custom)
    elif shutil.which("gnome-terminal") and os.environ.get("DISPLAY"):
        prefix = ["gnome-terminal", "--title", title, "--"]
    elif shutil.which("konsole") and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        prefix = ["konsole", "-e"]
    elif shutil.which("x-terminal-emulator") and os.environ.get("DISPLAY"):
        prefix = ["x-terminal-emulator", "-e"]
    else:
        fail("No graphical terminal found. Use FOREGROUND=1 or set TERMINAL='your-terminal -e'.")
    # Keep errors visible instead of closing a newly opened terminal immediately.
    command = shlex.join([str(a) for a in argv])
    wrapper = f"cd {shlex.quote(str(ROOT))}; {command}; rc=$?; if [ $rc -ne 0 ]; then echo 'Command failed. Press Enter to close.'; read -r _; fi; exit $rc"
    run([*prefix, "bash", "-c", wrapper])


def dev_config():
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    STATE.chmod(0o700)
    dev = STATE / "dev"
    dev.mkdir(parents=True, exist_ok=True, mode=0o700)
    cert, key = dev / "mappled.crt", dev / "mappled.key"
    if not cert.exists() or not key.exists():
        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key,
             "-out", cert, "-days", "365", "-subj", "/CN=localhost",
             "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
    key.chmod(0o600)
    token_path = dev / "token"
    if not token_path.exists():
        import secrets
        token_path.touch(mode=0o600)
        token_path.write_text(secrets.token_hex(32))
    token_path.chmod(0o600)
    token = token_path.read_text().strip()
    server = dev / "mappled.json"
    if not server.exists():
        save(server, {"listen_address": "127.0.0.1:8443", "tls_cert_file": str(cert),
                      "tls_key_file": str(key), "handshake_timeout_seconds": 30,
                      "lease_timeout_minutes": 15,
                      "identities": [{"name": "dev", "token_sha256": hashlib.sha256(token.encode()).hexdigest()}]})
    tui = dev / "mapple.json"
    config = json.loads(tui.read_text()) if tui.exists() else {
        "active_profile": "dev", "profiles": {"dev": {
            "server_url": "https://127.0.0.1:8443", "bearer_token": token, "ca_file": str(cert)}}}
    # Preserve personal module profiles while refreshing lab-owned destinations.
    profiles = config.setdefault("module_profiles", {})
    for path in (STATE / "slots").glob("*.json"):
        slot = json.loads(path.read_text())
        profiles[path.stem] = {"transport": "tcp", "host": slot["host"], "port": slot.get("module_port", 4444)}
    save(tui, config)
    return server, tui


def setup(install=True):
    if install:
        run(["sudo", "apt-get", "update"])
        run(["sudo", "apt-get", "install", "-y", "build-essential", "bc", "bison", "flex", "dwarves",
             "gcc-12", "libelf-dev", "libssl-dev", "clangd-18", "gdb", "python3", "openssh-client", "rsync",
             "sshpass", "openssl", "qemu-system-x86", "qemu-utils", "cloud-image-utils", "xterm"])
        if not shutil.which("go"):
            run(["sudo", "apt-get", "install", "-y", "golang-go"])
    if not shutil.which("go"):
        fail("Go is missing. Run make setup without NO_INSTALL=1.")
    run(["go", "mod", "download"], cwd=ROOT / "user")
    import_legacy()
    dev_config()
    print("Setup complete. Run make vm-create, or make vm-list to use an imported VM.")


def add_slot():
    name = validate_name(os.environ.get("NAME") or "dev")
    path = slot_path(name)
    if path.exists():
        fail(f"Slot {name} already exists. Use a new NAME, or edit {path}.")
    host = os.environ.get("HOST") or input("VM address: ").strip()
    user = os.environ.get("SSH_USER") or input("SSH user: ").strip()
    if not re.fullmatch(r"[A-Za-z0-9_.:%-]+", host) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", user):
        fail("Invalid SSH host or user.")
    config = {"host": host, "user": user, "ssh_port": int(os.environ.get("SSH_PORT") or 22),
              "module_port": int(os.environ.get("MODULE_PORT") or 4444),
              "gdb": os.environ.get("GDB_ENDPOINT") or "127.0.0.1:1234", "method": "qemu"}
    for env, key in (("SSH_KEY", "ssh_key"), ("SOURCE", "source"), ("VMLINUX", "vmlinux")):
        if os.environ.get(env):
            p = Path(os.environ[env]).expanduser().resolve()
            if not p.exists():
                fail(f"{env} does not exist: {p}")
            config[key] = str(p)
    if config.get("source") and not config.get("vmlinux"):
        config["vmlinux"] = str(Path(config["source"]) / "vmlinux")
    if os.environ.get("ASK_PASS") == "1":
        config["ssh_password"] = getpass.getpass("SSH password (empty for key auth): ")
        config["sudo_password"] = getpass.getpass("Sudo password (empty to reuse SSH password): ")
    save(path, config)
    if not (STATE / "active").exists():
        select(name)
    dev_config()
    print(f"Saved VM slot {name}. Select it with make vm-use NAME={name}, then make vm-setup.")


def free_port(preferred):
    reserved = set(RESERVED_PORTS)
    for path in (STATE / "slots").glob("*.json"):
        slot = json.loads(path.read_text())
        if slot.get("managed"):
            reserved.update((slot["ssh_port"], slot["module_port"], int(slot["gdb"].rsplit(":", 1)[1])))
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
    name = validate_name(os.environ.get("NAME") or "dev")
    if slot_path(name).exists() or (STATE / "vms" / name).exists():
        fail(f"Slot or VM directory {name} already exists; choose a new NAME.")
    for tool in ("cloud-localds", "qemu-img", "qemu-system-x86_64", "ssh-keygen"):
        if not shutil.which(tool):
            fail(f"Missing {tool}; run make setup.")
    images = STATE / "images"
    images.mkdir(parents=True, exist_ok=True)
    print("Fetching Ubuntu 24.04 image checksums…", flush=True)
    with urllib.request.urlopen(IMAGE_BASE + "/SHA256SUMS", timeout=60) as response:
        sums = response.read().decode()
    checksum = next((line.split()[0] for line in sums.splitlines()
                     if len(line.split()) == 2 and line.split()[-1].lstrip("*") == IMAGE_NAME), None)
    if not checksum:
        fail("Ubuntu image checksum is missing from SHA256SUMS.")
    image = images / f"{checksum}.qcow2"
    if not image.exists():
        print("Downloading Ubuntu cloud image (first use only)…", flush=True)
        temporary = images / f"{checksum}.part"
        with urllib.request.urlopen(IMAGE_BASE + "/" + IMAGE_NAME, timeout=120) as response, temporary.open("wb") as out:
            shutil.copyfileobj(response, out)
        with temporary.open("rb") as downloaded:
            image_hash = hashlib.file_digest(downloaded, "sha256").hexdigest()
        if image_hash != checksum:
            temporary.unlink()
            fail("Ubuntu image checksum mismatch; retry make vm-create.")
        temporary.replace(image)
    vm = STATE / "vms" / name
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
    if os.environ.get("SOURCE"):
        source = Path(os.environ["SOURCE"]).expanduser().resolve()
        config["source"] = str(source)
        config["vmlinux"] = str(Path(os.environ.get("VMLINUX") or source / "vmlinux").expanduser().resolve())
    save(slot_path(name), config)
    if not (STATE / "active").exists():
        select(name)
    dev_config()
    print(f"Created {name}. Run make vm-use NAME={name}, make vm-up, then make vm-setup.")


def vm_command(name, config, action):
    if not config.get("managed"):
        fail("This slot is an existing VM. Start/stop it with its QEMU launcher; make vm-ssh still works.")
    vm = STATE / "vms" / name
    monitor = vm / "monitor.sock"
    if action == "vm-down":
        with socket.socket(socket.AF_UNIX) as sock:
            sock.connect(str(monitor))
            sock.sendall(b"system_powerdown\n")
        print("Guest shutdown requested. Use make vm-status to check it has stopped.")
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
    accelerated = os.access("/dev/kvm", os.R_OK | os.W_OK)
    args = ["qemu-system-x86_64", "-name", f"mapple-{name}", "-machine", "q35",
            "-accel", "kvm" if accelerated else "tcg", "-cpu", "host" if accelerated else "max",
            "-m", str(config["memory"]), "-smp", str(config["cpus"]),
            "-drive", f"file={vm / 'disk.qcow2'},if=virtio,format=qcow2",
            "-drive", f"file={vm / 'seed.img'},if=virtio,format=raw,readonly=on",
            "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{config['ssh_port']}-:22,hostfwd=tcp:127.0.0.1:{config['module_port']}-:4444",
            "-device", "virtio-net-pci,netdev=net0", "-gdb", "tcp:" + config["gdb"],
            "-display", "none", "-serial", f"file:{vm / 'console.log'}", "-monitor",
            f"unix:{monitor},server=on,wait=off", "-pidfile", vm / "qemu.pid", "-daemonize"]
    run(args)
    print(f"Waiting for {name} SSH; console: {vm / 'console.log'}", flush=True)
    wait_ssh(config)
    ssh(config, "cloud-init status --wait", timeout=600)


def provision(name, config):
    print(f"Preparing {name} for kernel source debugging…", flush=True)
    if not config.get("source"):
        source = debug_kernel.ensure(STATE)
        config.update(source=str(source), vmlinux=str(source / "vmlinux"))
        save(slot_path(name), config)
    source = Path(config["source"])
    kernel_compat.check(source)
    release_file = source / "include/config/kernel.release"
    if not release_file.exists():
        fail("SOURCE must be a configured, fully built kernel. Use make debug-kernel or provide a built SOURCE.")
    desired = release_file.read_text().strip()
    if not Path(config["vmlinux"]).exists():
        fail("Matching VMLINUX is missing. Build the selected SOURCE first.")
    live = ssh(config, "uname -r", capture_output=True).stdout.decode().strip()
    if live != desired:
        debug_kernel.install(sys.modules[__name__], name, config, source)
    old_boot = ssh(config, "cat /proc/sys/kernel/random/boot_id", capture_output=True).stdout.decode().strip()
    # Provision boot arguments before rebooting into the selected compatible kernel.
    kernel_script("01", name, config, config.get("method", "qemu"), "--debug-symbols")
    command = "sudo -S -p '' sh -c 'sleep 2; reboot'"
    password = config.get("sudo_password") or config.get("ssh_password", "")
    try:
        ssh(config, command, input=(password + "\n").encode(), timeout=15)
    except subprocess.CalledProcessError as error:
        if error.returncode != 255:
            raise
    print("Waiting for the guest to reboot…", flush=True)
    wait_ssh(config, old_boot=old_boot)
    live = ssh(config, "uname -r", capture_output=True).stdout.decode().strip()
    if live != desired:
        fail(f"Guest booted {live}, expected {desired}. Check the selected GRUB entry.")
    kernel_script("02", name, config)
    build_kernel(name)
    print("VM ready. Use make run-kernel, make run-kernel-debug, or F5 in VS Code.")


def doctor(name, config):
    missing = [tool for tool in ("go", "gcc", "make", "python3", "gdb", "clangd-18", "ssh", "rsync", "openssl")
               if not shutil.which(tool)]
    print("Host tools:", "missing " + ", ".join(missing) if missing else "OK")
    print(f"Active slot: {name}; SSH {config['user']}@{config['host']}:{config.get('ssh_port', 22)}")
    cache = ROOT / "kernel/kernel-cache" / name
    for label, path in (("headers", cache / "build/Makefile"), ("kernel source", cache / "source/init/main.c"),
                        ("debug symbols", cache / "vmlinux"), ("navigation database", ROOT / "compile_commands.json")):
        print(f"{label}: {'OK' if path.exists() else 'missing (make vm-setup)'}")
    kernel_script("00", name, config, config.get("method", "qemu"))
    if missing:
        fail("Install missing host tools with make setup.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("setup", "debug-kernel", "dev-config", "vm-add", "vm-create", "vm-use", "vm-list",
                        "vm-up", "vm-down", "vm-status", "vm-ssh", "vm-setup", "vm-sync", "doctor",
                        "kernel-build", "kernel-clean", "kernel-run", "kernel-debug", "gdb", "gdb-here", "tui"))
    args = parser.parse_args()
    cmd = args.command
    if cmd == "debug-kernel":
        STATE.mkdir(parents=True, exist_ok=True)
        print(debug_kernel.ensure(STATE))
        return
    if cmd == "setup":
        setup(os.environ.get("NO_INSTALL") != "1")
        return
    if cmd == "dev-config":
        dev_config()
        return
    if cmd == "vm-add":
        add_slot()
        return
    if cmd == "vm-create":
        create_vm()
        return
    if cmd == "vm-list":
        active = (STATE / "active").read_text().strip() if (STATE / "active").exists() else ""
        for path in sorted((STATE / "slots").glob("*.json")):
            c = json.loads(path.read_text())
            print(f"{'*' if path.stem == active else ' '} {path.stem}: {c['user']}@{c['host']}:{c.get('ssh_port', 22)} ({'managed QEMU' if c.get('managed') else 'existing VM'})")
        if not list((STATE / "slots").glob("*.json")):
            print("No slots. Run make setup, then make vm-create or make vm-add.")
        return
    if cmd == "vm-use":
        select(os.environ.get("NAME") or fail("Usage: make vm-use NAME=<slot>"))
        dev_config()
        return
    if cmd == "tui":
        _, config = dev_config()
        terminal("Mapple TUI", [ROOT / "user/bin/mapple", "-config", config])
        return
    name, config = load_slot()
    if cmd == "kernel-build":
        build_kernel(name)
    elif cmd == "kernel-clean":
        cache = ROOT / "kernel/kernel-cache" / name
        release = (cache / "kernel.release").read_text().strip()
        run(["make", "-C", ROOT / "kernel", f"KDIR={cache / 'build'}", f"BUILD_ID={name}/{release}", "clean"])
    elif cmd == "vm-ssh":
        os.execvpe(ssh_args(config)[0], ssh_args(config), dict(os.environ, SSHPASS=config.get("ssh_password", "")))
    elif cmd in ("vm-up", "vm-down"):
        vm_command(name, config, cmd)
    elif cmd == "vm-setup":
        provision(name, config)
    elif cmd == "vm-sync":
        kernel_script("02", name, config)
        build_kernel(name)
    elif cmd == "vm-status":
        try:
            ssh(config, "uname -r; uptime; test ! -d /sys/module/mapple || echo 'mapple module loaded'", timeout=8)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            fail("VM not reachable over SSH. Run make vm-up for a managed VM.")
    elif cmd == "doctor":
        doctor(name, config)
    elif cmd in ("kernel-run", "kernel-debug"):
        build_kernel(name)
        method = "none" if cmd == "kernel-run" else config.get("method", "qemu")
        kernel_script("04", name, config, method, *shlex.split(os.environ.get("MODULE_PARAMS", "")))
    elif cmd in ("gdb", "gdb-here"):
        script = ROOT / "kernel/.gdb/current-debug.gdb"
        if not script.exists():
            fail("No prepared debug run. Run make run-kernel-debug first.")
        if cmd == "gdb":
            terminal("Mapple kernel GDB", [sys.executable, Path(__file__), "gdb-here"])
        else:
            os.execvp("gdb", ["gdb", "-q", "-tui", "-x", str(script)])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"mapple lab: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)

#!/usr/bin/env python3
"""Build the known-compatible guest debug kernel without changing application C."""

import hashlib
import inspect
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

VERSION = "6.6.6"
BASE = "https://cdn.kernel.org/pub/linux/kernel/v6.x"


def grub_selection(text, release):
    import re
    entry = re.search(r"(?m)^([\t ]*)menuentry 'Ubuntu, with Linux " + re.escape(release) + r"'.* '([^']+)'\s*\{", text)
    if not entry:
        raise RuntimeError("Installed kernel is missing from GRUB; select it manually before continuing.")
    submenu = re.search(r"(?m)^submenu 'Advanced options for Ubuntu'.* '([^']+)'\s*\{", text)
    return (submenu[1] + ">" if entry[1] and submenu else "") + entry[2]


def ensure(state):
    source = state / "kernels" / f"linux-{VERSION}"
    if (source / "vmlinux").exists() and (source / ".mapple-built").exists():
        return source
    if not shutil.which("gcc-12"):
        raise RuntimeError("The pinned debug kernel uses gcc-12. Install host prerequisites with make setup.")
    if shutil.disk_usage(state).free < 10 * 1024**3:
        raise RuntimeError("Building a debug kernel needs at least 10 GiB free. Supply an existing built kernel with SOURCE=... VMLINUX=... instead.")
    archive = source.parent / f"linux-{VERSION}.tar.xz"
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        print(f"Downloading Linux {VERSION} for the guest debug kernel…", flush=True)
        with urllib.request.urlopen(BASE + "/sha256sums.asc", timeout=60) as response:
            sums = response.read().decode()
        checksum = next((line.split()[0] for line in sums.splitlines()
                         if len(line.split()) == 2 and line.split()[1] == archive.name), None)
        if not checksum:
            raise RuntimeError("Kernel archive is missing from kernel.org SHA256 sums")
        with urllib.request.urlopen(BASE + "/" + archive.name, timeout=120) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
        with archive.open("rb") as downloaded:
            if hashlib.file_digest(downloaded, "sha256").hexdigest() != checksum:
                raise RuntimeError("Kernel archive checksum mismatch")
        source.mkdir()
        subprocess.run(["tar", "-C", str(source), "--strip-components=1", "-xf", str(archive)], check=True)
        archive.unlink()
    def make(*args):
        subprocess.run(["make", "-C", str(source), "CC=gcc-12", *args], check=True)
    make("x86_64_defconfig")
    # This is a headless QEMU lab; retain normal storage/network drivers but
    # avoid spending the first setup compiling graphics, sound, and radio stacks.
    options = ["LOCALVERSION_AUTO", "DEBUG_INFO_NONE", "DEBUG_INFO_BTF", "DRM", "SOUND",
               "MEDIA_SUPPORT", "WLAN", "BT", "NFC"]
    config = [str(source / "scripts/config"), "--file", str(source / ".config")]
    for option in options:
        config += ["--disable", option]
    config += ["--set-str", "LOCALVERSION", "-mapple"]
    for option in ("DEBUG_KERNEL", "DEBUG_INFO", "DEBUG_INFO_DWARF4", "GDB_SCRIPTS", "FRAME_POINTER",
                   "BLK_DEV_INITRD", "DEVTMPFS", "DEVTMPFS_MOUNT", "VIRTIO", "VIRTIO_PCI",
                   "VIRTIO_BLK", "VIRTIO_NET", "EXT4_FS", "MODULES", "KALLSYMS", "KALLSYMS_ALL",
                   "ISO9660_FS", "RD_ZSTD", "RD_GZIP", "RD_XZ",
                   "IKCONFIG", "IKCONFIG_PROC", "CGROUPS", "NAMESPACES", "SECCOMP", "SECCOMP_FILTER"):
        config += ["--enable", option]
    subprocess.run(config, check=True)
    make("olddefconfig")
    jobs = os.environ.get("JOBS") or str(min(os.cpu_count() or 2, 4))
    print(f"Building the debug kernel once ({jobs} jobs); later VM slots reuse it…", flush=True)
    try:
        make("-j" + jobs, "bzImage", "modules")
    except subprocess.CalledProcessError:
        raise RuntimeError("Debug kernel build failed; no completed build was cached. Review compiler output, "
                           "retry make debug-kernel JOBS=2, or register a built SOURCE=... VMLINUX=... kernel.") from None
    (source / ".mapple-compiler").write_text("gcc-12\n")
    (source / ".mapple-built").touch()
    return source


def install(lab, name, config, source):
    """Install a host-built kernel into an Ubuntu guest and select its GRUB entry."""
    import shlex
    import re
    release = (source / "include/config/kernel.release").read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", release):
        raise RuntimeError("Invalid kernel release string in SOURCE")
    image = source / "arch/x86/boot/bzImage"
    if not image.exists():
        raise RuntimeError("SOURCE must be a fully built x86 kernel (bzImage, vmlinux, modules).")
    signature = hashlib.sha256((source / ".config").read_bytes() + (source / "Module.symvers").read_bytes()).hexdigest()[:12]
    stage = lab.STATE / "kernel-install" / (release + "-" + signature)
    stage.mkdir(parents=True, exist_ok=True)
    modules = stage / "modules"
    if not (stage / "modules.tar.gz").exists():
        compiler = (source / ".mapple-compiler").read_text().strip() if (source / ".mapple-compiler").exists() else "gcc"
        lab.run(["make", "-s", "-C", source, f"CC={compiler}", f"INSTALL_MOD_PATH={modules}", "INSTALL_MOD_STRIP=1", "modules_install"])
        lab.run(["tar", "-C", modules / "lib/modules", "-czf", stage / "modules.tar.gz", release])
        shutil.rmtree(modules)
    remote = "/tmp/mapple-kernel-" + name
    lab.ssh(config, "mkdir -p " + shlex.quote(remote))
    args = lab.ssh_args(config)
    destination = args.pop()
    lab.run(["rsync", "-a", "-e", shlex.join(args), image, source / "System.map", source / ".config",
             stage / "modules.tar.gz", destination + ":" + remote + "/"],
            env=dict(os.environ, SSHPASS=config.get("ssh_password", "")))
    script = f'''set -euo pipefail
apt-get -qq update
apt-get -qq install -y initramfs-tools rsync cryptsetup-initramfs cryptsetup
{("printf 'manual_cache_clean: true\\n' > /etc/cloud/cloud.cfg.d/99-mapple-cache.cfg" if config.get("managed") else ":")}
install -m 644 {shlex.quote(remote + '/bzImage')} /boot/vmlinuz-{release}
install -m 644 {shlex.quote(remote + '/System.map')} /boot/System.map-{release}
install -m 644 {shlex.quote(remote + '/.config')} /boot/config-{release}
mkdir -p /lib/modules
tar --no-same-owner -xzf {shlex.quote(remote + '/modules.tar.gz')} -C /lib/modules
depmod -a {release}
if [ -e /boot/initrd.img-{release} ]; then
    update-initramfs -u -k {release}
else
    update-initramfs -c -k {release}
fi
update-grub
python3 - {release} <<'SELECT_KERNEL'
import json, pathlib, sys
{inspect.getsource(grub_selection)}
text = pathlib.Path('/boot/grub/grub.cfg').read_text()
selection = grub_selection(text, sys.argv[1])
pathlib.Path('/etc/default/grub.d/99-mapple-kernel.cfg').write_text('GRUB_DEFAULT=' + json.dumps(selection) + '\\n')
SELECT_KERNEL
update-grub
rm -rf {shlex.quote(remote)}
'''
    password = config.get("sudo_password") or config.get("ssh_password", "")
    lab.ssh(config, "sudo -k -S -p '' bash -c " + shlex.quote(script), input=(password + "\n").encode())
    return release

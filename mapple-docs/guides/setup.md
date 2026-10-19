# Set up a development lab

[Documentation](../README.md) / Guides

This guide takes a fresh Ubuntu host to a running Mapple stack. The Go server
and TUI run on the host; the kernel module runs in a local Ubuntu QEMU guest.
Run all shell commands from the repository root unless a step says otherwise.

After setup, use the [usage guide](usage.md) for the daily loop, command reference,
and source debugging. Return to the [project overview](../../README.md).

## 1. Prepare the host

Use **x86-64 Ubuntu 24.04**, sudo access, and an internet connection for apt,
Go dependencies, the Ubuntu cloud image, and Linux source downloads.

- **Disk:** the kernel builder requires at least 10 GiB free before starting.
  Plan additional space for the cloud image and VM disk growth; 20 GiB or more
  free is a useful starting point. The guest disk defaults to 30 GiB of virtual
  capacity and grows as it is used.
- **Memory:** the default VM uses 4 GiB; leave memory for the host and compiler.
  On a smaller host, create the VM with `MEMORY=1024` and build with `JOBS=2`.
- **Virtualization:** enable hardware virtualization in firmware for KVM.
  If the host is itself a VM, enable nested virtualization in its hypervisor.
  QEMU can use software emulation when KVM is unavailable, but it is much slower.
- **Editor:** VS Code is optional. For its kernel debugger, install the workspace's
  recommended **C/C++** and **clangd** extensions. GDB TUI works without VS Code.

Install the initial tools and clone:

```bash
sudo apt-get update
sudo apt-get install -y git make python3
git clone https://github.com/GhostDog788/Mapple.git
cd Mapple
make setup
```

`make setup` installs build tools, GCC 12, clangd 18, GDB, SSH/rsync, OpenSSL,
QEMU, cloud-init image tools, and a terminal emulator. It downloads Go dependencies
and creates matching local server/TUI configurations, a TLS certificate, and a
bearer token. The Go version comes from `user/go.mod`; Make selects the required
toolchain without changing global Go settings. You do not need to install Go
manually.

When necessary, setup adds your user to the `kvm` group. `make vm-up` can use the
new group membership immediately. Repeat setup as needed: it preserves existing
VM slots, their selection, tokens, certificates, and local configuration edits.
If you already installed all host prerequisites, `make setup NO_INSTALL=1` skips
apt and group changes; it still downloads Go dependencies and prepares local
configuration.

## 2. Choose a VM

Choose **A** to create a lab VM, or **B** to use a VM you already run. A VM slot is
a saved destination, including SSH details, module port, debugger endpoint, and
kernel inputs. The first slot becomes active automatically. Other commands use
that slot until you select another with `make vm-use NAME=<slot>`.

### A. Create an Ubuntu QEMU VM

```bash
make vm-create NAME=dev
make vm-up
make vm-setup
```

Creation downloads Canonical's Ubuntu 24.04 amd64 cloud image, verifies its SHA-256
checksum, and generates a private SSH key, cloud-init seed, and qcow2 overlay.
It does not start the VM. Defaults are **4 GiB RAM, 2 CPUs, and a 30 GiB virtual
disk**. To choose resources, supply them when creating it:

```bash
make vm-create NAME=small MEMORY=1024 CPUS=2 DISK=20G
make vm-use NAME=small
make vm-up
make vm-setup JOBS=2
```

Use one creation example; each name must be unique. When another slot is already
active, select your newly created VM before starting or preparing it.

The VM runs headless in the background. SSH, module TCP, and GDB ports are
forwarded on host loopback; available ports are selected automatically and saved
in the slot. The guest user is `mapple` with passwordless sudo. Inspect it with
`make vm-status` or log in with `make vm-ssh`. Console output is at
`.lab/vms/<slot>/console.log`.

### B. Register an existing Ubuntu QEMU VM

Start your VM with its own QEMU launcher first. The guest needs an SSH server, a
sudo-capable account, and network access for provisioning. It must boot with
GRUB and permit unsigned lab modules; disable guest Secure Boot for this lab.

QEMU must expose its GDB stub to the development host. Add this option to the
VM's QEMU launch command, then restart the VM with that launcher:

```text
-gdb tcp:127.0.0.1:1234
```

Register the VM, replacing the example address, username, and key path:

```bash
make vm-add NAME=dev HOST=192.168.122.50 SSH_USER=ubuntu \
  SSH_KEY=~/.ssh/id_ed25519 GDB_ENDPOINT=127.0.0.1:1234
make vm-use NAME=dev
make vm-setup
```

Use `SSH_PORT=<port>` for a forwarded SSH port. `MODULE_PORT=<port>` is the module
TCP port reachable **from the host server**; it defaults to 4444. If the VM uses
NAT, forward that host port to guest port 4444. `GDB_ENDPOINT` must match the
QEMU stub reachable from the host, not a service inside the guest. SSH access
alone cannot enable QEMU's debugger.

Omit `SSH_KEY` to use your default SSH identities or agent. Add `ASK_PASS=1` to
enter SSH and sudo passwords privately; this also supports key-based SSH with a
sudo password. An empty SSH password selects key authentication; an empty sudo
password reuses the SSH password. With neither password, sudo must work without
a password. Credentials are saved in the private slot file.

`vm-up` and `vm-down` manage repo-created VMs. For a registered VM, keep using its
own launcher to start and stop it; other lab commands use its saved SSH details.

## 3. Let the kernel preparation finish

`make vm-setup` installs guest prerequisites, prepares kernel boot arguments,
**reboots the guest**, waits for SSH to return, and builds the module with matching
host-side inputs. It does not load the module yet.

The lab uses a pinned **Linux 6.6.6 debug kernel, built with GCC 12**. The module
requires its Linux API; Ubuntu 24.04's stock 6.8 kernel is not a build target.
The first setup builds the kernel on the host and installs it in the guest.
Later slots reuse this build. Allow several minutes for the first compilation;
`JOBS=2` limits parallel compilation on a smaller host (default: up to 4 jobs).
The host continues running its own kernel. The selected guest kernel remains
its GRUB default across boots.

To build the kernel before preparing a guest:

```bash
make debug-kernel JOBS=2
```

To supply your own compatible debug kernel, pass `SOURCE` and `VMLINUX` when
creating or registering a slot:

```bash
make vm-add NAME=custom HOST=192.168.122.50 SSH_USER=ubuntu \
  GDB_ENDPOINT=127.0.0.1:1234 \
  SOURCE=/absolute/path/to/built/linux VMLINUX=/absolute/path/to/built/linux/vmlinux
make vm-use NAME=custom
make vm-setup
```

`SOURCE` must be a fully configured and built x86-64 kernel tree, with matching
`Module.symvers`, `arch/x86/boot/bzImage`, built modules, and unstripped DWARF
symbols. `VMLINUX` defaults to `SOURCE/vmlinux` when omitted. Supply the same
options to `vm-create` for a new managed VM. Setup installs that build if the
guest runs a different kernel; matching host source and symbols are reused
directly. Guest kernel, source, build configuration, and symbols must agree.

## 4. Run the stack

Load the module and check the selected target:

```bash
make run-kernel
make doctor
```

In terminal 1, keep the HTTPS server running:

```bash
make run-server
```

In terminal 2, from the same repository root, open the TUI:

```bash
make run-tui
```

This opens another terminal window. On a host without a graphical desktop, use
`make run-tui FOREGROUND=1` to run it in terminal 2 instead.

Inside the TUI, connect to your slot (`dev` in this example):

```text
session create dev
command info
command echo hello
command pslist
```

`info` reports the kernel session and supported commands. `echo` returns `hello`.
`pslist` opens the guest's process/thread snapshot; use the arrow keys to browse
and Esc to return to the command prompt. A successful echo verifies
the full TUI → HTTPS server → guest module round trip. The server listens on
`https://127.0.0.1:8443`; the TUI trusts the generated certificate automatically.
No token copying or system-wide configuration is needed.

## 5. Stop and resume

Delete your session in the TUI before reloading the module: use `session current`
to get its ID, then `session delete <id-prefix>`. Exit with `quit`, and stop the
server with Ctrl+C. Shut down a managed guest with `make vm-down`; this requests
a graceful shutdown. Use `make vm-status` to check it has become unreachable.

Next time, run `make vm-up`, `make run-kernel`, `make run-server`, and
`make run-tui` in their respective terminals. Selection persists; setup and
provisioning are not daily steps. For a registered VM, start it using its own
launcher. Continue with the [development loop and debugger guide](usage.md).

## Local files

| Location | Contents |
| --- | --- |
| `.lab/slots/<name>.json` | Private VM connection details, credentials, and kernel paths |
| `.lab/active` | Persistently selected slot |
| `.lab/dev/` | Server/TUI configs, development token, TLS certificate and key |
| `.lab/vms/<name>/` | Managed VM disk, SSH key, seed, monitor, and console log |
| `.lab/images/` | Downloaded Ubuntu base images |
| `.lab/kernels/` | Shared host-built debug kernel |
| `.lab/known_hosts` | SSH host keys for this lab |
| `kernel/kernel-cache/<name>/` | Selected guest's headers, source, and symbols |
| `kernel/build/` | Module build outputs per slot and kernel release |
| `kernel/.gdb/` | Generated debugger scripts and relocated module symbols |
| `compile_commands.json` | Generated module and kernel navigation database |

These paths are gitignored. Keep `.lab/` private: it contains credentials and VM
disks. Edit slot JSON only while lab commands are idle, then run
`make vm-use NAME=<slot>` to refresh selection and generated configuration.
For setup failures, start with the [troubleshooting reference](usage.md#troubleshooting).

# Set up a development lab

[Documentation](../README.md) / Guides

The host is Ubuntu 24.04 x86-64. Targets can be Linux machines over SSH, including
other architectures. We create Ubuntu x86-64 QEMU VMs locally; existing machines
keep their own lifecycle. Cross-compilation requires the appropriate compiler/SDK.
Mapple must support the target's kernel APIs; this tooling does not rewrite the
application for incompatible APIs.

## Prepare the host

Allow at least 10 GiB for a Linux build, plus room for copied imports and VM disks.
The default guest uses 4 GiB RAM and two CPUs. KVM is recommended; QEMU falls back
to software emulation if unavailable.

```bash
sudo apt-get update
sudo apt-get install -y git make python3
git clone https://github.com/GhostDog788/Mapple.git
cd Mapple
make setup
```

Setup installs build, SSH, QEMU, GDB, clangd and terminal tools, then downloads Go
dependencies. When the `code` CLI is available, it also installs this repository's
Mapple Navigation extension and its clangd extension dependency. Reload any
already-open VS Code window once after installation. If you install VS Code
later, run `make navigation-setup`.

If setup adds your account to the KVM group, the next `machine-start`
can use that membership without another login. `NO_INSTALL=1` skips host package
and editor-extension installation. Additional known toolchains can be requested explicitly:

```bash
make setup TOOLCHAIN_PACKAGES="gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu"
```

## Create a kernel, then a machine

```bash
make kernel-create NAME=baseline VERSION=6.6.6 PROFILE=debug
make kernel-build KERNEL=baseline JOBS=2
make machine-create NAME=dev KERNEL=baseline
make machine-start
make machine-prepare
```

`kernel-create` records a recipe; `kernel-build` downloads verified upstream
source and builds it. The debug profile enables symbols, DWARF, module support,
and common QEMU storage/network drivers. For other hardware, supply `CONFIG` as a starting configuration; the debug
profile still applies its required debug/module options. The resolved configuration
is saved in the slot’s `source/.config`. Configuration and patch changes require a new kernel slot:

```bash
make kernel-create NAME=experiment VERSION=6.6.6 CONFIG=/path/to/config \
    PATCHES=/path/to/patch-directory
make kernel-build KERNEL=experiment
```

For cross-compilation, specify the target architecture and compiler:

```bash
make kernel-create NAME=arm-debug VERSION=6.6.6 ARCH=aarch64 \
    CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu-
make kernel-build KERNEL=arm-debug
```

The first machine becomes active automatically. Further machines do not change
selection. `machine-create` only creates a disk and connection record;
`machine-prepare` is the explicit install, reboot, verify operation. It retains
the previous kernel and commits the association only after verifying the boot.
Ubuntu/Debian GRUB uses a one-shot candidate with the previous entry as fallback.
A fallback entry cannot recover every hang: inspect the console if SSH does not
return. A failed transition is recorded without claiming recovery.

## Add an existing machine without changing its kernel

```bash
make machine-add NAME=client HOST=192.168.122.50 SSH_USER=ubuntu \
    SSH_KEY=/path/to/key GDB_ENDPOINT=127.0.0.1:1234
make machine-adopt MACHINE=client
```

Registration performs read-only inspection and records the running kernel,
reusing a known slot when its build ID matches, or creating a Tier 0 entry
when no matching slot is known. It does not install anything
or reboot. Omit `GDB_ENDPOINT` for build/run-only work. `ASK_PASS=1` prompts for
SSH/sudo passwords; secrets remain private and are redacted from `machine-show`.

`machine-configure MACHINE=client GDB_ENDPOINT=...` adds debugger access later
without registering another machine. It also updates SSH/installer settings.

Adoption copies existing build/debug files first. If headers are missing, it may
download matching packages into a temporary directory using a target's configured
APT repositories, then extract them on the host. It does not install target
packages. `OFFLINE=1` prohibits this download step. Other package systems can use
a custom fetcher or explicit imports; see [usage](usage.md#integration-hooks).
Missing artifacts remain visible as blockers.

To replace its kernel instead, choose a built/installable slot explicitly:

```bash
make machine-prepare MACHINE=client KERNEL=baseline
```

Unknown boot systems require an `INSTALLER` hook. Linux source debugging also
requires working debugger access; SSH alone cannot stop kernel execution.

## Import matching kernel files

```bash
make kernel-import NAME=vendor ARCH=x86_64 BUILD=/path/to/prepared/build \
    SOURCE=/path/to/linux VMLINUX=/path/to/vmlinux CC=gcc
```

Artifacts are copied into `.lab`, including external header symlink targets.
Editing or deleting the originals does not change the slot. For a fully built
in-tree kernel, `BUILD` and `SOURCE` can identify the same directory; it is copied
once. Source and debug ELF are optional for build/run-only support.

Associate the imported slot with a machine already running that exact kernel:

```bash
make machine-adopt MACHINE=client KERNEL=vendor OFFLINE=1
```

This verifies the observed identity without installing or rebooting. A known
mismatch fails. Without a runtime build ID, the reported match is `release-only`.

Add missing artifacts for the same exact build later:

```bash
make kernel-enrich KERNEL=client-observed BUILD=/path/to/prepared/build
make kernel-enrich KERNEL=client-observed SOURCE=/path/to/source VMLINUX=/path/to/vmlinux
```

Existing artifacts cannot be replaced. Kernel input changes are detected before
building/deploying. A release string alone does not establish an exact match;
use build IDs and retain provenance. See [kernel support tiers](../architecture/kernel-support-tiers.md).

## Prepare source navigation

```bash
make navigation-prepare KERNEL=baseline
make navigation-status KERNEL=baseline
```

This builds Mapple locally to obtain its real compiler commands and prepares a
persistent clangd index for the kernel. No running VM, SSH connection, GDB endpoint,
or debug symbols are needed. A first Linux index can take several minutes;
subsequent preparation reuses it. Linux implementation navigation requires the
matching source. Slots without that source still support Mapple/header navigation.

Preparing a named kernel does not select a machine or retarget an open editor.
When ready to work, run `make machine-use NAME=dev` and open this checkout in
VS Code. The **Mapple Navigation** status bar shows the selected machine/kernel
and its live indexing state. The editor automatically refreshes clangd when the
selected kernel changes. See [source navigation](usage.md#source-navigation).

## Run the stack

```bash
make machine-use NAME=dev
make module-run
make server-run
```

Keep the server running. In another terminal:

```bash
make tui-run
```

The TUI opens in a new terminal. On a headless host, use `FOREGROUND=1`.
Inside the TUI:

```text
session create dev
command info
command echo hello
command pslist
```

Press Esc to leave the process snapshot. Server/TUI configurations are paired
per machine and remain attached to that destination when selection changes.

## Debug and return tomorrow

```bash
make vscode-open MIN_DEBUG=kernel-source
```

Install the recommended Microsoft C/C++ extension for debugging; clangd and the
local Mapple Navigation extension provide static navigation. In the generated
machine workspace, press F5, set a breakpoint in `kernel/src/`, and trigger a TUI
command. F5 reloads Mapple: close old sessions first, then create a new TUI
session before enabling the info breakpoint. Ctrl+click follows definitions in
the active machine's matching Linux source. Alternatively, run `make module-debug`,
create a fresh TUI session, and run `make gdb-open`. In GDB, use
`break mapple_handle_info` and `continue`, then trigger `command info` from the TUI.

Close application sessions before reloading Mapple. Stop server/TUI with Ctrl+C;
`machine-stop` requests graceful shutdown of an owned QEMU VM. Next time:

```bash
make machine-start
make module-run
make server-run
# Another terminal: make tui-run
```

Selection and kernels persist. Setup and kernel installation are preparation
steps, not part of the daily rebuild loop. [Usage](usage.md) documents every command.

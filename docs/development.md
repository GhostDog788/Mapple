# Development on Ubuntu with a QEMU guest

The host runs the Go server and TUI. An Ubuntu QEMU guest runs `mapple.ko`.
All commands below run from the repository root. Application code is unchanged
by this tooling. VS Code is optional except when you want its graphical debugger.

## First clone

Git, Make, Python 3, and sudo are required to start. On a minimal Ubuntu
installation, install them before cloning:

```bash
sudo apt-get update
sudo apt-get install -y git make python3
```

Then, from the cloned repository:

```bash
make setup
```

This installs host build/debug/VM prerequisites using apt and sudo, downloads
Go module dependencies, and creates matching local HTTPS server and TUI configs.
If `/dev/kvm` needs group access, setup adds the current user to `kvm`; the next
`make vm-up` uses that membership immediately, without requiring a new login.
A compatible preinstalled Go toolchain is reused; otherwise Ubuntu's Go package
is installed and Make selects the exact minimum released toolchain required by
`user/go.mod`. This also works with Ubuntu's older Go auto-selector. No global
Go settings are changed; an explicit `GOTOOLCHAIN` override is respected.
Use `make setup NO_INSTALL=1` if the host prerequisites are already installed.

Local state lives in gitignored `.lab/`, with credentials at mode `0600`. Repeating
setup preserves tokens, certificates, slot selection, and edited local configs.
SSH host keys are kept in `.lab/known_hosts`, avoiding collisions with unrelated
VMs using the same forwarded localhost ports. No global TUI configuration is changed.
Old `lab.local.env` targets are imported
once without overwriting existing slots, including existing custom kernel source
and `vmlinux` paths. The old environment template is no longer needed.

## Choose a VM

### Create an Ubuntu VM

```bash
make vm-create NAME=dev
make vm-use NAME=dev
make vm-up
make vm-setup
```

Creation downloads Canonical's Ubuntu 24.04 amd64 cloud image over HTTPS,
checks it against the published SHA256SUMS, creates a separate qcow2 overlay,
and generates a cloud-init seed and SSH key. The image SHA256 is recorded in
the slot. Defaults are 4 GiB RAM, 2 CPUs, and a 30 GiB virtual disk; override
`MEMORY=8192 CPUS=4 DISK=40G` when creating the VM. KVM is used when available,
otherwise QEMU uses software emulation. QEMU runs headless in the background.

SSH, the module's TCP port, and QEMU's GDB stub are bound to loopback on the host.
Host ports are recorded in the slot. Logs are in `.lab/vms/<slot>/console.log`.
The generated guest user has passwordless sudo for lab provisioning.
`vm-setup` prepares a compatible guest kernel, configures `nokaslr`, reboots the
guest, waits for SSH, connects the exact host build inputs, and builds the module.
The module currently uses the Linux 6.6 `get_file_rcu` API, which is incompatible
with Ubuntu's stock 6.8 kernel. To preserve application C unchanged, setup builds
a pinned **Linux 6.6.6 debug kernel with GCC 12** once on the host, installs it in the Ubuntu
guest, selects its GRUB entry, and reuses the exact host source/build/symbols.
Subsequent slots reuse that build. Allow at least 10 GiB free for the first kernel
build, plus VM disk growth, and several minutes of build time. `JOBS=8` controls
kernel build parallelism. `make debug-kernel` can prepare the host build separately.

To reuse an existing compatible kernel instead of compiling another one:

```bash
make vm-create NAME=dev SOURCE=/absolute/path/to/built/linux VMLINUX=/absolute/path/to/vmlinux
```

The source tree must contain its configured build, `Module.symvers`, `arch/x86/boot/bzImage`,
and built modules. The pinned kernel is installed only in the guest; the host
keeps its current kernel. Its default GRUB entry remains selected across reboots.

```bash
make vm-status
make vm-ssh
make vm-down                 # request a graceful ACPI shutdown
make vm-up
```

### Register an existing Ubuntu QEMU VM

```bash
make vm-add NAME=server HOST=192.168.122.76 SSH_USER=ubuntu \
  SSH_KEY=~/.ssh/id_ed25519 GDB_ENDPOINT=127.0.0.1:6722
make vm-use NAME=server
make vm-setup
```

Use `ASK_PASS=1` to enter SSH/sudo passwords privately, including when SSH uses a
key but sudo requires a password. Without it, SSH keys/agent and passwordless sudo
are used. Do not put passwords in Make arguments or shell history.

The QEMU launcher for an existing VM must expose its GDB stub, for example
`-gdb tcp:127.0.0.1:6722`. SSH access cannot enable a hypervisor's GDB stub.
Managed VMs created above configure this automatically. `vm-up`/`vm-down` manage
only repo-created VMs; keep using the original launcher for registered VMs.

For a custom guest kernel, supply its **exact matching** host-side inputs:

```bash
make vm-add NAME=custom HOST=192.168.122.76 SSH_USER=ubuntu \
  GDB_ENDPOINT=127.0.0.1:6722 \
  SOURCE=/absolute/path/to/linux-source VMLINUX=/absolute/path/to/vmlinux
```

The source tree must be a full configured build of the desired guest kernel.
If the guest runs another kernel, setup installs this build and selects it in
GRUB. If it already runs the matching kernel, its boot kernel is preserved.
Host-side build inputs are reused directly, avoiding another copy of the source
and headers. Supplying local source and `vmlinux` skips Ubuntu debug packages.

### Persistent slots

```bash
make vm-list
make vm-use NAME=server
make doctor
```

The first registered/created slot is selected automatically. Creating additional
slots does not change the selection. `vm-use` changes the default until selected
again; Make commands and VS Code use that same slot. Configuration is in
`.lab/slots/<name>.json`; it can be edited directly while no lab command is running.
Selecting a slot invalidates previous debugger symbols and refreshes navigation
when the selected slot already has a kernel cache.

After changing the guest kernel, run `make vm-setup` again. `make vm-sync` refreshes
headers, source, and symbols without provisioning or rebooting an already prepared
guest. Both paths preserve exact custom host-side inputs.

## Build and run

| Command | Action |
| --- | --- |
| `make` / `make all` | Build module, server, and TUI |
| `make kernel` | Incrementally build for the selected guest kernel |
| `make server` | Build `user/bin/mappled` |
| `make tui` | Build `user/bin/mapple` |
| `make user` | Build both Go binaries |
| `make run-server` | Build and run the HTTPS daemon in this terminal |
| `make run-tui` | Build and launch the TUI in a new terminal |
| `make run-kernel` | Build, upload, and reload the module without debugger setup |
| `make run-kernel-debug` | Build, upload, reload, and prepare relocated GDB symbols |
| `make gdb` | Open GDB TUI in a new terminal for the prepared debug run |
| `make gdb-here` | Open GDB TUI in this terminal |
| `make check` | API drift check, tooling tests, Go build/vet/unit tests, shell syntax |
| `make clean` | Remove selected module build outputs and Go binaries |

`build-kernel`, `build-server`, and `build-tui` are aliases. `MODULE_PARAMS='key=value'`
passes module parameters to either upload command. A normal upload invalidates any
previous prepared debugger run. Close active module sessions before reloading;
an unload failure stops deployment instead of silently reusing an old module.

For terminals, the launcher detects GNOME Terminal, Konsole, or Ubuntu's default
terminal. Override with `TERMINAL='xterm -e'`, or use `FOREGROUND=1` on a headless host.
Only the kernel debugger has VS Code launch configuration; Go apps use Make.

Start the daemon and TUI in separate terminals:

```bash
make run-server
make run-tui
```

In the TUI, run `session create server` (replace `server` with the slot name),
then `command info`, `command echo hello`, or `command pslist`.
Slot destinations are prefilled automatically, including forwarded ports for
managed QEMU VMs. `help` and `session help` list the current commands.

Go loop: edit Go code, run `make test` or `make check`, restart the affected process.
Kernel loop: edit C code, run `make run-kernel`, exercise it through the TUI.
A clean rebuild is explicit: `make clean-kernel && make kernel`.
`make api` intentionally regenerates API code; `make api-verify` never changes it.

## Source debugging in VS Code and GDB TUI

Install the recommended C/C++ debugger and clangd VS Code extensions. Prepare the
VM once with `make vm-setup`. The following two workflows share the same symbols
and source mapping:

* **VS Code:** select **Kernel: cppdbg (full GDB MI)** and press F5. Its pre-launch
  task runs `make run-kernel-debug` for the active slot. Set breakpoints directly
  in `kernel/src`, exercise the module via the TUI, and step/inspect variables.
* **GDB TUI:** run `make run-kernel-debug`, then `make gdb`. At the GDB prompt,
  set a breakpoint such as `break mapple_handle_info`, then `continue`.

Only one debugger can own a QEMU GDB stub. Disconnect it before preparing another
run. While GDB has stopped the guest, SSH and module requests also stop responding;
continue the guest before invoking another upload or VM command. Breakpoints in
module initialization require a separate early-load strategy; normal debug upload
loads the module before attaching, as the previous workflow did.

Debug preparation verifies the guest kernel release and `nokaslr`, reads actual
runtime module section addresses, relocates a **copy** of the module ELF, and maps
DWARF paths back to `kernel/src`. The original build artifact remains intact.
The generated `.gdb/current-debug.gdb` and `current-vmlinux` links are used by both
VS Code and terminal GDB. Kernel source paths are derived from the matching
`vmlinux` DWARF information. QEMU multicore scheduling is enabled in GDB.

VS Code uses custom launch commands to load this script. The fixed
`miDebuggerServerAddress` value is only a remote-mode marker: custom launch
commands replace the adapter's connection command, and the script supplies the
active slot's actual endpoint. This makes Pause interrupt the guest through GDB
instead of trying to signal a host process. The generated script maps GDB's
`kill` command to `detach`, so VS Code Stop leaves the VM running.

Building also refreshes root `compile_commands.json` with module and kernel source
commands. clangd uses the configured guest headers and source tree, so Ctrl+click
can navigate from module calls into kernel definitions. Built kernel trees retain
their real compilation commands; source-only packaged trees receive x86 navigation
commands using the guest's header/config flags. Navigation fallback commands are
never used to compile the kernel. Kernel source must match the guest build for
both navigation and accurate stepping.

## Troubleshooting

* `make doctor` checks host tools, SSH/sudo, headers, symbols, source, and the debug
  endpoint. A VM registered by SSH must be started with its own QEMU launcher.
* Kernel mismatch: run `make vm-setup` after a guest kernel upgrade, or boot back
  into the previously prepared kernel. Do not load a module built for another kernel.
* Missing source/symbols: check slot `source`/`vmlinux` overrides, or rerun
  `make vm-setup` to install/sync Ubuntu debug packages. The guest must permit
  unsigned lab modules (disable guest Secure Boot or arrange signing separately).
* Unverified breakpoint: check the active slot, ensure F5 completed preparation,
  and disconnect any other GDB client. Inspect `kernel/.gdb/<slot>-qemu-loader.log`.
* Navigation after switching guests: let `vm-use` finish its build, then restart
  clangd if the editor still displays stale results. Cache/source trees are hidden
  in the explorer, but remain available to navigation and debugging.

Ubuntu cloud image and checksum source: [Canonical images](https://cloud-images.ubuntu.com/releases/noble/release/).
QEMU GDB behavior: [QEMU documentation](https://www.qemu.org/docs/master/system/gdb.html).

# Work with Mapple

[Documentation](../README.md) / Guides

Use this guide after completing [setup](setup.md). All Make commands run from
the repository root on the Ubuntu host. The server and TUI run on the host; the
module runs in the selected guest. See the [README](../../README.md) for the overview.

[Daily loop](#daily-development-loop) · [VM selection](#select-a-vm) ·
[Kernel debugging](#kernel-debugging) · [Make reference](#make-command-reference) ·
[Options](#make-options) · [Troubleshooting](#troubleshooting)

## Daily development loop

1. Start the selected managed guest with `make vm-up` (or use your own launcher
   for a registered VM). Check the selection with `make vm-list`.
2. Build and load the module with `make run-kernel`.
3. Keep `make run-server` running in one terminal.
4. Run `make run-tui` from another terminal. It opens the TUI in a new window;
   use `FOREGROUND=1` to stay in the current terminal.
5. In the TUI, run `session create <slot-name>`, then `command info`,
   `command echo hello`, or `command pslist`. Use arrow keys in the process
   browser and Esc to return to the command prompt. `help` lists TUI commands.

For **Go changes**, run `make test` or `make check`, then stop and restart the
changed process using `make run-server` or `make run-tui`. These commands rebuild
their binaries. Restarting the server clears its in-memory sessions; create a
new session in the TUI.

For **kernel changes**, delete open sessions first (`session current`, then
`session delete <id-prefix>`). Run `make run-kernel` to build, upload, and reload,
then create a new TUI session. Deployment stops if the loaded module cannot be
unloaded. Use `make run-kernel-debug` for a run you want to attach a debugger to.

There is no automatic watcher. `make check` validates generated API code, tooling,
and Go code; `make kernel` builds the real module. Test behavior on the guest
through the TUI. `make check` does not start a VM or exercise a live kernel module.

## Select a VM

```bash
make vm-list
make vm-use NAME=dev
make vm-status
make doctor
```

The `*` in `vm-list` marks the active slot. Selection persists across terminals
and is shared by Make and VS Code. Creating another slot does not change an
existing selection. `NAME` is used by `vm-create`, `vm-add`, and `vm-use`;
`make run-kernel NAME=other` does **not** select another destination.

`vm-use` refreshes navigation for a prepared slot and invalidates prepared debugger
state. Disconnect GDB and let the guest run before switching or deploying. After
a guest kernel change, run `make vm-setup`; it provisions and reboots the guest.
Use `make vm-sync` to refresh host inputs from an already prepared guest without
provisioning or rebooting it.

## Kernel debugging

Preparation uses the selected guest's exact kernel, unstripped `vmlinux`, and
runtime module section addresses. Module symbols are relocated in a copy of the
ELF; the original build artifact is preserved. Both debugger interfaces use
`kernel/.gdb/current-debug.gdb` and its matching `current-vmlinux`.

### VS Code

1. Open the **repository root** as the workspace. Install the recommended
   **C/C++** (`ms-vscode.cpptools`) and **clangd**
   (`llvm-vs-code-extensions.vscode-clangd`) extensions.
2. Start your prepared guest and delete any open module sessions in the TUI.
3. Select **Kernel: cppdbg (full GDB MI)** in Run and Debug, then press **F5**.
   The pre-launch task builds, uploads, reloads, and prepares the module for you.
4. In the TUI, run `session create <slot-name>` and wait for it to connect.
5. Place a breakpoint in `kernel/src/control/commands/info/info.c`, inside
   `mapple_handle_info`. Run `command info` in the TUI to hit it. Inspect
   variables, step through C, and step into kernel calls.

Create the session before enabling this breakpoint: session creation also calls
INFO, and its handshake has a 30-second timeout. Once connected, `command info`
has no command timeout, so you can take time to step and inspect. If you already
have INFO breakpoints enabled when creating a session, continue promptly through
the handshake or disable them until connected.

Pause interrupts the guest through GDB. Continue resumes it. Stop detaches and
leaves the VM running. The Go applications use Make and terminal workflows;
the VS Code launch configuration is for the kernel.

### GDB TUI

With no other debugger attached and no open module sessions:

```bash
make run-kernel-debug
make gdb
```

`make gdb` opens a new terminal and connects to the prepared run. To use your
current terminal instead, run `make gdb-here`. At the GDB prompt, resume the guest:

```gdb
continue
```

In the Mapple TUI, run `session create <slot-name>` and wait for it to connect.
Then return to GDB, press Ctrl+C to interrupt the guest, and set your breakpoint:

```gdb
break mapple_handle_info
continue
```

Run `command info` in the Mapple TUI to stop at the source breakpoint. Setting
the breakpoint after session creation avoids pausing its timed INFO handshake.
`next`, `step`, `print`, `backtrace`, and `continue` work normally. Ctrl+X, A
toggles GDB's source view if it is hidden. Finish with:

```gdb
detach
quit
```

Only one debugger can attach to a QEMU stub at a time. **Stopping at a breakpoint
pauses the whole guest**, so SSH, uploads, and module requests wait until you
continue. Resume and detach before running another deployment or VM operation.
Module initialization runs before this attach workflow, so breakpoints in
initialization code are not caught by a normal debug upload.

A normal `run-kernel` upload, slot selection, managed VM start/shutdown, or
reprovisioning clears prepared debugger state. After a reboot or module reload,
run `make run-kernel-debug` before `make gdb`; F5 prepares a fresh run automatically.

### Navigate into Linux source

`make vm-setup`, `make vm-sync`, and `make kernel` generate root
`compile_commands.json` from the module and matching Linux build. With clangd
running, Ctrl+click a kernel function called from `kernel/src/` to open its
Linux definition. Use Go to Definition if your editor uses a different shortcut.

The workspace config selects `/usr/bin/clangd-18` and disables the C/C++ extension's
IntelliSense engine; C/C++ supplies the debugger and clangd supplies navigation.
The cached Linux tree is hidden in Explorer but available to both. After changing
slots, let `vm-use` finish; restart clangd if it still shows stale definitions.
Do not edit generated compilation databases or debugger scripts.

For a concrete exercise, open
`kernel/src/control/transports/tcp_server/stream_ops.c` and Ctrl+click
`kernel_sendmsg`. clangd opens its definition in the matching Linux tree. To
step into it, connect a session first, set a breakpoint on the call in that file,
then run `command echo hello` in the TUI. At the breakpoint, use Step Into in
VS Code or `step` in GDB. Continue the guest to finish the response.

## Make command reference

Targets marked **selected VM** use the persistent slot. Build and clean targets
need a prepared host cache; remote operations also need the guest running.

### Setup and VM management

| Target | What it does | Example |
| --- | --- | --- |
| `help` | Print the workflow and guide locations | `make help` |
| `setup` | Install host prerequisites; download Go dependencies; create local TLS/auth configs | `make setup` |
| `debug-kernel` | Build or reuse the pinned host-side Linux 6.6.6 debug kernel; no guest required | `make debug-kernel JOBS=2` |
| `dev-config` | Create missing local server/TUI configs; refresh saved module destinations from slots | `make dev-config` |
| `vm-create` | Create a managed Ubuntu VM and save a slot; does not start it | `make vm-create NAME=dev` |
| `vm-add` | Save SSH and debug details for an existing VM; does not provision it | `make vm-add NAME=dev HOST=192.168.122.50 SSH_USER=ubuntu` |
| `vm-list` | List slots and mark the active one | `make vm-list` |
| `vm-use` | Select a slot, refresh navigation if prepared, invalidate prepared debugger state | `make vm-use NAME=dev` |
| `vm-up` | **Selected VM, managed only:** start QEMU and wait for SSH/cloud-init; reuse a running VM | `make vm-up` |
| `vm-down` | **Selected VM, managed only:** request ACPI shutdown; returns before shutdown completes | `make vm-down` |
| `vm-status` | **Selected VM:** query live kernel, uptime, and whether the module is loaded over SSH | `make vm-status` |
| `vm-ssh` | **Selected VM:** open an SSH shell using saved credentials | `make vm-ssh` |
| `vm-setup` | **Selected VM:** install/prepare kernel, configure debugging, reboot, sync inputs, build module | `make vm-setup JOBS=2` |
| `vm-sync` | **Selected VM:** refresh host headers/source/symbols and build; no provisioning or reboot | `make vm-sync` |
| `doctor` | **Selected VM:** report tools, SSH/sudo, kernel inputs, navigation, and debug endpoint availability | `make doctor` |

`dev-config` preserves existing server/TUI settings, certificates, and token;
slot-named module profiles are refreshed. It does not restart running processes.
`doctor` does not attach to the GDB stub or pause the guest; read its warnings as
well as its exit status. `vm-status` returning an SSH error after `vm-down` means
the guest is no longer reachable; it is not a hypervisor power-state query.

### Build, run, and debug

| Target | What it does |
| --- | --- |
| `all` (also bare `make`) | Build kernel module, server, and TUI; needs a prepared selected VM cache |
| `kernel` / `build-kernel` | Incrementally build the module for the selected VM and refresh navigation |
| `server` / `build-server` | Build `user/bin/mappled` with `CGO_ENABLED=0`; no VM required |
| `tui` / `build-tui` | Build `user/bin/mapple` with `CGO_ENABLED=0`; no VM required |
| `user` | Build both Go binaries; no VM required |
| `run-server` | Build and run the HTTPS server in this terminal, using `.lab/dev/mappled.json` |
| `run-tui` | Build and open the TUI in a new terminal, using `.lab/dev/mapple.json` |
| `run-kernel` | **Selected VM:** build, upload, unload/reload module; clear prepared debugger state |
| `run-kernel-debug` | **Selected VM:** build, upload, unload/reload module; generate relocated symbols and debug scripts |
| `gdb` | **Selected VM:** open GDB TUI in a new terminal and attach to the prepared debug run |
| `gdb-here` | Same connection in the current terminal; needs a real interactive terminal |

Every run target builds what it runs. The Go server listens on
`127.0.0.1:8443` by default; it stays in the foreground until Ctrl+C. The local
TCP lab runs it as your user. Installing it as a root service or using local
character-device transport is covered by [server installation](server-installation.md).

### Checks, generation, and cleanup

| Target | What it does |
| --- | --- |
| `check` | API drift check, Python tooling tests, Go build/vet/tests, and shell syntax checks |
| `test` | Run Go tests under `user/` |
| `test-tools` | Run Python development-tooling regression tests in isolated fixtures |
| `api` | Regenerate committed Go API bindings from `user/api/openapi.yaml` with the pinned generator |
| `api-verify` | Regenerate bindings in a temporary directory and fail on differences; leaves the checkout unchanged |
| `clean` | Run `clean-kernel` and `clean-user`; needs a prepared selected VM cache |
| `clean-kernel` | Remove the selected slot/kernel's module build outputs |
| `clean-user` | Run Go cleanup and remove `user/bin/`; no VM required |

After editing the API schema, run `make api`, review the generated diff, then run
`make check`. Cleanup leaves VM disks, credentials, the shared debug kernel, and
the guest's loaded module in place.

## Make options

Pass options as `make <target> OPTION=value`. Quote values containing spaces.
Creation and registration options are saved in the slot; run options apply to
that invocation. Use `vm-use` to select a destination before other VM commands.

| Option | Used by | Meaning and default |
| --- | --- | --- |
| `NAME` | `vm-create`, `vm-add`, `vm-use` | Slot name; defaults to `dev` for creation/registration; required for selection. Starts with a letter; then letters, digits, `_`, or `-` |
| `HOST` | `vm-add` | VM address reachable from the host; prompted if omitted |
| `SSH_USER` | `vm-add` | Guest login account; prompted if omitted |
| `SSH_PORT` | `vm-add` | Host-reachable SSH port; default `22` |
| `SSH_KEY` | `vm-add` | Private key path; otherwise use default SSH identities/agent |
| `ASK_PASS` | `vm-add` | `1` prompts privately for SSH and sudo passwords; default is key auth and passwordless sudo |
| `GDB_ENDPOINT` | `vm-add` | QEMU GDB stub as `host:port`; default `127.0.0.1:1234`. Your QEMU launcher must expose it |
| `MODULE_PORT` | `vm-add` | TCP port reachable by the host server; default `4444` |
| `SOURCE` | `vm-create`, `vm-add` | Path to a fully built, compatible kernel tree; omitted means build the pinned kernel during `vm-setup` |
| `VMLINUX` | `vm-create`, `vm-add` | Exact unstripped kernel debug image; defaults to `SOURCE/vmlinux`. For `vm-create`, supply `SOURCE` with it |
| `MEMORY` | `vm-create` | Guest RAM in MiB; default `4096` |
| `CPUS` | `vm-create` | Guest virtual CPUs; default `2` |
| `DISK` | `vm-create` | Initial virtual disk capacity, such as `30G`; default `30G` |
| `JOBS` | `debug-kernel`, `vm-setup` | Host Linux build jobs; default CPU count capped at `4`. Does not set Go or module build parallelism |
| `NO_INSTALL` | `setup` | `1` skips host apt installation and KVM group changes |
| `FOREGROUND` | `run-tui`, `gdb` | `1` uses the current terminal instead of opening a window |
| `TERMINAL` | `run-tui`, `gdb` | Terminal command prefix, for example `'xterm -e'`; otherwise detect GNOME Terminal, Konsole, or the desktop default |
| `MODULE_PARAMS` | `run-kernel`, `run-kernel-debug` | Quoted, space-separated `key=value` arguments passed to `insmod`; empty by default. The module currently declares no configurable parameters |
| `GOTOOLCHAIN` | Go-backed targets | Explicit Go toolchain override; normally leave automatic selection enabled |

Managed VM creation chooses available host SSH/module/GDB ports automatically;
`SSH_PORT`, `MODULE_PORT`, and `GDB_ENDPOINT` are registration options. Its guest
module port is 4444. To change a saved slot, edit `.lab/slots/<name>.json` while
commands are idle, then reselect it. Re-running `vm-add` with the same name fails
instead of overwriting it.

Examples:

```bash
make vm-create NAME=experiment MEMORY=2048 CPUS=2 DISK=25G
make vm-use NAME=experiment
make vm-up
make vm-setup JOBS=2
make run-tui TERMINAL='xterm -e'
make gdb FOREGROUND=1
```

The last command requires a prepared debug run; see [GDB TUI](#gdb-tui).

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| No active VM / missing kernel cache | Run `make vm-list`, select a slot, start its VM, then run `make vm-setup` |
| SSH fails | Check `host`, `ssh_port`, `user`, and credentials in the slot; confirm the guest is running and its SSH server/port forwarding work |
| Sudo fails | Register using `ASK_PASS=1` or update `sudo_password` in the private slot; otherwise the guest needs passwordless sudo |
| VM boot is very slow | Check KVM/firmware/nested virtualization; inspect `.lab/vms/<slot>/console.log` |
| Kernel build runs out of memory | Retry with `make debug-kernel JOBS=2` or `JOBS=1`; reduce guest RAM or stop the guest while compiling on the host |
| Kernel release mismatch / missing source or symbols | Verify slot `source`/`vmlinux`, then run `make vm-setup`; it reboots the guest into the selected kernel |
| Module unload fails | Delete all sessions in every connected client/server, close any local character-device users, then retry the upload |
| TUI cannot reach server | Start `make run-server`, check its terminal output, and confirm port 8443 is free |
| TUI has stale credentials after a local edit | Keep `.lab/dev/mappled.json` and `.lab/dev/mapple.json` consistent; restart the affected processes. `dev-config` preserves edits |
| No graphical terminal found | Use `FOREGROUND=1`, `make gdb-here`, or `TERMINAL='xterm -e'` on a graphical host |
| No prepared debug run | Run `make run-kernel-debug` after boot/reload/selection, then `make gdb`; F5 prepares its own run |
| Breakpoints remain unverified | Confirm the active slot, let F5 preparation finish, and detach other debugger clients; inspect `kernel/.gdb/<slot>-qemu-loader.log` |
| SSH or a request hangs during debugging | Continue the guest in the debugger; a source breakpoint pauses the entire VM |
| Ctrl+click cannot find a kernel definition | Run `make kernel`, confirm clangd 18 is enabled, then restart clangd after indexing or a slot change |

For the local setup and file layout, see [setup](setup.md). Protocol and session
references are linked from [architecture](../architecture/overview.md).

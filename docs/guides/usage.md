# Work with Mapple

[Documentation](../README.md) / Guides

Use [setup](setup.md) for the first checkout. All commands run from the repository
root.

## Find command help

```bash
make                               # Browse commands and short descriptions
make machine-create HELP=1         # Arguments, defaults, examples and effects
make help COMMAND=kernel-remove    # Another way to explain one command
make kernel-remove                 # Missing required KERNEL: shows usage only
```

`HELP=1` never executes the command or its prerequisites, even if you also supply
valid arguments. It works on every public target, including `setup`, `check` and
`clean`. Plain commands still run normally when their required inputs or saved
selection are available. A partially specified command reports missing required
options and shows its usage.

Removal requires a named destination: `make kernel-remove KERNEL=old-kernel`,
`make machine-remove MACHINE=old-dev`, or `make machine-destroy MACHINE=old-dev`.
These commands never infer the object to delete from the active selection.

## Select a destination

```bash
make machine-list
make machine-use NAME=dev
make module-run                  # Uses dev and its assigned kernel
make module-run MACHINE=client   # One-command override; dev stays active
make module-build KERNEL=baseline # Offline build; no machine needed
```

There is one persistent active machine and no independent active kernel.
Deployment uses its destination's assigned kernel. A conflicting `KERNEL`
override fails; change the machine's kernel with `machine-prepare` instead.

Existing servers, TUIs and debuggers stay attached to their original machines
when selection changes. Navigation follows the active machine. An override does
not retarget editor navigation. Lists use cached observations and print blockers;
`machine-inspect` refreshes live information. `machine-show` includes timestamps.

## Daily development

Build/reload with `module-run`; keep `server-run` in one terminal and open
`tui-run` from another. Go run commands rebuild their component. Before reloading
Mapple, close application sessions (`session delete <id-prefix>` in the TUI).
A reload invalidates previous module addresses, so prepare a new debug run.

`clean` removes application build outputs only. Navigation databases and indexes,
frozen run files, Linux builds, downloads, machine records and disks are preserved. Run `check` for repository
checks and `doctor` for the selected machine's live blockers. Checks do not
attach to a QEMU debug stub or pause a machine.

## Kernel debugging

```bash
make module-debug
# In the TUI, create a fresh session after this reload: session create dev
make gdb-open                     # New GDB TUI terminal
make gdb-attach RUN=run-...        # Current terminal; RUN comes from debug-list
```

`module-debug` builds, checks the target, uploads, loads, and saves a frozen run
with its exact ELF and runtime addresses. `debug-list` lists these runs.
If exactly one prepared run exists for the machine, `gdb-open` selects it;
otherwise supply `RUN=...`. `FOREGROUND=1` keeps GDB TUI in this terminal.
GDB initially pauses the guest. Set a breakpoint and resume it:

```gdb
break mapple_handle_info
continue
```

Run `command info` in the TUI to hit it. Use `next` to step and
`print context->session->id` to inspect a value. `continue` completes the request;
`detach` then `quit` ends the debug session.

The best available mode is reported: `module`, `kernel-symbol`, or
`kernel-source`. Require a minimum explicitly:

```bash
make module-debug MACHINE=dev MIN_DEBUG=kernel-source
```

A missing endpoint or insufficient mode fails before upload. Normal
`module-run` does not need source, symbols, or readable section addresses.
GDB attachment checks the boot ID, load receipt, module build ID when available,
and section address again. Detached runs can be reattached while still current. It holds a machine
and endpoint lock until detach, preventing another command from reloading that
machine's module. Different machines/endpoints can be debugged simultaneously.

For VS Code:

```bash
make vscode-open MACHINE=dev MIN_DEBUG=kernel-source
```

This opens a generated machine-bound workspace. Install the recommended C/C++
and clangd extensions, select **Mapple: dev**, and press F5. Its prelaunch task
builds/uploads for that frozen destination, so close existing application sessions
before F5. F5 resumes the guest automatically. Create a fresh TUI session with
the info breakpoint disabled, then enable the breakpoint and run `command info`.
Stop detaches and resumes the guest; it does not kill QEMU. Reusing F5 prepares the workspace's run again after detach.
The toolbar's Restart button detaches, rebuilds and reloads Mapple, refreshes its
addresses and symbols, and reconnects without rebooting the machine. Existing
application sessions belong to the previous module load; create a new TUI session
after restarting.
The post-debug task clears exited sessions. If VS Code or its launcher crashes,
the next F5 also recovers stale session state after confirming its processes have
exited. A surviving debugger retains the machine locks and prevents reloads.
Open another machine workspace for a simultaneous session.

Set a breakpoint in `kernel/src/control/commands/info/info.c` after creating a
TUI session, then run `command info`. Create the session before enabling the
breakpoint so its handshake does not time out. You can step into Linux functions
such as `kernel_sendmsg` when the kernel slot has source and DWARF.
When matching Linux DWARF and `vmlinux-gdb.py` are available in the managed kernel
snapshot, GDB loads its helper commands, including `lx-ps` and `lx-dmesg`. In VS Code,
use `-exec lx-ps` or `-exec lx-dmesg` in the Debug Console. Only that loader is
added to the session's auto-load safe path; your global GDB configuration is unchanged.
Mapple's local editor integration supplies the **Mapple kernel (GDB)** debug type
and uses Microsoft's C/C++ adapter underneath. Debug Console execution commands
such as `-exec continue`, `-exec next`, `-exec step`, `-exec finish` and
`-exec interrupt` synchronize the toolbar with the same GDB session. Inspection
commands pass through normally. `vscode-open` checks the installed integration
version before opening a new window; after updating, generate a new workspace
with `make vscode-open`. Old generated workspaces retain their old debug configuration.
Pausing kernel execution also pauses that machine's networking.

To verify the actual editor controls against an already prepared, disposable
machine with full kernel-source support:

```bash
make test-debugging MACHINE=debug-test
```

This reloads Mapple repeatedly and uses an isolated VS Code profile. It checks
toolbar and console Pause/Continue, Restart while running and paused, source
breakpoints, Step Into/Over/Out, memory/disassembly, Stop, and F5 again.
Results and the DAP transcript are saved under `.lab/debugging-verification`.
The command leaves the supplied test machine registered. On a headless host it
requires `xvfb-run`; `make check` remains independent of live machines and editors.

## Source navigation

Ctrl+click / Go to Definition uses clangd independently of GDB. It works with the
VM powered off and without `vmlinux` or debug symbols. The active machine's kernel
controls static navigation; existing GDB sessions retain their original targets.

```bash
make navigation-setup                       # Install/update the local VS Code integration
make navigation-prepare KERNEL=baseline      # Build compiler context and index locally
make navigation-status KERNEL=baseline       # Inspect this kernel's preparation receipt
make machine-use NAME=dev                    # Editor follows dev's assigned kernel
make navigation-status                      # Active context and live editor states
```

`setup` installs the editor integration when `code` is present, unless
`NO_INSTALL=1`. After installing/updating it, reload an already-open VS Code
window once. The recommended clangd extension provides the language engine;
the local **Mapple Navigation** extension manages selection and status and supplies
the debug control bridge described above. Clangd provides source navigation; GDB
provides runtime debugging. In a trusted checkout, opening the normal
repository folder is sufficient; a debug workspace is optional.

`navigation-prepare` uses the active machine's kernel unless `KERNEL` or `MACHINE`
is supplied. It refreshes the compiler commands, indexes the sources, and checks
jumps into unopened implementations. It does not contact a machine, upload Mapple,
or change the active selection. The status command also works offline.

The status bar identifies the machine and exact kernel slot and shows:

| State | Meaning |
| --- | --- |
| Switching kernel / Loading index | The language server is being replaced or restoring its index |
| Indexing | Background indexing is still running |
| Ready | Indexing completed and Mapple/matching Linux implementation checks passed |
| Limited navigation | Mapple/header navigation is available, or Linux commands were derived rather than recorded; full Linux coverage is not established |
| Select a machine / Assign a kernel / Prepare navigation | The required selection or compilation database is absent |
| Navigation error | A startup or destination check failed; click the status bar for details/output |

Click the status bar or run **Mapple: Prepare source navigation** from the Command
Palette to start preparation. Preparing the index moves the initial Linux indexing
cost out of the edit loop. A cached index still takes time to load, and opening a
new file can require parsing; readiness does not promise zero latency for every jump.

Switching to a different kernel stops the old language server and starts a new
one against that kernel's database. Switching between machines assigned to the
same kernel keeps the server. Old Linux source tabs remain viewable, but inactive
kernel files are excluded from this session's dynamic definition index. Make
commands publish selection asynchronously to open editors; wait for their status
to show the new kernel and become ready before relying on implementation jumps.

Each kernel owns `.lab/navigation/<kernel-id>/compile_commands.json`, its clangd
cache, preparation log and receipt. Module builds refresh these inputs; unchanged
compiler context retains its index. `clean` preserves them. Compiler or context
changes invalidate the preparation receipt. Kernel removal also removes that
kernel's navigation data. The root `compile_commands.json` remains available to
other tools, while VS Code's launcher uses the per-kernel directory directly.

For SDKs, compiler paths are resolved in the kernel's toolchain environment.
Clangd may query only the explicitly recorded compiler paths, rather than an
unrestricted driver pattern. `CLANGD` overrides the native engine for preparation;
the prepared context records it. Generated debug workspaces also carry a custom
`MAPPLE_LAB_DIR` when used. For a plain folder using alternate state, set
`mapple.navigation.labDirectory` and pass the same absolute path through the
clangd argument `--mapple-state-dir=/absolute/lab` in workspace settings.

The [source navigation review](../reference/source-navigation-review.md) records
the measurements and the kernel-switching reproduction behind this design.
`make test-navigation` exercises real VS Code switching, open old-kernel tabs,
cache reuse, and reopening after clean in a disposable checkout. It requires the
`code` CLI, clangd extension, and `xvfb-run` on a headless host; it leaves the
developer's selection and normal VS Code profile alone.

## Command reference

`NAME` names a new entry (or the entry selected by `machine-use`). `MACHINE`
overrides the active machine. `KERNEL` identifies a kernel slot. Names can start
with a letter or digit and contain letters, digits, dots, underscores and hyphens
(for example, `6.6.6-debug-v1`).

| Command | Behavior |
| --- | --- |
| `setup` | Install Ubuntu host prerequisites and Go dependencies; no target changes |
| `kernel-create` | Define a Linux build recipe; requires `NAME`, accepts `VERSION`, `PROFILE=debug`, `ARCH`, `CONFIG`, `PATCHES`, compiler settings |
| `kernel-build` | Build/reuse the selected `KERNEL`; new Linux source/config/patches require a new slot |
| `kernel-import` | Copy supplied artifacts into a new `NAME`; accepts `BUILD`, `SOURCE`, `VMLINUX`, `DEBUG_INFO`, `SYSTEM_MAP`, `IMAGE`, `MODULES` |
| `kernel-enrich` | Add missing matching artifacts to `KERNEL`; existing artifacts cannot be replaced |
| `kernel-list` | List release, architecture, tier, installability, state and assigned machines |
| `kernel-show` | Show identity, artifact paths, toolchain and provenance for `KERNEL` |
| `kernel-remove` | Require explicit `KERNEL`; refuse referenced kernels; `CASCADE=1` also removes referencing machine records, preserving VM assets |
| `machine-create` | Create an Ubuntu x86-64 QEMU machine with a requested `KERNEL`; requires `NAME`; does not start/install |
| `machine-add` | Register SSH details, inspect, and record the existing kernel; requires `NAME`; does not provision |
| `machine-configure` | Update SSH, module port, GDB endpoint or installer settings without changing the kernel; `GDB_ENDPOINT=none` disables debugging |
| `machine-use` | Persist `NAME` as the active machine; refresh editor navigation without contacting or rebuilding the target |
| `machine-list` | Show active selection, assignment, last observed release, tier, match and blockers |
| `machine-show` | Show the machine record, observations and transition results, with credentials redacted |
| `machine-inspect` | Refresh read-only live kernel/OS observations |
| `machine-adopt` | Copy/fetch matching inputs for the running kernel; `KERNEL` selects an imported/observed slot; `OFFLINE=1` prevents built-in downloads |
| `machine-prepare` | Install `KERNEL`, reboot, verify, then commit association; retains previous kernel/fallback |
| `machine-start` | Start an owned QEMU VM and wait for SSH/cloud-init; reuse an already running VM |
| `machine-stop` | Request graceful ACPI shutdown of an owned VM; returns before shutdown completes |
| `machine-ssh` | Open SSH with the saved connection details |
| `machine-remove` | Require explicit `MACHINE`; retire the record; preserve VM disks and saved connection details in `retired.json` |
| `machine-destroy` | Require explicit `MACHINE`; stop and delete the disk/seed of an owned QEMU VM, then retire its record |
| `module-build` | Incrementally build Mapple against `KERNEL` or the selected machine's assignment; no live target needed |
| `navigation-setup` | Package/install the local Mapple Navigation VS Code extension; no npm tooling required |
| `navigation-prepare` | Build the selected kernel's compiler context and persistent index offline, then verify implementation jumps |
| `navigation-status` | Show active navigation context and live editor states, or a named `KERNEL`'s preparation data; no SSH |
| `module-run` | Build, verify available identity evidence, upload, unload/reload; no debugger preparation |
| `module-debug` | Build/upload and prepare the best available debug mode, subject to `MIN_DEBUG` |
| `server-build` | Build the Go server into `.lab/builds/go/mappled` |
| `server-run` | Build and run the server in this terminal with a machine-bound TLS/auth config and reserved port |
| `tui-build` | Build the Go TUI into `.lab/builds/go/mapple` |
| `tui-run` | Build and launch the matching machine's TUI in a new terminal |
| `gdb-open` | Open GDB TUI for a prepared `RUN`; use `FOREGROUND=1` for this terminal |
| `gdb-attach` | Attach GDB in this terminal; accepts `RUN` |
| `vscode-open` | Generate/open a machine-bound VS Code workspace; `NO_OPEN=1` only generates it |
| `debug-list` | List module run IDs, machines, kernels, state and debugger mode |
| `doctor` | Inspect the machine and report artifact, identity, API, compiler and module-loading blockers |
| `clean` | Remove application build outputs, preserving preparation data and frozen runs |
| `test` | Run Go unit tests |
| `test-tools` | Run development-tool regression tests |
| `test-navigation` | Run native clangd and real VS Code lifecycle tests in a disposable checkout |
| `test-debugging` | Exercise real VS Code debugger controls; requires explicit `MACHINE` naming a prepared, disposable Tier 3 target; reloads Mapple repeatedly |
| `check` | Verify generated API, run tool tests, build/vet/test Go, and compile-check Python |
| `api` | Regenerate Go API bindings |
| `api-verify` | Verify bindings match the API without modifying source files |
| `help` | Browse command descriptions; `COMMAND=<command>` shows its full help; `<command> HELP=1` also works |

## Options

| Option | Meaning |
| --- | --- |
| `NAME`, `MACHINE`, `KERNEL`, `RUN` | Entry/run selection as described above |
| `HOST`, `SSH_USER`, `SSH_PORT`, `SSH_KEY` | `machine-add` connection details; port defaults to 22; key otherwise uses SSH defaults/agent |
| `ASK_PASS=1` | Privately prompt for SSH/sudo passwords during registration |
| `GDB_ENDPOINT` | Existing machine's configured debugger endpoint, as `host:port` or a host serial-device path such as `/dev/ttyUSB0`; omitted means no debugger access |
| `MODULE_PORT` | Host-reachable module TCP port; default 4444 for existing machines; managed QEMU allocates forwarding automatically |
| `VERSION`, `ARCH`, `LINUX_ARCH` | Linux release, normalized machine architecture, and optional Kbuild architecture override |
| `DEFCONFIG`, `IMAGE_PATH` | Override the default Linux configuration target and relative boot-image path; unfamiliar architectures require an explicit `CONFIG` and image path |
| `TOOLCHAIN` | JSON compiler/SDK description with `cc`, `cross_compile`, optional `llvm`, and string-valued `env` (for example SDK `PATH`) |
| `BTF` | Optional BTF blob to retain as metadata during import/enrichment; does not establish build support |
| `PROFILE`, `CONFIG`, `PATCHES` | Debug recipe with optional custom `.config` and directory of sorted `*.patch` files |
| `CC`, `CROSS_COMPILE`, `LLVM` | Persisted compiler, binary-tool prefix, and optional `LLVM=1`; specify a compatible SDK/compiler already on the host |
| `BUILD`, `SOURCE`, `VMLINUX`, `SYSTEM_MAP` | Prepared build tree, Linux source, debug ELF, or text symbol map to copy during import/enrichment |
| `IMAGE`, `MODULES` | Boot image and module archive (release directory at archive root) for installation |
| `DEBUG_INFO`, `BUILD_ID` | Separate matching Linux debug ELF, or expected GNU build ID on import when no symbol ELF is available |
| `JOBS` | Linux build parallelism (default CPU count capped at 4); navigation preparation workers (default 4) |
| `MEMORY`, `CPUS`, `DISK` | Managed VM RAM in MiB, CPU count, and capacity; defaults 4096, 2, and 30G |
| `MIN_DEBUG` | Minimum debug mode; default `module`, highest available mode is selected |
| `MODULE_PARAMS` | Space-separated `key=value` module arguments; Mapple currently declares none |
| `TERMINAL`, `FOREGROUND` | Terminal launcher prefix, or `FOREGROUND=1` to use this terminal |
| `NO_INSTALL`, `TOOLCHAIN_PACKAGES` | Skip host/editor-extension installation or explicitly add known Ubuntu toolchain packages during setup |
| `OFFLINE` | Prevent built-in adoption downloads; custom fetchers must honor the inherited option |
| `INSTALLER`, `FETCHER` | Local executable integration hooks described below |
| `SIGNER` | Optional local executable to sign a frozen module copy before upload; may be persisted during kernel creation/import |
| `CASCADE` | Explicitly retire machine records referencing a removed kernel |
| `NO_OPEN` | Generate a VS Code workspace without opening a window |
| `GDB` | Override the GDB executable; otherwise use native GDB or gdb-multiarch for foreign targets |
| `CLANGD` | Native clangd executable for navigation preparation; default `clangd-18`; distinct from the repository's editor launcher |
| `NAV_TIMEOUT` | Navigation indexing timeout in seconds; default 1800; partial index shards remain reusable after interruption/failure |
| `MAPPLE_LAB_DIR` | Optional isolated state directory; default is this checkout's `.lab` |

## Integration hooks

Configure an unfamiliar boot system with `machine-add INSTALLER=/absolute/executable`.
The controller invokes the executable locally with:

```text
installer install <machine-record.json> <kernel-record.json> <kernel-directory>
installer commit  <machine-record.json> <kernel-record.json> <kernel-directory>
```

`install` stages/installs the kernel and configures the next boot plus any fallback;
it must not reboot. The controller reboots and verifies the build identity.
`commit` persists the boot choice after verification. Nonzero exit fails the
transition. The records are private and may contain credentials; hooks are trusted
local programs. The previous kernel must remain installed. The built-in installer refuses conflicting
boot images with the same release name; rebuild with a unique `LOCALVERSION` or
use a custom installer that can preserve both builds. Built-in preparation
requires an identifiable matching build, normally supplied by `vmlinux`'s GNU build ID.

For another package source, run `machine-adopt FETCHER=/absolute/executable`.
The controller invokes:

```text
fetcher <machine-record.json> <kernel-record.json> <output-directory>
```

Write `artifacts.json` in the output directory, mapping artifact keys (`build`,
`source`, `vmlinux`, `debug_info`, `system_map`, `image`, `modules`, `config`, `btf`) to relative paths contained
within it. Do not replace existing artifacts. Honor `OFFLINE=1`. The controller
seals the collected inputs and reports the resulting tier. BTF or a version string
alone does not create build support; reconstruction is a manual preparation task.

A `SIGNER` executable receives `<module.ko> <kernel-record.json>
<machine-record.json>` and signs that frozen copy in place. It can call Linux's
`sign-file` with an authorized key. Nonzero exit prevents upload. Keys need not
be stored in kernel snapshots, and signing does not change interface compatibility.

## State and recovery

All large managed files live under `.lab`: `kernels/<name>`, `machines/<name>`,
`builds/`, `runs/<id>`, and `downloads/`. Records are private JSON; use show commands
for inspection. Do not edit sealed kernel artifacts. Changes to Linux require a
new slot; additional matching source/symbols can enrich one.

A failed preparation leaves its transition record and previous association.
Inspect `machine-show`, the VM's `vm/console.log`, or your machine's console.
A one-shot boot uses the old default on the next reboot; this is not a guarantee
that an inaccessible machine recovered. Do not rerun module deployment against a
known mismatch. `release-only` reports limited identity evidence, not a verified
exact build. Imported data should retain evidence tying it to the target.

## Verify a fresh checkout

`make check` runs the normal repository checks. The full development acceptance
test needs a disposable fresh clone, KVM/QEMU, Xvfb (`sudo apt-get install xvfb`),
and the recommended Microsoft C/C++ extension installed on the host:

```bash
python3 scripts/tests/fresh_clone.py --repo /absolute/path/to/disposable-clone
```

It runs setup, builds Linux, creates two test VMs, exercises the stack and real
GDB/VS Code debugging at each tier, switches kernels, then destroys its test VMs.
Logs and results are saved under that clone's `.lab/acceptance/`. Kernel downloads
and build files remain available for inspection. Use this only in a disposable
checkout; `--resume` continues that test lab after an interrupted run.

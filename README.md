# Mapple

Mapple is a kernel-resident agent (`mapple.ko`) that gives a control server
full access to the machine it's installed on, over a small versioned wire
protocol that's designed to keep growing new capabilities.

`mappled` is that control server: a root HTTPS daemon, and the only thing
ever allowed to speak the agent's wire protocol directly. Everything else —
the `mapple` terminal UI, your own scripts, another frontend you write —
talks to `mappled` over its authenticated OpenAPI-defined HTTP API instead.
The kernel side never touches a network or a terminal on its own.

```
 a frontend ──HTTPS/OpenAPI──▶ mappled (control server, root) ──wire protocol──▶ mapple.ko
 (mapple TUI,                                                   (TCP :4444 or /dev/mapple)
  or your own client)
```

## Repo layout

| Path | What |
| --- | --- |
| `kernel/` | The kernel module (`mapple.ko`): sessions, transports (TCP + chardev), commands (`echo`, `pslist`, `info`). Cross-built against a target's exact running kernel via `kernel/scripts/`. |
| `shared/` | The wire ABI both the kernel module and any client compile against (`shared/commands.h`, `shared/shared.h`) — the one header pair that has to stay in sync across the C/Go boundary. |
| `user/` | The Go control plane: `user/server/internal/moduleclient` (the private wire-protocol client), `user/api` (OpenAPI contract + generated server/client), `user/server` (the `mappled` implementation), `user/tui` (the `mapple` TUI), `user/cmd/{mappled,mapple}` (the two binaries). |
| `user/packaging/systemd/` | The `mappled.service` unit. |
| `docs/` | Architecture and protocol docs — see below. |
| `scripts/lab.py` / `.lab/` | Development commands and private local VM slots, active selection, credentials, and QEMU disks. Existing `lab.local.env` targets are imported by setup. |

## Development

Develop on an Ubuntu host with an Ubuntu QEMU guest. On minimal Ubuntu
installations, first install the clone/bootstrap tools with
`sudo apt-get update && sudo apt-get install -y git make python3`.
Then, from the cloned repository:

```bash
make setup
make vm-create NAME=dev          # create a new Ubuntu QEMU VM
make vm-use NAME=dev             # persistent selection, shared with VS Code
make vm-up
make vm-setup                    # provision debug prerequisites, reboot, sync, build
```

The module currently targets the Linux 6.6 API. VM setup builds and installs a
compatible 6.6.6 debug kernel in the Ubuntu guest, keeping application C unchanged.
Reuse an existing built kernel with `SOURCE=... VMLINUX=...` on `vm-create` or
`vm-add`; otherwise allow 10 GiB free and time for the first kernel build.

For an existing VM instead:

```bash
make vm-add NAME=server HOST=192.168.122.76 SSH_USER=ubuntu \
  GDB_ENDPOINT=127.0.0.1:6722
make vm-use NAME=server
make vm-setup
```

Add `SSH_KEY=/path/to/key` or `ASK_PASS=1` for private password prompts.
The existing VM's QEMU launcher must expose the configured GDB stub.
Custom kernels can supply `SOURCE=/path/to/linux VMLINUX=/path/to/vmlinux`.
Setup imports existing lab targets without changing their custom kernel inputs.

```bash
make kernel                     # selected guest's module, incremental
make server                     # Go HTTPS daemon
make tui                        # Go terminal UI
make                            # all three
make check                      # generation, tooling checks, Go build/vet/tests

make run-kernel                 # build + upload + reload, no debugger required
make run-server                 # daemon in this terminal
make run-tui                    # TUI in a new terminal; matching credentials prefilled

make run-kernel-debug           # build + upload + relocated symbols/source paths
make gdb                        # GDB TUI in a new terminal, or use VS Code F5
```

`make vm-list`, `make vm-use NAME=...`, and `make doctor` manage the lab.
Kernel source Ctrl+click navigation, VS Code source breakpoints/stepping, and GDB
TUI use the selected guest's source, headers, and symbols. Go apps use Make.
Run `make help` for commands; [development guide](docs/development.md) covers
setup, slots, debugging, terminals, and troubleshooting.

[Production installation](docs/mapple-install.md) covers TLS, systemd, and operator profiles.

## Commands

Every command below runs against a session (`$ID`), which you get by
creating one first — `session create <slot>` in the `mapple` TUI, or
`POST /v1/sessions` with `{"type":"tcp","host":"...","port":4444}`
(see `docs/mapple-install.md` for the full request). In the TUI, use `command info`, `command echo <text>`, or `command pslist` after connecting.

### `info` — sanity-check a session (TUI: `command info`)

Returns the kernel-side session ID, the module's max concurrent sessions,
and a bitmap of which commands this module build supports. Cheap, useful
right after creating a session.

```
curl -sk -H "Authorization: Bearer $TOKEN" -X POST \
  https://$HOST/v1/sessions/$ID/commands/info
```

### `echo` — round-trip check (TUI: `command echo <text>`)

Sends a payload through the kernel module and back unchanged. A liveness
check for the wire protocol itself, not a real capability.

```
curl -sk -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"data":"aGVsbG8="}' -X POST \
  https://$HOST/v1/sessions/$ID/commands/echo
```

### `pslist` — process/thread snapshot (TUI: `command pslist`)

A point-in-time snapshot of every process and thread: identity
(executable, command line), security context (uid/gid, capabilities,
LSM), open file descriptors, and network sockets. Optional group flags
narrow what comes back — see `docs/pslist-snapshot.md` for the full
collection model.

```
curl -sk -H "Authorization: Bearer $TOKEN" -X POST \
  https://$HOST/v1/sessions/$ID/commands/pslist
```

## Docs

- `docs/development.md` — setup, VM slots, builds, runs, navigation, and kernel debugging.
- `docs/usermode-architecture-plan.md` — the Go control-plane design (wire
  protocol v3, the module-client library, the HTTPS API, the TUI).
- `docs/mapple-install.md` — production installation.
- `docs/pslist-snapshot.md` — what a `pslist` snapshot actually collects and
  the guarantees around it (request-scoped, no retained state between calls).
- `docs/session-lifecycle.md` — kernel-side session/transport invariants.

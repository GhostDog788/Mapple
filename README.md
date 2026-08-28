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
| `lab.example.env` / `lab.local.env` | Per-machine target config (IPs, SSH creds, debug endpoints) for the kernel build/deploy/debug scripts. Copy the example to `lab.local.env` (gitignored) and fill in your machines. |

## Building

```
make            # kernel module (default target's kernel-cache) + user/ (build, vet, test, binaries)
make kernel     # kernel module only
make user       # Go side only: OpenAPI regen-check, build, vet, test, and
                #   CGO_ENABLED=0 static binaries at user/bin/{mappled,mapple}
```

Kernel module builds are cross-compiled against a specific target's exact
running kernel, driven by `lab.local.env` and `kernel/scripts/0*.sh`
(`00-check-target.sh`, `02-setup-host-build.sh`, `03-build-module.sh`,
`04-deploy-and-prep-gdb.sh`) — see their own header comments and
`docs/session-lifecycle.md` / `docs/pslist-snapshot.md` for what the module
itself is doing.

## Running the control plane

```
cd user
bash scripts/dev-setup.sh                     # local self-signed cert + bearer token + dev/mappled.json
go run ./cmd/mappled -config dev/mappled.json # in one terminal

# in another: fill in the server_url/bearer_token dev-setup.sh printed
go run ./cmd/mapple -init && $EDITOR ~/.config/mapple/config.json
go run ./cmd/mapple
```

`docs/mapple-install.md` covers the production path: real TLS provisioning,
bearer-token generation/hashing, the systemd unit, firewall rules for the
kernel module's TCP port, and per-operator TUI profiles.

## Commands

Every command below runs against a session (`$ID`), which you get by
creating one first — `n` in the `mapple` TUI, or
`POST /v1/sessions` with `{"type":"tcp","host":"...","port":4444}`
(see `docs/mapple-install.md` for the full request). In the TUI, each
command is a single key once you're viewing a session's detail screen.

### `info` — sanity-check a session (TUI: `i`)

Returns the kernel-side session ID, the module's max concurrent sessions,
and a bitmap of which commands this module build supports. Cheap, useful
right after creating a session.

```
curl -sk -H "Authorization: Bearer $TOKEN" -X POST \
  https://$HOST/v1/sessions/$ID/commands/info
```

### `echo` — round-trip check (TUI: `e`)

Sends a payload through the kernel module and back unchanged. A liveness
check for the wire protocol itself, not a real capability.

```
curl -sk -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"data":"aGVsbG8="}' -X POST \
  https://$HOST/v1/sessions/$ID/commands/echo
```

### `pslist` — process/thread snapshot (TUI: `p`)

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

- `docs/usermode-architecture-plan.md` — the Go control-plane design (wire
  protocol v3, the module-client library, the HTTPS API, the TUI).
- `docs/mapple-install.md` — production installation.
- `docs/pslist-snapshot.md` — what a `pslist` snapshot actually collects and
  the guarantees around it (request-scoped, no retained state between calls).
- `docs/session-lifecycle.md` — kernel-side session/transport invariants.

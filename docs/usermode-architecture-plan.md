# Mapple Go Control-Plane Architecture and Migration Plan

## Summary

Replace the C user-mode client with a Go control plane consisting of:

- `mappled`: a root HTTPS daemon and the only component permitted to communicate with kernel modules.
- `mapple`: a nonprivileged Bubble Tea v2 TUI that communicates exclusively with `mappled`.
- A private Go module-client library, importable only by server code, implementing wire protocol v3 over TCP or a character-device stream.
- An OpenAPI 3.0.3 contract defining the sole public API for the TUI, automation, and acceptance tests.

The topology is many-to-many: each daemon may connect to many module PCs, and each module may simultaneously accept sessions from multiple daemons. One HTTP session lease maps one-to-one to one kernel session. Commands on the same lease are synchronous; concurrency requires another lease.

## Key Interfaces and Behavior

### Wire protocol v3

- Keep the kernel implementation in C and bump `MAPPLE_PROTOCOL_VERSION` from 2 to 3 without backward compatibility.
- Replace the 20-byte header with a packed 24-byte big-endian header:
  - `u32 magic`
  - `u16 version`
  - `u16 command`
  - `u32 flags`
  - `u32 status`
  - `u64 payload_size`
- Requests must set `status` to zero. Successful responses use zero; recoverable command failures use a stable nonzero status and no payload.
- Define stable statuses for invalid argument, unsupported command, busy, not found, permission denied, resource exhaustion, and internal failure.
- Preserve sized and streaming payload modes. Invalid framing, partial responses, transport errors, or failures after a response begins remain session-fatal.
- Add `INFO`, valid throughout a session, returning:
  - Kernel session ID
  - Module-wide maximum session count
  - Reserved field for future extension
  - 64-bit supported-command bitmap
- Document the complete v3 byte layout and status semantics separately from the C representation. Add golden byte fixtures consumed by Go tests.
- Keep kernel session enforcement authoritative. Each module still permits at most eight sessions globally across all transports and servers.

### Private Go module client

- Create one Go 1.26 module under `user/`, using module path `github.com/GhostDog788/Mapple/user`.
- Place transport, wire protocol, session lifecycle, and typed operations below `server/internal/moduleclient` so the TUI cannot import them.
- One `ModuleClient` instance owns one TCP or character-device stream and one kernel session.
- Expose typed `Info`, `Echo`, and `ProcessSnapshot` methods. Never expose file descriptors, streams, wire headers, or command IDs.
- Serialize complete request/response transactions per instance and reject concurrent calls with `session_busy`.
- Mark a session broken after fatal protocol or transport errors; never transparently replay or replace it.
- TCP accepts any caller-supplied host and port.
- Character-device creation accepts an absolute caller-supplied path, opens it as root, verifies it is a character device, and rejects other file types.
- Use a 30-second bound for TCP connection plus initial `INFO` verification. Publish no HTTP lease until verification succeeds.
- Apply no timeout to normal commands.
- If an HTTP caller disconnects after dispatch, finish reading the module response to preserve framing and discard the result.
- Implement force-close so session deletion or daemon shutdown interrupts active TCP and chardev operations. Open chardevs nonblocking and use polling plus an explicit cancellation wakeup to guarantee blocked operations can be interrupted.

### HTTPS API and session manager

Define the public API in `user/api/openapi.yaml` and generate committed strict server types and a Go client with a pinned `oapi-codegen` tool dependency.

Core endpoints:

- `GET /health/live`: unauthenticated daemon liveness only.
- `GET /v1/status`: authenticated daemon information.
- `POST /v1/sessions`: create a lease from either `{type:"tcp",host,port}` or `{type:"chardev",path}`.
- `GET /v1/sessions`: list all leases.
- `GET /v1/sessions/{id}`: inspect state, destination, cached INFO, and expiry.
- `POST /v1/sessions/{id}/renew`: refresh an idle lease.
- `DELETE /v1/sessions/{id}`: idempotently force-close and remove a lease, including while busy.
- `POST /v1/sessions/{id}/commands/info`
- `POST /v1/sessions/{id}/commands/echo`, with binary data represented as base64.
- `POST /v1/sessions/{id}/commands/pslist`, returning semantic process/thread JSON.

Session behavior:

- Generate opaque random 128-bit lease IDs.
- Use a 15-minute idle lease; commands and renewals refresh expiry.
- The TUI renews its lease every five minutes while idle.
- Busy sessions do not expire automatically because normal commands have no timeout.
- A concurrent command on the same lease returns HTTP `409 session_busy`.
- Different leases execute concurrently, including against the same module.
- Keep all lease state in memory. Daemon restart or module disconnect invalidates leases; clients must create replacements.
- Apply no daemon-wide or per-destination session cap. The destination kernel and operating system are the only capacity authorities.
- All valid bearer identities have equal rights to create, inspect, execute on, renew, and delete every lease.
- Do not maintain a target registry. Session creation supplies the connection details directly.
- Return stable JSON error codes and appropriate HTTP statuses for authentication, validation, busy sessions, handshake timeout, capacity rejection, broken sessions, transport failures, and protocol failures.
- Add request IDs and structured audit logs containing identity, destination, lease, command, duration, and outcome; never log bearer tokens or command payloads.

### Daemon, authentication, and TUI

- Build separate `mappled` and `mapple` binaries with `CGO_ENABLED=0`.
- `mappled` runs as root under systemd, terminates HTTPS itself, and loads `/etc/mapple/mappled.json`.
- Configuration contains listen address, TLS certificate/key paths, 30-second handshake timeout, 15-minute lease timeout, and bearer identities.
- Provision each identity with a random 256-bit bearer token. Store only its identity name and SHA-256 digest in the root-readable configuration and compare digests in constant time.
- Token/configuration changes require daemon restart in v1.
- Require bearer authentication on every `/v1` endpoint; disable CORS, accept only documented methods/content types, limit HTTP body/header sizes, and configure header/read/idle timeouts. Leave HTTP response write timeout disabled because module commands may run indefinitely.
- On SIGTERM, stop accepting requests, force-close all module sessions, then exit. Run with systemd restart and feasible root-service hardening such as `NoNewPrivileges`, private temporary storage, and filesystem restrictions.
- Treat bearer-token holders as fully trusted operators: they can direct a root process to any TCP host/port or character-device path and can control every lease.
- The private management network must prevent ordinary clients from reaching module TCP port 4444 directly. Modules may allow multiple designated server machines.
- `mapple` uses only the generated HTTPS client. It supports named profiles containing server URL, bearer token, and CA configuration in a mode-`0600` user config file, with one active server per TUI instance.
- The initial TUI exposes server/session status, list/create/renew/delete, INFO, echo, and pslist with filtering/sorting and clear ready/busy/broken state presentation.

## Migration and Build Changes

- Implement wire v3, INFO, and structured kernel responses together with the Go control plane; do not maintain protocol v2 compatibility.
- Replace the contents of the current C `user/` program immediately rather than retaining a parity binary.
- Remove the C user sources, C Makefile rules, compile database integration, remote `gdbserver` deployment, and C-specific VS Code launch configuration.
- Preserve kernel build/deploy/debug behavior, removing only its dependency on `user/compile_commands.json`.
- Make the root build invoke pinned OpenAPI generation checks, `go test`, and production builds for both binaries.
- Commit generated OpenAPI code and provide a verification target that regenerates it and fails on differences.
- Add installation documentation and assets for:
  - `/usr/local/bin/mappled`
  - `/usr/local/bin/mapple`
  - `/etc/mapple/mappled.json`
  - TLS certificate/key provisioning
  - Bearer-token generation and hashing
  - The root systemd service
  - Private-network firewall rules
  - Nonprivileged TUI profile configuration

## Test and Acceptance Plan

- Wire unit tests cover exact v3 header bytes, partial reads/writes, byte order, INFO, statuses, sized/streamed echo, pslist records, malformed data, and unknown extensions.
- Transport tests verify TCP and nonblocking chardev behavior, especially force-close interrupting blocked reads and writes.
- Session tests verify one in-flight command, `session_busy`, recoverable remote statuses, fatal framing transitions, no replay, and cleanup.
- HTTP tests validate the OpenAPI contract, bearer authentication, JSON/content limits, error mappings, session CRUD, lease renewal/expiry, and forced deletion while busy.
- Run Go race tests against concurrent operations on different leases and conflicting operations on one lease.
- TUI model tests cover profile selection, lease heartbeat, session-state transitions, command results, and server failures without importing the private module client.
- Real-kernel acceptance tests use HTTPS exclusively and cover:
  - One daemon controlling multiple modules
  - Multiple daemons controlling one module
  - Multiple concurrent leases to one module
  - Sequential commands reusing one lease
  - Ninth-session rejection when the kernel is full
  - TCP and local chardev session creation
  - INFO compatibility rejection
  - Daemon restart and module unload
  - HTTP disconnect followed by successful session reuse
  - Forced deletion of a blocked session
- Completion requires both binaries to build without runtime Go dependencies, all public integration tests to use HTTP, and no non-server package or supported executable to access the module wire protocol directly.

## Explicit Assumptions

- Root execution, arbitrary TCP destinations, arbitrary character-device paths, equal-power bearer tokens, no daemon session cap, and no normal-command timeout are intentional requirements despite their security and resource-exhaustion risk.
- HTTPS runs on a private management network with a server certificate trusted by TUI and automation clients.
- Servers do not coordinate capacity; the kernel is authoritative and races between servers are expected.
- The public compatibility boundary is HTTP/OpenAPI v1. The kernel wire protocol is private to `mappled` and may evolve with coordinated kernel/server releases.

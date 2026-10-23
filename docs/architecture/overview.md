# Architecture overview

[Documentation](../README.md) / Architecture

Mapple has three components: a C kernel module, an HTTPS server, and a terminal
client. In the development lab, the module runs in an Ubuntu QEMU guest while
both Go programs run on the host. See [setup](../guides/setup.md) and [usage](../guides/usage.md)
for the runnable workflow.

## Components and transports

| Component | Source | Responsibility |
| --- | --- | --- |
| `mapple.ko` | `kernel/src/` | Kernel sessions, command dispatch, process/thread snapshots, TCP and character-device transports |
| `mappled` | `user/server/`, `user/cmd/mappled/` | HTTPS API, bearer authentication, session leases, private module client |
| `mapple` | `user/tui/`, `user/cmd/mapple/` | Nonprivileged terminal UI using the generated HTTPS client |
| Shared wire definitions | `shared/` | Kernel command IDs, framing, statuses, and data structures |
| API contract | `user/api/openapi.yaml` | Public HTTP endpoints and generated Go bindings |

A server connects to a module over TCP (guest port 4444) or a local `/dev/mapple`
character device. The module-client implementation is below
`user/server/internal/moduleclient/`, so the TUI cannot import it. The TUI
communicates exclusively over HTTPS.

Multiple servers can connect to one module, and one server can connect to many
modules. The kernel enforces a maximum of eight sessions across its transports
and connected servers. Each HTTP lease owns one kernel session.

## Private module protocol

Wire protocol v3 uses a packed, big-endian 24-byte header:

| Field | Bytes |
| --- | --- |
| Magic | 4 |
| Version | 2 |
| Command | 2 |
| Flags | 4 |
| Status | 4 |
| Payload size | 8 |

Requests set status to zero. Successful responses use zero; recoverable command
errors return a nonzero status. Payloads support sized and streaming modes.
Framing violations, partial responses, and transport failures break the session;
the server does not silently reconnect or replay a command.

Typed operations are `Info`, `Echo`, and `ProcessSnapshot`. An initial INFO
exchange verifies a new connection before an HTTP lease is published. TCP
connection and this handshake have a 30-second default bound. Normal commands
have no timeout.

## HTTPS API and leases

`user/api/openapi.yaml` defines the public contract. Bindings are committed;
`make api` regenerates them and `make api-verify` checks for drift.

| Endpoint | Purpose |
| --- | --- |
| `GET /health/live` | Unauthenticated daemon liveness |
| `GET /v1/status` | Authenticated daemon status |
| `POST /v1/sessions` | Create a lease for a supplied TCP destination or character-device path |
| `GET /v1/sessions` | List leases |
| `GET /v1/sessions/{id}` | Inspect destination, state, INFO, and expiry |
| `POST /v1/sessions/{id}/renew` | Renew an idle lease |
| `DELETE /v1/sessions/{id}` | Force-close and remove a lease, including while busy |
| `POST /v1/sessions/{id}/commands/info` | Query module/session capabilities |
| `POST /v1/sessions/{id}/commands/echo` | Round-trip bytes, represented as base64 in JSON |
| `POST /v1/sessions/{id}/commands/pslist` | Return a semantic process/thread snapshot |

The server keeps leases in memory with random 128-bit IDs and a default
15-minute idle lifetime. The TUI renews an idle lease every five minutes. Busy
leases do not expire during a command. One command may run per lease; concurrent
use of the same lease returns `409 session_busy`. Different leases can execute
concurrently.

A caller disconnecting after command dispatch does not abandon the module
response: the server finishes reading it to preserve framing. Deleting a lease
or shutting down the server interrupts its active transport operations. A daemon
restart invalidates its leases, so clients create new ones.

Lab slots are development-tooling state. They populate TUI module profiles;
the HTTPS API itself takes a destination at session creation and does not own
a target registry.

## Authentication and deployment

`mappled` terminates TLS. Every `/v1` endpoint requires a bearer token; the server
stores identity names and SHA-256 token digests. All authenticated identities
can inspect, execute on, renew, and delete every lease. Configuration and token
changes take effect after restarting the server.

The local TCP lab listens on host loopback and uses generated TLS/authentication
settings. The installed service runs as root to support arbitrary local
character-device paths. Operators and tokens are trusted; deployment isolates
the module's TCP port from ordinary clients and exposes the HTTPS API on a
private management network. See [server installation](../guides/server-installation.md).

## Detailed contracts

- [Kernel support tiers (proposed model)](kernel-support-tiers.md)
- [Session lifecycle](sessions.md)
- [Process snapshot model](process-snapshots.md)
- [Process snapshot JSON schema](../reference/process-snapshot-json.md)

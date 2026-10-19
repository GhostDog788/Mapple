# Process snapshot JSON (schema v1)

[Documentation](../README.md) / Reference

`mappled` returns one JSON snapshot from
`POST /v1/sessions/{id}/commands/pslist`. The TUI's `command pslist` displays this
response through the generated HTTPS client. A connected session is required;
see the [usage guide](../guides/usage.md#daily-development-loop).

The current representation uses `schema_version: 1`, independently of the
module's private wire-protocol version and snapshot record framing. The
[OpenAPI contract](../../user/api/openapi.yaml) defines the endpoint, and
[`user/server/pslistview.go`](../../user/server/pslistview.go) defines the detailed
JSON field layout.

## Top-level layout

```json
{
  "schema_version": 1,
  "snapshot": {},
  "tasks": {},
  "objects": {}
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | number | Always `1` for this document shape. |
| `snapshot` | object | Request status, timing, and requested/effective group names. |
| `tasks` | object | Arrays of `processes`, `unmatched_threads`, and `kthreads`, grouped for presentation. |
| `objects` | object | Selected sibling group dictionaries and shared catalogs. |

Every task contains numeric `pid`, `tgid`, and `ppid`, a decimal-string
`start_boottime_ns`, a normalized state name, raw-byte `comm`, and a `groups`
object. A value in `groups` is either a numeric request-local reference into the
corresponding dictionary below `objects` or `null` when that group does not apply
to the task (for example, address-space groups on a kernel thread). References
are meaningful only inside their named group and the current response.

The optional object sections are `identity`, `environment`, `security`,
`context`, `resources`, `fds`, `network`, `memory_maps`, `mapped_files`, and
`elf`. `fds-details` enriches entries inside `fds`; it is not a separate section.
Similarly, `elf` implies memory maps and uses the shared `mapped_files` catalog.
The precise collection contents and dependency rules are described in
[process snapshot architecture](../architecture/process-snapshots.md).

## JSON scalar rules

Schema v1 assigns one JSON type to each field. A value never changes between a
JSON number and string based on its runtime magnitude.

- PIDs, file-descriptor numbers, enum values, counts, lengths, Linux IDs whose
  wire type is at most 32 bits, and request-local object references are JSON
  numbers.
- Semantic signed or unsigned 64-bit quantities are quoted decimal strings.
  This includes boot-relative times, inode numbers, file sizes, cgroup IDs,
  CPU and I/O counters, page counts, file offsets, mount IDs, and ELF symbol sizes.
  Consumers can parse these decimal strings into an integer type without losing
  precision in JSON implementations backed by IEEE-754 doubles.
- Wall-clock timestamps are UTC ISO-8601 strings, including timestamp fields
  whose names end in `_ns`.
- Addresses and 64-bit bitmasks are fixed-width or natural-width hexadecimal
  strings prefixed with `0x`. Examples include VMA addresses, capabilities,
  file attributes, ELF values, and filesystem magic values.
- Boolean facts are JSON booleans. Unavailable or inapplicable optional values
  are JSON `null`; their surrounding object carries status and error context.

## Raw kernel byte strings

Kernel-provided byte sequences such as `comm`, argv entries, environment
entries, paths, LSM labels, and ELF strings are not assumed to be UTF-8.
The server maps each input byte directly to the Unicode code point with the same
numeric value from U+0000 through U+00FF and applies normal JSON escaping. This
mapping is reversible: a consumer recovers the original bytes by rejecting code
points above U+00FF and converting every remaining code point to one byte.

This is deliberately not UTF-8 decoding. Display software may choose a separate
human-friendly rendering, but must retain the raw schema value when exact bytes
matter.

## Status and partial data

`snapshot.status` is `complete` or `partial`. Individual group objects can be
`complete`, `partial`, `unavailable`, `raced`, or `too_large`; optional fields
can carry more specific collection statuses. A valid partial snapshot remains a
successful JSON response. Consumers must inspect statuses instead of treating a
missing value as proof that the underlying kernel value does not exist.

The collector does not freeze the system. Tasks and subordinate objects may
change during the one-pass traversal. Detected changes are represented through
status fields. In particular, Linux 6.6 does not export `files_struct`
reference-management helpers to loadable modules, so FD-table sharing and race
reporting are best-effort as documented in the architecture note.

## Bounds and resource model

There is no request-wide byte or object budget in schema v1. Collection is
recomputed from scratch for every request, retains no history, and uses the
per-field and per-object limits defined by the wire ABI. A limit failure is
reported in the affected object's status rather than silently truncating its
meaning. The TUI presents the server's decoded JSON response.

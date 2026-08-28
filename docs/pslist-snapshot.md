# PSLIST snapshot architecture

PSLIST is an observational, request-scoped snapshot. Every request starts with an
empty collection context, walks the kernel task list once, writes the response,
and releases every reference and allocation. The module does not install hooks,
keep accounting data, or retain object IDs between requests.

## Collection groups

Core task data is always returned. The request's 64-bit group mask selects
optional collectors:

| Bit | Name | Contents |
| ---: | --- | --- |
| 0 | `identity` | argv and executable identity |
| 1 | `environment` | environment entries |
| 2 | `security` | credentials, capabilities, seccomp and LSM context |
| 3 | `context` | CWD, root, namespaces and cgroups |
| 4 | `resources` | CPU, memory and I/O counters |
| 5 | `fds` | generic FD inventory |
| 6 | `fd-details` | generic file metadata and raw type-specific fdinfo |
| 7 | `network` | socket-specific state and counters |
| 8 | `memory-maps` | VMA and mapped-file metadata |
| 9 | `elf` | static metadata for mapped ELF files |

The current milestone implements `identity`, `fds`, `fd-details`, and `network`.
Requests for reserved but unimplemented groups fail with `EOPNOTSUPP` before the
task walk. `fd-details` and `network` imply `fds`; the streamed response reports
both the requested and effective masks.

## One-pass object model

The outer traversal is one forward pass over task IDs. A task is never revisited.
The runtime is therefore linear in visited tasks plus the selected subordinate
records, such as FDs, VMAs, and ELF symbols.

Linux tasks frequently share objects such as `mm_struct` and `files_struct`.
A request-local hash table maps the first observed object to an opaque 64-bit ID.
The object is captured once; later tasks emit only links. IDs are meaningful only
inside that response and must never be compared across snapshots.

The FD table is snapshotted once on first encounter. One pass over its pinned file
references conditionally produces the generic FD, FD-details, and network records.
Socket-specific fields belong only to the network group. If a task replaces its
FD table between discovery and capture, that object is marked raced rather than
capturing the replacement under the original object ID.

## Stream framing and races

The response is a flat sequence of size-delimited records: snapshot begin, task,
task/object link, object begin, group records, object end, and snapshot end.
Unknown future record types can be skipped using their size. Object-end records
carry completion status and counts, so a raced or inaccessible object does not
make absence ambiguous.

The snapshot does not freeze the system. Tasks and descriptors can appear, exit,
or change while collection runs. The collector takes short-lived references and
locks, reports detected partial/raced data, and never holds the task-list lock
across data collection or a blocking stream write.

## Non-mutation and retained state

Collectors use read-only kernel interfaces and do not advance target file offsets
or intentionally change process, file, socket, or namespace state. Observation can
still cause incidental kernel activity such as allocation, cache fills, page faults,
or lock contention. All module-owned state is released at the end of the request.

CPU, I/O, and TCP counters added by later collectors are existing kernel-maintained
counters. Rates and deltas are calculated by remote userspace from two independent
snapshots; the module never maintains history.

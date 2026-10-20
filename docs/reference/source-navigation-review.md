# Source navigation review

[Documentation](../README.md) / Reference

Recorded on **20 October 2026**, against commit
`a2a78e708bc2a3392a9bb5ab269b52bc57358c30`.

This review measures Ctrl+click / Go to Definition between Mapple's C code and
the matching Linux source. It records the findings so future development can
build on this investigation. It does not assess GDB or runtime source stepping.
The investigation changed no tracked code or editor configuration. The
recommendations below have **not** been implemented by this review.

**Implementation follow-up:** the development tooling now provides per-kernel
navigation preparation and persistent caches, an editor integration that replaces
clangd on kernel changes, inactive-kernel document filtering, and live readiness
checks. See the [source navigation workflow](../guides/usage.md#source-navigation)
and `make test-navigation`. The measurements and reproduction below remain the
historical baseline; they are not timings for the subsequent implementation.

The follow-up was verified with `make check` (51 tooling tests plus API, Go, and
Python checks), and `make test-navigation` in a disposable checkout with real
VS Code/clangd processes. The editor regression covers A → B → A, an open old-kernel
source tab, manual restart, a same-kernel machine change, missing inputs,
headers-only support, and reopening a generated debug workspace after clean.
An additional smoke test on the full cached baseline Linux tree reached
`kernel_accept`, `mapple_session_manager_register`, and `new_inode_pseudo` through
the new editor integration. The baseline index was prepared offline. These were
static navigation tests; no live kernel-debugging session was started for this
follow-up.

## Conclusion

**Keep clangd and a build-derived `compile_commands.json`.** The tested kernel
and Mapple sources work together through one language server. Once ready, VS
Code resolves definitions in about a millisecond, and navigation to a different
file typically completes in 40–62 milliseconds in the final idle-host test.

The main gaps are preparation and correctness when changing kernels:

1. A new Linux index takes minutes to prepare. Before the relevant source is
   indexed, a fast response can lead to a header declaration instead of the
   implementation. Fast response time alone is not a successful test.
2. Updating the compilation database does not reliably remove the previous
   kernel's definitions from a running clangd. An isolated reproduction returned
   the old definition after switching kernels with both clangd 18 and 23.
   Restarting clangd resolved it.
3. The checkout had no active machine or root compilation database when audited.
   Measurements used the cached `baseline` kernel's database explicitly, without
   selecting a machine. These are not measurements of an already configured
   active-machine session in the main workspace.

No measured result justifies changing the navigation engine. Improving index
readiness and refreshing the language server when the kernel changes would
address the demonstrated problems.

## How navigation is configured

- [VS Code settings](../../.vscode/settings.json) select `/usr/bin/clangd-18`,
  enable background indexing, and point it at the repository-root compilation
  database. Microsoft's IntelliSense engine is disabled, leaving clangd as the
  C/C++ navigation provider. The C/C++ extension remains available for GDB
  debugging.
- [The module Makefile](../../kernel/Makefile) and
  [database generator](../../scripts/compile_commands.py) collect Kbuild compiler
  commands, remap Mapple's intermediate source paths to the editable source,
  remove selected GCC flags unsupported by clangd, and combine the module and
  Linux entries. When Linux command records are unavailable, the generator can
  derive navigation commands from the module's build context.
- [Machine selection](../../scripts/mapple_dev/machines.py) publishes the selected
  machine's kernel database through
  [the navigation publication functions](../../scripts/mapple_dev/runs.py).
  It copies the database to the root; it does not restart an existing language
  server. If that kernel has no generated database, it removes the root database.

This use of compiler commands follows
[clangd's documented approach](https://clangd.llvm.org/design/compile-commands).
The database provides the include paths, preprocessor definitions, architecture
flags, language mode, and working directory needed to interpret the matching
kernel configuration. A directory of source files alone does not provide this
context.

Clangd's [background index](https://clangd.llvm.org/design/indexing) supplies
definitions from files that have not been opened. Its persistent shards normally
live in `.cache/clangd/index/` beside the compilation database. Opening a file
also builds its live syntax tree; a saved index does not eliminate that parsing
cost.

## Test environment and boundaries

| Item | Tested value |
| --- | --- |
| Host | Ubuntu 24.04.5 LTS, x86-64 |
| CPU and memory | Intel Core i7-8700K, 6 cores / 12 logical CPUs, about 16 GB RAM |
| CPU affinity | Logical CPUs `0,2-6,8-11`; the same precaution used after earlier host-tool crashes on physical core 1 |
| VS Code | 1.137.0, commit `645f29cc3176500b4b5762ba887cf2a7f0ffdf2c`, x64 |
| Navigation extension | `llvm-vs-code-extensions.vscode-clangd` 0.6.0 |
| Installed engine | Ubuntu clangd 18.1.3 |
| Alternative engine | Official stable clangd 23.1.0; downloaded assets verified against their published SHA-256 digests |
| Microsoft comparison | `ms-vscode.cpptools` 1.34.4 |
| Managed kernel slot | `baseline` |
| Linux release | `6.6.6-mapple-baseline-b8325559` |
| Target architecture / compiler | x86-64 / GCC 12 |
| Database | 2,441 translation units: 2,389 C and 52 assembly; 24 entries were Mapple source files |

The database audit found no missing source files. All entries used the same
prepared Linux build root. Mapple entries retained `__KERNEL__`, `MODULE`, and
the target's include paths and configuration. A separate `clangd --check` on
Mapple's TCP implementation completed with **zero errors**. That check is not
a claim that every translation unit has zero diagnostics.

Tests used isolated VS Code profiles and temporary workspaces, with the actual
source files and recorded compiler flags. VS Code ran under Xvfb with GPU
acceleration disabled. Only the engine under test provided C/C++ navigation.
The final navigation-command measurements ran after the other intensive audit
workloads had stopped.

“Cold index” means that no clangd index shards existed for that test database.
Filesystem caches were **not** flushed. These are host-specific measurements,
not a guarantee for a fresh machine or a different kernel configuration.

## What was measured

Three different costs were measured separately:

1. **Definition lookup:** a `textDocument/definition` request directly to clangd,
   and `vscode.executeDefinitionProvider` through the actual VS Code extension.
2. **Editor navigation:** `editor.action.revealDefinition`, starting with the
   source editor and cursor selected, then checking the destination editor.
3. **Preparation:** starting a new language-server process, restoring a saved
   index, and building an index without existing shards.

The destination implementation was not opened to make an initial lookup pass.
Ten direct LSP cases checked the expected destination files, with 30 repeated
queries each after indexing. Nine VS Code cases used 30 repeated provider
queries and 15 navigation commands each. All expected destinations passed with
clangd once the index was ready.

Editor timings measure command completion. They are not physical mouse-click
to final painted-frame measurements, and the headless display can differ from
an ordinary desktop session.

## Warm VS Code results

Final clangd 18 results with the host otherwise idle. Times are milliseconds.
“Navigation” includes moving to the destination editor. P95 is the observed
95th percentile of that case's sample set, not a latency guarantee.

| Origin → requested definition | Expected destination | Lookup median | Navigation median | Navigation P95 |
| --- | --- | ---: | ---: | ---: |
| TCP → `kernel_accept` | Linux `net/socket.c` | 0.89 | 61.9 | 75.7 |
| TCP → `mapple_session_manager_register` | Mapple `control/sessions/session_manager.c` | 0.96 | 50.9 | 61.9 |
| TCP → local `mapple_tcp_accept_session` | Same Mapple `tcp.c` | 0.84 | 2.2 | 8.4 |
| TCP → `struct socket` | Linux `include/linux/net.h` | 0.68 | 51.9 | 302.5 |
| TCP → `MAPPLE_TCP_PORT` | `shared/shared.h` | 0.74 | 45.3 | 56.5 |
| Module entry point → `mapple_tcp_init` | Mapple `control/transports/tcp_server/tcp.c` | 1.01 | 40.5 | 55.0 |
| Memory-map code → `fput` | Linux `fs/file_table.c` | 0.86 | 51.3 | 57.6 |
| Linux socket code → `new_inode_pseudo` | Linux `fs/inode.c` | 1.07 | 60.3 | 121.3 |
| Linux `net.h` → `kernel_accept` implementation | Linux `net/socket.c` | 1.13 | 47.2 | 51.5 |

Direct LSP warm medians were about **0.1–0.6 ms** across the completed indexed
runs. The extra editor and extension work accounts for part of the difference
between these timings and a visible navigation command.

The outliers above matter: warm navigation is usually quick, but the measurements
do not support a promise of zero wait or a strict maximum below 100 ms.

## First use and index preparation

| Scenario | Observation |
| --- | --- |
| New full Linux index, clangd 18, four workers | 187.0 seconds |
| New full index, configured default worker count | 202.3 seconds; overlapped the Microsoft comparison, so not a controlled worker-count comparison |
| Restore existing index, clangd 18 | 2.26 seconds |
| First correct Mapple → Linux implementation result in final VS Code run after restart | 3.08 seconds |
| First lookup from newly opened module entry-point file, index ready | 273 ms |
| First lookup from newly opened memory-map implementation, index ready | 400 ms |
| First lookup from newly opened Linux socket implementation, index ready | 833 ms |
| First lookup from newly opened Linux networking header, index ready | 317 ms |

The startup and file-open costs are separate from repeated warm lookup speed.
Reading a file before clicking often gives clangd time to parse it, but an
immediate request after opening it can include the parsing cost.

Without background indexing, several requests returned header declarations
instead of the requested C implementations, including
`mapple_session_manager_register`, `kernel_accept`, `sock_release`, `fput`, and
`new_inode_pseudo`. Some of those responses arrived in under a millisecond.
That is why both destination correctness and elapsed time were checked.

Even with cached shards, an immediate request during startup could briefly
return the declaration before the index had finished loading.

## Engine and option comparisons

### Clangd 18 versus 23

Both versions resolved all tested definition destinations once indexed. In the
initial VS Code provider-only runs, the nine cases' warm medians ranged from
**0.71–1.33 ms** with clangd 18 and **0.77–1.62 ms** with clangd 23. This did not
demonstrate a meaningful speed advantage for upgrading.

Clangd 23's new four-worker index took 201.0 seconds, but that run overlapped
the Microsoft comparison. It should not be used to rank cold indexing speed
against the otherwise uncontended 187.0-second clangd 18 run. Both versions
also reproduced the kernel-switching issue below.

### Memory-backed preambles

A clangd 18 trial with `--pch-storage=memory` restored its index in 2.76 seconds,
versus 2.26 seconds in the disk-backed trial. Warm lookups remained below a
millisecond directly through LSP. These single runs did not establish a useful
advantage from changing the default preamble storage. They also do not prove
that memory storage is generally slower.

### Microsoft's navigation engine

The isolated C/C++ extension trial used the same recorded commands, explicit
browse paths, and actual source files. Its first `kernel_accept` response took
41.9 seconds and returned `include/linux/net.h`. It did not reach the requested
`net/socket.c` implementation within the 180-second test window. Repeated
responses were faster but still returned the declaration.

This was a bounded trial, not an exhaustive tuning exercise or a general verdict
on Microsoft's engine. It provided no evidence that switching from the working
clangd setup would improve this repository's experience. Its GDB debugging
integration is a separate capability and was not being compared.

## Kernel-switching correctness issue

An isolated fixture exercised the repository's actual machine-selection and
database-publication functions. It did **not** deploy two full Linux kernels or
change the main checkout's active machine.

The fixture contained a module file calling `kernel_api()` and two small kernel
source trees, A and B, each defining that same C function in a different file.
Each machine's database supplied its own header path and kernel implementation.

The sequence was:

1. Select machine A and start clangd. Verify that the definition points to A.
2. Select machine B. The repository replaces the root compilation database.
3. Notify clangd of the database change and send document updates, then query
   the definition repeatedly.
4. Check the server log: it rereads the database, uses B's compile command, and
   indexes B's implementation.
5. Observe that the definition still points to A through the final request,
   about 16 seconds after the switch.
6. Restart clangd against B's current database. The definition now points to B.

This occurred with both clangd 18.1.3 and 23.1.0. The small fixture resolved B
about 15 ms and 11 ms after the respective restarted servers' document-open
step; those times must not be confused with restoring a full Linux index.

**Publishing the right compiler flags is necessary but does not by itself prove
that a running language server will navigate exclusively into the right kernel.**
The old in-memory definitions can survive the database change in this
reproduction. It is a regression case to address, rather than a claim that every
real kernel switch always fails.

The current manual workaround is to select/build for the intended machine and
then run **clangd: Restart language server** from VS Code's Command Palette.
Allow the selected kernel's index to become ready before relying on jumps into
unopened implementations. See the [usage guide](../guides/usage.md) for the
machine-selection and module-build commands.

## Coverage and limits

- All 125 undefined symbols imported by the built Mapple module had exact-name
  locations in the completed clangd index. Of those, 117 had a C-file location;
  the remaining entries included header-defined code, tracepoint declarations,
  and architecture thunks. Finding a symbol in the index is **not** proof of
  125 correct Ctrl+click implementation destinations.
- The ten direct LSP cases and nine VS Code cases verified their expected
  destination files. The previous fresh-clone navigation test opened the Linux
  destination before querying the module call; that earlier test alone could
  not establish navigation into unopened implementations.
- All 52 assembly translation units logged compilation failures during clangd's
  background pass. Partial symbol information can still exist, but these C/C++
  navigation results do not establish complete assembly navigation support.
- Inactive preprocessor branches and code excluded by the chosen Linux config
  are outside the guarantee of the matching build's index.
- The measured database came from a built kernel with real Kbuild command
  records. Imported source-only trees using synthesized commands, other
  architectures, and custom SDK compiler discovery were not performance tested.
- Source navigation needs local source and compilation context. It does not
  require a running VM, a GDB endpoint, `vmlinux`, or Linux DWARF debug data.
  This review does not change the separate
  [kernel debugging requirements](../architecture/kernel-support-tiers.md).

## Recommended follow-up

These are proposed development-tooling improvements, not features added here:

1. **Make kernel selection refresh the language-server session.** Test that
   switching A → B → A returns only the selected kernel's definitions. Restart
   or replace the server as needed; changing the database alone is insufficient
   in the reproduction above.
2. **Prepare indexes explicitly and report readiness.** Preserve useful caches
   per exact kernel and distinguish “database generated,” “index loading,” and
   “implementation navigation ready.” Move initial indexing into preparation
   when practical, and avoid unnecessary full reindexing during the edit loop.
3. **Diagnose absent or incomplete navigation inputs.** Report no active machine,
   no published database, missing source, and synthesized rather than recorded
   compiler commands clearly.
4. **Test the actual destinations across lifecycle changes.** Keep unopened
   implementation jumps, editor restart, and kernel switching as regression
   cases. Include first-use costs and warm latency; do not accept a fast jump
   to a declaration as an implementation-navigation pass.

There is no need to repeat the entire engine survey for routine changes. Rerun
the relevant regression cases when implementing these recommendations, changing
the navigation engine or database generator, or adding an untested kernel/SDK
configuration.

## Evidence and reproduction notes

The recorded evidence includes direct JSON-RPC LSP clients, isolated VS Code
extension-host tests, expected destination checks, server logs, and the
machine-selection fixture. Local raw results and harnesses were retained under
`.lab/navigation-review/`, including `results.json`. That directory is gitignored
and is **not** required to read this report in a fresh clone.

To reproduce a targeted check later:

1. Select the intended machine and build Mapple using the normal workflow, or
   explicitly point the test server at a named kernel's generated database.
2. Record the kernel release, configuration/build identity, engine and extension
   versions, worker count, database size, and cache state.
3. Start a new clangd process with the repository's configured arguments. Use
   a separate database directory with no `.cache/clangd/index/` for a cold-index
   measurement, leaving the developer's normal cache intact.
4. Request `textDocument/definition` from the caller without opening the
   destination. For VS Code, use `vscode.executeDefinitionProvider` and
   `editor.action.revealDefinition`; verify the returned/opened file, not just
   whether the command succeeded.
5. Observe background-index progress through LSP work-done notifications, then
   take repeated warm samples. Restart with the same cache to measure recovery.
6. For kernel selection, keep the same language-server process alive while
   publishing the second database. Compare both the returned locations and
   compile-command/indexing logs, then repeat after restarting the server.

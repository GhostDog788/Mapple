# Kernel support tiers and development requirements

[Documentation](../README.md) / Architecture

This document explains exactly what is required to **build and run Mapple**,
**debug Mapple**, and **debug the Linux kernel at different levels**. It defines
four proposed kernel support tiers and the added artifacts that unlock each one.
The lower debugger modes and tier reporting are a design, not implemented Make
options; [current implementation](#current-implementation) describes that boundary.

We always have Mapple's repository source, compile the module for the target,
and upload it. A prebuilt-module-only workflow is outside this model. The target
can be a Linux VM or physical device, including a router. The development host
and target can be different machines and architectures.

**The tier and the operation checks answer different questions:**

1. **Which kernel artifacts do we have?** This establishes the kernel tier.
2. **Can this host build Mapple, and can this target load and run it?** Toolchain,
   module API compatibility, target services, permissions, and signing policy
   are separate checks. They do not change which kernel artifacts we have.
3. **Can a debugger actually control the target?** Live debugging also needs
   the matching Mapple debug file, runtime addresses, and source mappings.

A machine can support building and running Mapple without supporting any live
debugging. Having all Linux source and symbols does not change that if debugger
access is unavailable.

[Operation requirements](#requirements-by-operation) ·
[Four tiers](#the-four-kernel-tiers) ·
[Artifact glossary](#what-each-file-actually-provides) ·
[Examples](#worked-examples) ·
[Current implementation](#current-implementation)

## Requirements by operation

### 1. Compile Mapple

Here, **compile** means producing the module through Kbuild, including its
post-processing and linking steps. Compiling one C file into an object is not
proof that we can produce a usable module for the target.

On the **development host**, we need:

- **Mapple's source and Kbuild files**, which come from this repository.
- **Matching prepared kernel development files** for the target build:
  - Kernel headers, including architecture-specific and generated headers.
  - Configuration outputs, such as `include/config/auto.conf` and
    `include/generated/autoconf.h`. A `.config` file alone is not a prepared build.
  - Kbuild makefiles, scripts, and required generated build tools.
  - Kernel export metadata, normally `Module.symvers`. If symbol versioning is
    enabled, its checksums must match the target kernel's exported interfaces.
- **A compatible architecture toolchain**, including the compiler, binary tools,
  and any compiler features/plugins required by that kernel build. An ARM target,
  for example, needs an appropriate ARM toolchain; the host can still be x86-64.
- **Mapple code compatible with that kernel's APIs.** Headers cannot repair a
  source-level API incompatibility by themselves.

A distribution's matching kernel development package or a correctly prepared
kernel build tree can provide the development files. Ordinary userspace headers
under `/usr/include/linux` are insufficient. `modules_prepare` does not generate
`Module.symvers` for a kernel using symbol versioning.
[Linux module build requirements](https://docs.kernel.org/kbuild/modules.html)

**Not required for compilation:** a running target, a full Linux C source tree,
Linux debug symbols, a local kernel boot image, or a debugger connection.
Matching build inputs must be available, but they need not be installed on the
target. Compiling directly on the target still requires those inputs.

### 2. Upload, load, and run Mapple

In addition to compilation, the **target** needs:

- A running kernel compatible with the module's architecture, configuration,
  and kernel interfaces. A matching release string alone is not complete proof.
- Loadable-module support enabled and the kernel services Mapple uses available.
- A way to transfer the module and sufficient permission to load it, normally
  root or the appropriate kernel-module capability. SSH is the current lab's
  transfer/control channel.
- Acceptance under the target's module-loading policy. If signatures are
  enforced, the module must be signed with a key the target trusts. Signing
  establishes trust; it does not fix ABI incompatibility.
- For end-to-end application tests, the Go server and TUI/API client, plus a
  working module transport: reachable TCP or a local character device.

Linux does not promise a stable internal module ABI across arbitrary kernel
builds. [Kernel interface documentation](https://docs.kernel.org/process/stable-api-nonsense.html)
The kernel's policy determines when module signatures are mandatory.
[Module signing documentation](https://docs.kernel.org/admin-guide/module-signing.html)

**This is a complete build-and-run-only workflow.** It requires neither Linux
symbols/source nor a debugger. You can compile on the host, upload, load, and
exercise Mapple through the server and TUI without ever attaching GDB.

### 3. Source-debug Mapple itself

In addition to the build-and-run requirements, we need:

- **Mapple's matching debug-enabled ELF**, usually the unstripped `mapple.ko`
  retained on the host. Our [Kbuild](../../kernel/Kbuild) requests `-g`. Source files alone do not give
  GDB instruction-to-line or variable mappings; the ELF's debug information does.
- **The module's actual runtime section addresses**, so GDB can associate its
  local symbols with the code and data loaded on the target. Our lab obtains
  these through privileged access to `/sys/module/mapple/sections/`. They must
  be refreshed after a module reload; offsets in the local ELF alone do not
  establish where the target loaded it.
- **GDB supporting the target architecture**, and a working debugger connection
  that can stop execution and access registers and kernel memory. Examples are
  QEMU's GDB stub, configured KGDB, or suitable hardware debug access.
- **The matching Mapple source files and correct source-path mappings** on the
  host. VS Code uses a GDB adapter; GDB TUI uses the same underlying information.

GDB can load a dynamically loaded object's symbols using its runtime section
addresses. [GDB symbol-file documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Files.html)

**A Linux `vmlinux` is not fundamentally required to debug Mapple.** Mapple is a
separate compiled object and supplies its own symbols and DWARF. The current
launcher requires `vmlinux`, but that is a tooling restriction, not a GDB rule.

With this access, we can set breakpoints in Mapple source, step through our code,
and inspect recorded variables. Optimization can make some variables unavailable.
The debugger connection also permits reading kernel memory/registers and stepping
through Linux instructions by address, even without Linux symbols.

**SSH alone is not debugger access.** Neither is ordinary `gdb -p` attachment to
a userspace process. Without a suitable kernel/VM debugger connection, compiling,
uploading, and running remain possible, but these live debugging operations do
not. [QEMU debugger access](https://www.qemu.org/docs/master/system/gdb.html)

### 4. Debug Linux assembly by address or by function name

These are two different levels:

| Level | Addition to a working debugger connection | What becomes possible |
| --- | --- | --- |
| **Raw instructions** | No Linux symbol file; the debugger reads instruction bytes from target memory | Disassemble an address range, inspect registers, set address breakpoints, and single-step instructions |
| **Named instructions** | A matching Linux symbol table loaded with correct runtime addresses | Identify addresses as functions such as `kernel_sendmsg`, label assembly, and set breakpoints by kernel function name |

Instruction mnemonics such as `mov` and `call` are decoded from bytes. They do
not require Linux symbols. A function name such as `kernel_sendmsg` requires
usable symbolic information. [GDB machine-code documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Machine-Code.html)

The usual named-symbol artifact is `vmlinux` containing a symbol table. A matching
`System.map` or permitted `/proc/kallsyms` output may also provide useful names,
but requires an explicit import/conversion path; text maps are not drop-in ELF
symbol files. Any relocation, including KASLR, must be accounted for.

**A symbol table alone does not supply C source lines, local-variable locations,
or complete type descriptions.** Matching DWARF adds those recorded descriptions;
it can enable richer inspection even when the Linux source files are absent.

### 5. Source-debug Linux itself

In addition to the supported module build/run workflow and debugger access, we
need:

- **Linux symbols and matching DWARF debug information** for the running kernel.
  DWARF can be embedded in `vmlinux` or supplied in a matching separate debug file.
- **The matching Linux source files**, including applicable vendor patches.
- **Correct runtime-address and source-path mappings** in the debugger.

The symbol table names a function. DWARF connects its machine instructions to
C file/line locations and describes recorded types and variables. The source
files provide the actual C text to display. Together they enable stepping from
Mapple into Linux source. Debug information does not guarantee that every variable
survives compiler optimization.
[Linux kernel debugging documentation](https://www.kernel.org/doc/html/v6.2/dev-tools/gdb-kernel-debugging.html)

**Adding source alone is enough only if the matching kernel debug file already
contains the necessary DWARF.** If `vmlinux` contains names and addresses only,
we must add both the Linux source and the matching DWARF information.

### 6. Navigate Linux source in the editor

Ctrl+click/Go to Definition needs the matching Linux source, prepared headers,
configuration, and a suitable compilation database for clangd. It does **not**
require a running target, GDB connection, kernel symbol table, or DWARF.

This is source indexing, not live debugging. A target can therefore have source
navigation while remaining at a lower debugging tier. Our current full lab
creates navigation inputs during kernel setup and module builds.

## The four kernel tiers

The tiers classify **kernel artifacts** for our source-build workflow. Except
for Tier 0, each includes matching prepared module build inputs. The debugging
column assumes the complete [Mapple debugging requirements](#3-source-debug-mapple-itself),
including its ELF, runtime addresses, mappings, and debugger access.

A missing host compiler, incompatible Mapple API usage, or a target's refusal to
load modules must be reported as an operation blocker alongside the tier. For
example, the same Tier 3 kernel files remain Tier 3 on two machines even if only
one grants module-loading permission. An API-incompatible target can have Tier 3
artifacts while Mapple compilation is blocked. The tier alone never promises
that the complete operation succeeds.

| Tier | Boundary: what establishes it | Debugging level once all prerequisites are met |
| --- | --- | --- |
| **0 — No kernel build support** | Usable matching prepared kernel build inputs are unavailable | Cannot establish the Mapple source-build workflow; list the missing kernel inputs |
| **1 — Module development** | Matching prepared kernel development files | Mapple source debugging and Linux instruction debugging by address |
| **2 — Kernel symbol debugging** | Tier 1 plus usable matching Linux symbol information | Named Linux function breakpoints and named assembly debugging |
| **3 — Kernel source debugging** | Tier 2 plus matching Linux source and DWARF | Linux C source stepping and additional recorded kernel type/variable inspection |

### Tier 0 to Tier 1: add the module build environment

Tier 1 is established when matching prepared headers, generated configuration,
Kbuild files, and required export/version metadata are available for the target
kernel. A kernel image, release number, or debug `vmlinux` alone does not
establish this environment.

Tier 0 should list its missing kernel inputs, such as prepared headers or
required symbol-version metadata. Report missing host tools, incompatible
Mapple APIs, unsupported toolchain/debugger architectures, and inability to load
modules separately. Adding kernel artifacts does not repair these independent
operation blockers.

### Tier 1 to Tier 2: add Linux names and addresses

Add a matching symbol-bearing `vmlinux`, or equivalent symbol information that
our tooling can load into GDB. Linux source and DWARF are not required for this
transition. The debugger gains names for kernel addresses; the machine does not
need to reboot merely because we loaded local symbols.

### Tier 2 to Tier 3: add Linux source and line/type information

Add the matching Linux source tree and any DWARF not already present in the
kernel's debug file. If the Tier 2 `vmlinux` already contains DWARF, source is the
remaining addition. If it has only a symbol table, source alone does not unlock
Linux C stepping.

Individual flags should record useful combinations that do not form a perfect
ladder: source navigation without DWARF, for example, or DWARF-based type
inspection without source files.

## What each file actually provides

| Artifact | Purpose | Not a substitute for |
| --- | --- | --- |
| Prepared kernel headers/configuration/Kbuild files | Compile the module for the target's kernel interfaces | Linux debug symbols or full C source debugging |
| `Module.symvers` | Kernel exports and symbol-version checksums used by the module build | GDB address-to-name mappings or DWARF |
| Mapple's debug-enabled `mapple.ko` | Module code, symbols, C line mappings, and recorded variable/type information | Debug information for the separately compiled Linux kernel |
| Linux symbol table, commonly in `vmlinux` | Kernel function/data names and addresses | C text, C line mappings, or full local-variable/type information |
| Linux DWARF, embedded or separate | Recorded kernel source-line mappings, types, arguments, and variables | The actual Linux source text or a module build environment |
| Matching Linux source tree | C text for display and editor navigation | Generated build inputs, runtime symbols, or DWARF line mappings |
| `vmlinuz` / `bzImage` | A kernel boot image | A complete module development kit or debug-enabled `vmlinux` |
| clangd compilation database | Compiler context for source indexing | A live debugger connection or GDB debug information |

`vmlinux` is an ELF filename, not a promise about its contents. Check whether it
contains the required symbol table and DWARF. A file named `vmlinux` with the
wrong contents or from a different build does not unlock the corresponding tier.

## Worked examples

### A. Build and run, with no debugging

We have matching prepared kernel build files and a compiler. Mapple builds, the
target permits loading it, and its TCP or character-device transport works.
There is **no usable kernel debugger connection**.

**Result:** build, upload, run, and application tests work. Live Mapple debugging
and live Linux debugging do not. This remains true even if we later obtain all
Linux source and debug symbols but still cannot attach a debugger.

### B. Debug Mapple and raw Linux assembly

Start with A. Add access to the target through a suitable debugger, GDB for its
architecture, the retained debug-enabled Mapple ELF, its runtime section addresses,
and local Mapple source mapping.

**Result:** Tier 1 with live debugging. We can step through Mapple's C code and
into Linux machine instructions. General Linux function names are not established
by this artifact set; some names/types may already be known from our module's
own information.

### C. Add a Linux symbol table

Start with B. Load a matching symbol-bearing `vmlinux` and resolve its runtime
mapping. Assume it has a symbol table but no DWARF.

**Result:** Tier 2. GDB can identify kernel functions by name and label their
assembly. It cannot map Linux instructions to C lines using a plain symbol table.

### D. Add Linux DWARF, but still no source

Start with C. Obtain the matching DWARF debug information, embedded in `vmlinux`
or provided as a separate debug file, but do not add the Linux source tree.

**Result:** still Tier 2, with additional recorded type/variable inspection and
line metadata. The debugger does not have the Linux C text to display. Adding
the matching source files and source-path mappings would establish Tier 3.

### E. Add Linux source, but still no DWARF

Start with C. Obtain the matching Linux source tree and configure clangd indexing.

**Result:** still Tier 2, with source navigation. We can read and navigate the
Linux C files, but adding text did not establish instruction-to-source-line
mappings for live stepping.

### F. Add matching Linux DWARF

Start with E. Obtain a matching debug-enabled `vmlinux` or separate debug file
with the necessary DWARF, and configure source paths.

**Result:** Tier 3. Linux C source stepping and recorded type/variable inspection
are available. Adding source to example D would also establish this tier.

### G. We only have the running VM and its kernel image

No matching development files are available on either machine or obtainable
offline. The release string and boot image do not reconstruct a module build kit.

**Result:** Tier 0, with the missing build inputs listed. Adding a debug `vmlinux`
or Linux source without establishing usable build inputs does not fix the
Mapple compilation requirement. Reverse engineering is outside this support model.

## Where the files belong and what must match

Build inputs, toolchains, source trees, retained module debug files, Linux symbols,
and DWARF can all live on the **development host**. The target needs its running
kernel, the uploaded module, a way to control loading, and debugger access only
when live debugging is requested. It need not store the source or debug files.

All artifacts must correspond to the target build or the exact module loaded.
A release number alone cannot distinguish different patches/configurations using
the same name. Keep provenance tying the development package/build outputs,
symbols, and source to that build; do not mark an imported set verified merely
because its filenames or release strings match. Runtime addresses and source
paths must also resolve correctly. A future verification procedure must establish
that match; this document specifies the requirement, not a certification method
for every Linux device.

Adding local symbols or source normally changes what the tools know; it does not
replace the running kernel. Installing another kernel and rebooting is a separate,
explicit preparation operation.

## Current implementation

The current lab provisions Ubuntu/QEMU targets, and its
[VS Code launch](../../.vscode/launch.json) is x86-64.
Its build-and-run path does not fundamentally require Linux debug information,
but its full debug preparation and launch require matching Linux source and a
DWARF-bearing `vmlinux`.

The module-only and reduced-symbol debugger modes, independent kernel catalog,
capability reports, and stronger build-identity checks described here remain
proposed work. The requirements above explain what is technically needed; they
do not imply every combination is already implemented by Make.

Building Linux ourselves is a management/provenance capability, not a fifth tier.
It can provide all Tier 3 artifacts and boot/install outputs together. Imported
matching artifacts can also establish Tier 3. Kernel entries should record their
artifacts and build identity; machine entries should reference the associated
kernel and record verification against what is actually running.

Use the [setup guide](../guides/setup.md) and [usage guide](../guides/usage.md)
for commands supported by the current repository.

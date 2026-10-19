# Kernel support tiers

[Documentation](../README.md) / Architecture

We always build Mapple from this repository for the target kernel, then upload
it. The target can be a VM or another Linux device.

**Building needs matching kernel interfaces and a suitable toolchain.
Loading needs compatibility and permission.
Debugging additionally needs debugger access and the appropriate symbols/source.**
These are separate requirements.

This document describes the proposed support model. See
[current implementation](#current-implementation) for what the repository supports today.

## 1. What we need to build

We need Mapple's source, a compiler/linker for the target architecture, and this
**target-specific information**:

| Required information | What it covers |
| --- | --- |
| Architecture and compilation requirements | Instruction set, calling conventions, and required compiler features/settings |
| Kernel interfaces Mapple uses | Function declarations, structure layouts and field offsets, constants, and inline/helper behavior, including relevant configuration and patches |
| Exported symbols | Which functions/data modules may import, with applicable restrictions and namespaces |
| Module compatibility metadata | Required module format and metadata, including symbol-version checksums when `CONFIG_MODVERSIONS` is enabled |

The usual way to get this is a **matching prepared kernel headers/build package**:
headers, generated configuration, Kbuild scripts/tools, and export metadata
normally stored in `Module.symvers`. Ordinary `/usr/include/linux` headers or a
`.config` alone are insufficient. `modules_prepare` does not produce the
`Module.symvers` needed for symbol versioning.
[Linux build documentation](https://docs.kernel.org/kbuild/modules.html)

**Those original files are not fundamentally mandatory.** We can supply our own
build tools and reconstruct equivalent definitions and metadata. They must
reproduce everything Mapple depends on; a version number alone is not enough.
The loader normally resolves imported function addresses, so we do not need to
hard-code them.

Full Linux source, `vmlinux`, Linux debug information, and a running target are
**not required just to build**. Inputs can live on the development host.
Compiling on the target does not remove these requirements. Mapple's code must
also support the target's kernel APIs; matching headers cannot fix incompatible
API usage.

## 2. What we need to load and run

After building, the target must:

- Run a kernel compatible with the module's architecture and interfaces.
- Have module loading enabled and the kernel services Mapple uses available.
- Allow us to transfer and load the module with sufficient privileges.
- Accept its signature if trusted module signatures are enforced.

Linux does not guarantee a stable internal module ABI across arbitrary builds.
A matching release string or successful load does not prove every interface is
correct. Signing establishes trust, not compatibility.
[Kernel interfaces](https://docs.kernel.org/process/stable-api-nonsense.html) ·
[Module signing](https://docs.kernel.org/admin-guide/module-signing.html)

**Build and run can work with no debugger at all.** For end-to-end application
tests, also run the Go server and TUI/API client with a working TCP or character
device transport.

## 3. What we need to debug

Every live debugging mode needs GDB for the target architecture and a connection
that can stop execution and read kernel memory/registers: for example, QEMU's
GDB stub, KGDB, or hardware debug access. **SSH alone is not such a connection.**
[QEMU debugging](https://www.qemu.org/docs/master/system/gdb.html)

To source-debug **Mapple**, also provide:

- The matching debug-enabled module ELF, usually the unstripped `mapple.ko`.
- Mapple's source and correct source-path mappings.
- The module's runtime section addresses, refreshed after each reload. Our lab
  reads these from `/sys/module/mapple/sections/`.

Source files alone do not supply instruction-to-line mappings; the module's
DWARF debug information does. **Linux `vmlinux` is not fundamentally required
for Mapple source debugging.** VS Code and GDB TUI use the same underlying data.
[GDB symbol loading](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Files.html)

### The four tiers

Tiers describe the kernel information/build inputs we have. Tiers 1–3 form a
ladder: each higher tier adds to the earlier requirements. **Every debugging
result below also requires the debugger connection and Mapple debugging inputs
listed above.**

| Tier | What establishes it or is added | What becomes possible |
| --- | --- | --- |
| **0 — No build support** | No usable matching module build environment yet | Inspect the target and report missing inputs; Mapple's build workflow is blocked |
| **1 — Module development** | Matching prepared kernel build files, or validated equivalent inputs | Build Mapple; debug its C source and step through raw Linux instructions by address |
| **2 — Kernel symbol debugging** | Add matching Linux names/addresses, usually a symbol-bearing `vmlinux` | Break on Linux function names and view named assembly |
| **3 — Kernel source debugging** | Add matching Linux source and DWARF debug information | Step into Linux C code and inspect recorded types/variables |

Tier 1 permits build-and-run-only development when loading is allowed but no
debugger connection exists.

The boundaries between debugging levels are:

- **Instruction bytes** let GDB decode assembly such as `mov` and `call`, even
  without Linux symbols. A symbol table adds function names such as `kernel_sendmsg`.
- **DWARF** adds source-line mappings and recorded type/variable information.
  It may be inside `vmlinux` or in a separate matching debug file.
- **Linux source files** provide the C text. Source plus DWARF enables Linux C
  debugging; source alone does not. DWARF without source still supports recorded
  type/variable inspection but cannot display the missing C text.

A file named `vmlinux` does not guarantee it contains symbols or DWARF. A boot
image (`vmlinuz`/`bzImage`) is not a development package or debug ELF. Text symbol
maps such as `System.map` or accessible `/proc/kallsyms` need an import/conversion
step rather than being drop-in GDB ELF files. All symbol addresses must account
for runtime relocation, including KASLR. Optimization can limit variable inspection.

**Ctrl+click navigation is separate:** clangd needs matching Linux source,
prepared headers/configuration, and a suitable compilation database. It does not
need DWARF, a debugger, or a running VM.

## 4. What we can get from the VM

These sources may be absent or restricted. They are inspection opportunities,
not guarantees for every Linux machine.

| Information | Direct source, if available | If absent |
| --- | --- | --- |
| Architecture/release | `uname` and system information | Inspect the kernel image |
| Configuration | `/proc/config.gz` or saved `/boot/config-<release>` | Infer the relevant effects from code/data; the exact original configuration may be unrecoverable |
| Structure layouts, enums, function types | BTF at `/sys/kernel/btf/vmlinux`, or retained debug information | Reverse engineer accesses and function callers |
| Symbol names/addresses | Permitted `/proc/kallsyms` or retained symbol files | Analyze the kernel image/data |
| Exports, version checksums, loader metadata | Retained `Module.symvers`, kernel data/image, or existing module files | Analyze export tables and the loader; existing modules cover only their own dependencies |
| A complete build environment | Matching files already on disk, often through `/lib/modules/<release>/build` | Obtain a matching package or reconstruct equivalent inputs |

BTF contains useful type information, **not all macros, inline helpers, build
settings, or API semantics**. `/sys/kernel/btf/vmlinux` is a BTF blob, not the ELF
`vmlinux` used by GDB. Symbol names alone do not establish exports or version
checksums. [BTF specification](https://docs.kernel.org/bpf/btf.html)

Reverse engineering may recover enough equivalent interface information,
including helper behavior, locking, ownership, and lifetime rules. It cannot
necessarily recover the original source, headers, or configuration: compilation
can discard that information. A kernel image or BTF alone therefore does not
automatically establish Tier 1. Reconstruction must produce usable, validated
build inputs; success is not guaranteed on an arbitrary target.

### What must come from outside

For **compilation**, no particular original developer file is inherently required
if we can reconstruct the necessary interfaces. Matching developer/distribution
build files are the practical way to avoid that work. They can also come from a
local archive, so offline development is possible.

For **loading**, we need permission and, when signatures are enforced, authorized
signing or an allowed trust/policy change. The kernel's public certificate does
not reveal its private signing key. Reverse engineering interfaces does not grant
loading authorization.

Keep records of where build inputs came from. For reconstructed inputs, record
evidence, assumptions, and validation. Files can remain on the development host;
adding local symbols/source does not require changing or rebooting the target.

## Current implementation

The current lab provisions Ubuntu/QEMU targets with an
[x86-64 VS Code configuration](../../.vscode/launch.json). Its full debugging
workflow requires matching Linux source and a DWARF-bearing `vmlinux`, even
though reduced debugging modes are technically possible.

Tier reporting, reduced debugger modes, an independent kernel catalog, and
reconstruction workflows are proposed work, not existing Make features.
Building Linux ourselves can provide all Tier 3 files; imported matching files
can do the same. It is not a fifth tier.

Use the [setup guide](../guides/setup.md) and [usage guide](../guides/usage.md)
for supported commands.

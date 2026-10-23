"""Public Make command reference, available without a configured lab or toolchain."""
from dataclasses import dataclass
import sys
import textwrap

from . import core as c


@dataclass(frozen=True)
class Command:
    summary: str
    usage: str
    options: tuple = ()
    examples: tuple = ()
    details: str = ''
    required: tuple = ()


OPTIONS = {
    'COMMAND': 'Command to explain; omit it to show all command groups.',
    'NAME': 'Name of the new entry, or the machine selected by machine-use.',
    'KERNEL': 'Named kernel slot. Build/navigation commands otherwise use the selected machine\'s assigned kernel; removal requires an explicit KERNEL.',
    'MACHINE': 'Machine name for this command; otherwise use the persistent active machine. Removal/destruction requires an explicit MACHINE.',
    'RUN': 'Frozen debug run ID from make debug-list. Otherwise select the single prepared/detached run for the selected machine.',
    'VERSION': 'Linux version to download and build; default 6.6.6.',
    'ARCH': 'Target architecture; default x86_64. Also supports aarch64, arm, riscv64 and imported SDK architectures.',
    'LINUX_ARCH': 'Kbuild ARCH override; normally derived from ARCH.',
    'DEFCONFIG': 'Linux configuration target; normally derived from ARCH.',
    'IMAGE_PATH': 'Relative boot-image path in the Linux tree; normally derived from ARCH.',
    'PROFILE': 'Linux configuration profile; currently debug (default).',
    'CONFIG': 'Path to a Linux .config to copy into the managed snapshot.',
    'PATCHES': 'Directory of sorted *.patch files to copy into the recipe.',
    'CC': 'Compiler command to persist for this kernel; x86_64 defaults to gcc-12.',
    'CROSS_COMPILE': 'Cross-toolchain binary prefix, for example aarch64-linux-gnu-.',
    'LLVM': 'Set to 1 to use LLVM tools.',
    'TOOLCHAIN': 'Path to a JSON SDK description: cc, cross_compile, optional llvm and env.',
    'JOBS': 'Parallel workers; Linux builds default to the CPU count capped at 4, navigation to 4.',
    'BUILD': 'Matching prepared kernel build tree/headers to copy. Required for module compilation, but not for creating an incomplete Tier 0 record.',
    'SOURCE': 'Matching Linux source tree to copy for navigation and source debugging.',
    'VMLINUX': 'Matching Linux ELF with symbols; debug info enables richer inspection.',
    'DEBUG_INFO': 'Separate matching Linux debug ELF containing DWARF.',
    'SYSTEM_MAP': 'Matching text kernel symbol map.',
    'BUILD_ID': 'Expected GNU build ID when no Linux symbol ELF is available.',
    'IMAGE': 'Bootable Linux image for installation.',
    'MODULES': 'Archive of Linux modules, with the release directory at the archive root.',
    'BTF': 'Optional BTF metadata blob; it does not establish module build support.',
    'SIGNER': 'Local executable to sign the frozen module before upload. See the usage guide for the hook contract.',
    'CASCADE': 'Set to 1 to also retire referencing machine records. Preserves their VM disks and connection details; does not stop running VMs.',
    'HOST': 'Target SSH hostname or address. machine-add prompts if omitted.',
    'SSH_USER': 'SSH login user. machine-add prompts if omitted.',
    'SSH_PORT': 'SSH port; default 22 for an existing machine.',
    'SSH_KEY': 'Path to a private SSH key; otherwise use SSH defaults/agent.',
    'ASK_PASS': 'Set to 1 to privately prompt for SSH and sudo passwords.',
    'GDB_ENDPOINT': 'Existing debugger endpoint: host:port or a local serial-device path. Set to none when configuring to disable it.',
    'MODULE_PORT': 'Host-reachable module TCP port; default 4444 for existing machines. Managed VMs allocate forwarding automatically.',
    'MEMORY': 'Managed VM RAM in MiB; default 4096.',
    'CPUS': 'Managed VM virtual CPU count; default 2.',
    'DISK': 'Managed VM disk capacity; default 30G.',
    'INSTALLER': 'Local installer hook for an unfamiliar boot system; see the usage guide. Configure as none to clear it.',
    'FETCHER': 'Local artifact-fetching hook for kernel adoption; see the usage guide.',
    'OFFLINE': 'Set to 1 to prevent built-in adoption downloads; custom fetchers must honor it.',
    'MIN_DEBUG': 'Require module (default), kernel-symbol, or kernel-source. The best available mode is selected; a missing minimum fails before upload.',
    'MODULE_PARAMS': 'Space-separated key=value module arguments; Mapple currently declares none.',
    'GDB': 'GDB executable override; defaults to gdb or gdb-multiarch for foreign targets.',
    'TERMINAL': 'Terminal launcher prefix, for example "xterm -e".',
    'FOREGROUND': 'Set to 1 to launch the TUI/GDB in this terminal instead of a new window.',
    'NO_OPEN': 'Set to 1 to generate the VS Code workspace without opening it.',
    'NO_INSTALL': 'Set to 1 to skip host package and editor-extension installation; Go dependency setup still runs.',
    'TOOLCHAIN_PACKAGES': 'Additional Ubuntu toolchain packages to install explicitly, for example "gcc-aarch64-linux-gnu".',
    'CLANGD': 'Native navigation engine; default clangd-18.',
    'NAV_TIMEOUT': 'Navigation indexing timeout in seconds; default 1800.',
}

ARTIFACTS = ('BUILD', 'SOURCE', 'VMLINUX', 'DEBUG_INFO', 'SYSTEM_MAP', 'CONFIG', 'IMAGE', 'MODULES', 'BTF')
TOOLCHAIN = ('CC', 'CROSS_COMPILE', 'LLVM', 'TOOLCHAIN', 'SIGNER')
MACHINE = ('MACHINE',)
KERNEL = ('KERNEL', 'MACHINE')
DEBUG = ('MACHINE', 'MIN_DEBUG', 'MODULE_PARAMS', 'SIGNER')

# Keep the short descriptions and detailed command help in the same catalog.
COMMANDS = {
    'setup': Command('Install host tools and Go dependencies.', '[NO_INSTALL=1]',
        ('NO_INSTALL', 'TOOLCHAIN_PACKAGES'), ('make setup',),
        'Uses sudo/apt on Ubuntu. Installs the navigation extension when code is available; does not create a kernel or machine.'),
    'kernel-create': Command('Define a new Linux build recipe.', 'NAME=<kernel> [VERSION=6.6.6] [PROFILE=debug]',
        ('NAME', 'VERSION', 'ARCH', 'PROFILE', 'CONFIG', 'PATCHES', 'LINUX_ARCH', 'DEFCONFIG', 'IMAGE_PATH') + TOOLCHAIN,
        ('make kernel-create NAME=baseline VERSION=6.6.6', 'make kernel-build KERNEL=baseline'),
        'Does not compile Linux or change any machine. Different Linux source, config or patches require a new named slot.', ('NAME',)),
    'kernel-import': Command('Copy existing kernel artifacts into a new slot.', 'NAME=<kernel> [BUILD=<path>] [SOURCE=<path>] [VMLINUX=<path>]',
        ('NAME', 'ARCH', 'LINUX_ARCH', 'BUILD_ID') + ARTIFACTS + TOOLCHAIN,
        ('make kernel-import NAME=client BUILD=/path/to/prepared-kernel',),
        'Copies a stable snapshot into .lab. Incomplete imports remain Tier 0 until matching build inputs are added.', ('NAME',)),
    'kernel-build': Command('Compile Linux, or reuse an unchanged completed build.', '[KERNEL=<kernel>] [JOBS=4]',
        KERNEL + ('JOBS',), ('make kernel-build KERNEL=baseline',),
        'Downloads/builds the kernel recipe locally. Does not boot a machine or install the kernel on it.'),
    'kernel-enrich': Command('Add missing matching artifacts to an existing kernel slot.', '[KERNEL=<kernel>] [BUILD=<path>] [SOURCE=<path>] [VMLINUX=<path>]',
        KERNEL + ARTIFACTS + TOOLCHAIN, ('make kernel-enrich KERNEL=client SOURCE=/path/to/linux VMLINUX=/path/to/vmlinux',),
        'Checks and seals the added artifacts; existing artifacts cannot be replaced. The support tier can increase.'),
    'kernel-list': Command('List kernel slots, support tiers and assignments.', '',
        examples=('make kernel-list',), details='Offline; no active machine is needed.'),
    'kernel-show': Command('Show a kernel slot\'s identity, artifacts and toolchain.', '[KERNEL=<kernel>]',
        KERNEL, ('make kernel-show KERNEL=baseline',)),
    'kernel-remove': Command('Delete a named kernel snapshot and its navigation index.', 'KERNEL=<kernel> [CASCADE=1]',
        ('KERNEL', 'CASCADE'), ('make kernel-list', 'make kernel-remove KERNEL=old-kernel',
        'make kernel-remove KERNEL=old-kernel CASCADE=1'),
        'Requires an explicit KERNEL; never implicitly deletes the active machine\'s kernel. Refuses assigned/requested kernels unless CASCADE=1. Active debug runs block removal. Deletion cannot be undone by this command.', ('KERNEL',)),
    'machine-create': Command('Create an Ubuntu x86-64 QEMU VM and SSH configuration.', 'NAME=<machine> KERNEL=<kernel> [MEMORY=4096] [CPUS=2] [DISK=30G]',
        ('NAME', 'KERNEL', 'MEMORY', 'CPUS', 'DISK'),
        ('make machine-create NAME=dev KERNEL=baseline', 'make machine-start', 'make machine-prepare'),
        'Downloads a verified Ubuntu cloud image and creates a disk. Does not start the VM or install the requested kernel. Selects the first machine automatically.', ('NAME', 'KERNEL')),
    'machine-add': Command('Register and inspect an existing Linux target over SSH.', 'NAME=<machine> [HOST=<host>] [SSH_USER=<user>]',
        ('NAME', 'HOST', 'SSH_USER', 'SSH_PORT', 'SSH_KEY', 'ASK_PASS', 'GDB_ENDPOINT', 'MODULE_PORT', 'INSTALLER', 'CC', 'CROSS_COMPILE'),
        ('make machine-add NAME=client HOST=192.0.2.10 SSH_USER=developer SSH_KEY=~/.ssh/id_ed25519',),
        'Prompts for omitted host/user. Records the running kernel without replacing it; use machine-adopt or machine-prepare separately.', ('NAME',)),
    'machine-use': Command('Select the default machine and its kernel for new commands.', 'NAME=<machine>',
        ('NAME',), ('make machine-list', 'make machine-use NAME=dev'),
        'Persistent selection; updates editor navigation offline. Existing servers and debug sessions keep their original target.', ('NAME',)),
    'machine-list': Command('List machines, active selection, kernels and blockers.', '',
        examples=('make machine-list',), details='Offline. RUNNING is the last observed kernel release; machine-inspect refreshes it.'),
    'machine-show': Command('Show saved machine details and managed QEMU status.', '[MACHINE=<machine>]',
        MACHINE, ('make machine-show MACHINE=dev',), 'Credentials are redacted.'),
    'machine-configure': Command('Update saved connection, debugger or installer settings.', '[MACHINE=<machine>] [GDB_ENDPOINT=<host:port>]',
        MACHINE + ('HOST', 'SSH_USER', 'SSH_PORT', 'SSH_KEY', 'ASK_PASS', 'MODULE_PORT', 'GDB_ENDPOINT', 'INSTALLER'),
        ('make machine-configure MACHINE=client GDB_ENDPOINT=127.0.0.1:1234',),
        'Does not change the kernel association. Managed QEMU connection settings belong to machine-create and cannot be overridden here.'),
    'machine-inspect': Command('Refresh the running target\'s kernel and OS observations.', '[MACHINE=<machine>]',
        MACHINE, ('make machine-inspect MACHINE=dev',), 'Connects over SSH without installing or rebooting.'),
    'machine-adopt': Command('Collect matching artifacts for the target\'s current kernel.', '[MACHINE=<machine>] [KERNEL=<slot>] [OFFLINE=1]',
        MACHINE + ('KERNEL', 'OFFLINE', 'FETCHER'), ('make machine-adopt MACHINE=client OFFLINE=1',),
        'Inspects local target files first, then may download matching packages. Does not replace the running kernel. Keeps an incomplete Tier 0 record if build inputs are unavailable.'),
    'machine-prepare': Command('Install a kernel, reboot, verify and update the assignment.', '[MACHINE=<machine>] [KERNEL=<kernel>]',
        KERNEL, ('make machine-prepare MACHINE=dev KERNEL=baseline',),
        'Uses the requested/assigned kernel when KERNEL is omitted. Changes the target and reboots it. Retains the previous kernel and uses fallback boot entries where supported; only commits a verified transition.'),
    'machine-start': Command('Boot an owned QEMU VM and wait for SSH/cloud-init.', '[MACHINE=<machine>]',
        MACHINE, ('make machine-list', 'make machine-start MACHINE=dev'),
        'Requires a VM created with machine-create. Reuses an already running VM. Does not build or install Linux; run machine-prepare after the first boot.'),
    'machine-stop': Command('Request graceful shutdown of an owned QEMU VM.', '[MACHINE=<machine>]',
        MACHINE, ('make machine-stop MACHINE=dev',), 'Sends ACPI shutdown and returns before shutdown completes. Does not delete the VM.'),
    'machine-ssh': Command('Open SSH using saved connection details.', '[MACHINE=<machine>]',
        MACHINE, ('make machine-ssh MACHINE=dev',)),
    'machine-remove': Command('Retire a machine record while keeping its VM assets.', 'MACHINE=<machine>',
        MACHINE, ('make machine-list', 'make machine-remove MACHINE=old-dev'),
        'Requires an explicit MACHINE. Preserves disks and connection details in retired.json; does not stop a running VM. Active debug runs block removal. Use machine-destroy to delete an owned VM.', ('MACHINE',)),
    'machine-destroy': Command('Stop and delete an owned QEMU VM, then retire its record.', 'MACHINE=<machine>',
        MACHINE, ('make machine-show MACHINE=old-dev', 'make machine-destroy MACHINE=old-dev'),
        'Requires an explicit MACHINE. Deletes its VM disk and seed; data is lost. Only supports VMs created by this repository. Use machine-remove to preserve disks.', ('MACHINE',)),
    'module-build': Command('Compile Mapple against the selected kernel, offline.', '[KERNEL=<kernel>] [MACHINE=<machine>]',
        KERNEL, ('make module-build KERNEL=baseline',), 'No live machine needed when KERNEL is explicit. Does not upload or load the module.'),
    'module-run': Command('Build, verify, upload and reload Mapple on the target.', '[MACHINE=<machine>]',
        MACHINE + ('MODULE_PARAMS', 'SIGNER'), ('make module-run MACHINE=dev',),
        'Uses the machine\'s assigned kernel. Close application sessions before reload. No debugger endpoint or Linux debug symbols required.'),
    'module-debug': Command('Build/upload Mapple and prepare a frozen debug run.', '[MACHINE=<machine>] [MIN_DEBUG=module]',
        DEBUG, ('make module-debug MACHINE=dev', 'make gdb-open'),
        'Requires a debugger endpoint. Reports the best available mode and fails before upload if MIN_DEBUG is unavailable. Does not itself attach GDB; use gdb-open. For VS Code use vscode-open then F5 instead.'),
    'server-build': Command('Build the Go server into .lab/builds/go/mappled.', '', examples=('make server-build',)),
    'server-run': Command('Build and run the machine-bound Go server in this terminal.', '[MACHINE=<machine>]',
        MACHINE, ('make server-run MACHINE=dev',), 'Creates/reuses private TLS/auth configuration and a reserved local port. Keep it running while using the TUI.'),
    'tui-build': Command('Build the Go TUI into .lab/builds/go/mapple.', '', examples=('make tui-build',)),
    'tui-run': Command('Build and open the selected machine\'s TUI in a new terminal.', '[MACHINE=<machine>] [FOREGROUND=1]',
        MACHINE + ('FOREGROUND', 'TERMINAL'), ('make tui-run MACHINE=dev', 'make tui-run FOREGROUND=1'),
        'Start server-run for the same machine first. FOREGROUND=1 works without a graphical terminal.'),
    'gdb-open': Command('Open GDB TUI for a prepared debug run.', '[RUN=<run-id>] [MACHINE=<machine>] [FOREGROUND=1]',
        ('RUN', 'MACHINE', 'GDB', 'FOREGROUND', 'TERMINAL'), ('make module-debug', 'make gdb-open', 'make gdb-open RUN=run-... FOREGROUND=1'),
        'Attaches to the same frozen module and runtime addresses. Attachment pauses the target; continue resumes it, detach releases it.'),
    'gdb-attach': Command('Attach GDB to a prepared debug run in this terminal.', '[RUN=<run-id>] [MACHINE=<machine>]',
        ('RUN', 'MACHINE', 'GDB'), ('make debug-list', 'make gdb-attach RUN=run-...'),
        'Use gdb-open for a TUI window. A second debugger cannot share the same target endpoint.'),
    'vscode-open': Command('Generate and open a machine-bound VS Code debug workspace.', '[MACHINE=<machine>] [MIN_DEBUG=module] [NO_OPEN=1]',
        MACHINE + ('MIN_DEBUG', 'NO_OPEN'), ('make vscode-open MACHINE=dev MIN_DEBUG=kernel-source',),
        'Uses Mapple Navigation and Microsoft C/C++ for debugging. Press F5 to build/upload and attach; Restart rebuilds/reloads; Stop detaches and resumes the VM. Static Ctrl+click navigation is handled separately by clangd.'),
    'debug-list': Command('List frozen module runs and debugger states.', '', examples=('make debug-list',)),
    'navigation-setup': Command('Install/update the local Mapple Navigation VS Code extension.', '',
        examples=('make navigation-setup',), details='Requires the code CLI; also installs its clangd extension dependency. Reload an already-open VS Code window once.'),
    'navigation-prepare': Command('Prepare compiler commands and the persistent clangd index.', '[KERNEL=<kernel>] [JOBS=4]',
        KERNEL + ('CLANGD', 'JOBS', 'NAV_TIMEOUT'), ('make navigation-prepare KERNEL=baseline',),
        'Builds locally and checks jumps to implementations. Does not contact a target or change selection. Repeat per kernel; unchanged indexes are reused.'),
    'navigation-status': Command('Show navigation preparation and live editor status.', '[KERNEL=<kernel>]',
        KERNEL, ('make navigation-status', 'make navigation-status KERNEL=baseline'), 'Offline; also works without an active machine.'),
    'doctor': Command('Inspect the selected target and report development blockers.', '[MACHINE=<machine>]',
        MACHINE, ('make doctor MACHINE=dev',), 'Checks build artifacts, compatibility, compiler and module-loading permissions. Does not attach to the debugger or pause the VM.'),
    'clean': Command('Remove Mapple and Go build outputs.', '', examples=('make clean',),
        details='Preserves Linux builds, downloads, machine records, VM disks, frozen runs and navigation databases/indexes.'),
    'check': Command('Run API verification, tool tests and Go/Python checks.', '', examples=('make check',),
        details='Builds, vets and tests Go; compile-checks Python. Does not run live VM/editor integration tests.'),
    'test': Command('Run Go tests.', '', examples=('make test',)),
    'test-tools': Command('Run development-tool regression tests.', '', examples=('make test-tools',)),
    'test-navigation': Command('Test real clangd and VS Code navigation lifecycle.', '', examples=('make test-navigation',),
        details='Uses a disposable checkout. Requires code, the clangd extension and xvfb-run on a headless host. Preserves your normal editor profile and selection.'),
    'test-debugging': Command('Test the real VS Code debugger controls against a disposable target.', 'MACHINE=<disposable-machine>',
        MACHINE, ('make test-debugging MACHINE=debug-test',),
        'Reloads Mapple repeatedly on an already prepared Tier 3 machine. Tests toolbar and console controls, source breakpoints, memory/disassembly, stepping, Restart and Stop/F5 using an isolated editor profile. Requires code, Microsoft C/C++, clangd, and xvfb-run when headless. Does not destroy the supplied machine.', ('MACHINE',)),
    'api': Command('Regenerate Go API bindings from the API definition.', '', examples=('make api',),
        details='Modifies generated source files. Review the diff before committing.'),
    'api-verify': Command('Verify generated API bindings without changing source.', '', examples=('make api-verify',)),
    'help': Command('Browse commands or explain one command.', '[COMMAND=<command>]', ('COMMAND',),
        ('make', 'make help COMMAND=kernel-remove', 'make kernel-remove HELP=1'),
        'HELP=1 is available on every public Make command and never executes that command or its prerequisites. Bare commands with required naming options also show help; complete build/run commands still execute normally.'),
}

GROUPS = {
    'Host': ('setup',),
    'Kernels': tuple(n for n in COMMANDS if n.startswith('kernel-')),
    'Machines': tuple(n for n in COMMANDS if n.startswith('machine-')),
    'Build and run': ('module-build', 'module-run', 'server-build', 'server-run', 'tui-build', 'tui-run'),
    'Debugging': ('module-debug', 'gdb-open', 'gdb-attach', 'vscode-open', 'debug-list'),
    'Source navigation': ('navigation-setup', 'navigation-prepare', 'navigation-status'),
    'Checks and maintenance': ('doctor', 'check', 'test', 'test-tools', 'test-navigation', 'test-debugging', 'api', 'api-verify', 'clean', 'help'),
}


class UsageError(RuntimeError):
    """Missing required inputs, with public command help attached by the CLI."""


def paragraph(text, stream, indent=''):
    print(textwrap.fill(text, width=88, initial_indent=indent, subsequent_indent=indent), file=stream)


def show(command=None, stream=None):
    stream = stream or sys.stdout
    if not command:
        print('Mapple development commands\n', file=stream)
        print('Learn a command:  make <command> HELP=1', file=stream)
        print('                  make help COMMAND=<command>\n', file=stream)
        print('First lab: setup -> kernel-create -> kernel-build -> machine-create', file=stream)
        print('           -> machine-start -> machine-prepare -> module-run\n', file=stream)
        for group, names in GROUPS.items():
            print(group + ':', file=stream)
            for name in names:
                print(f'  {name:20} {COMMANDS[name].summary}', file=stream)
            print(file=stream)
        print('New commands use the active machine; MACHINE=<name> overrides it once.', file=stream)
        print('Full guides: docs/guides/setup.md and docs/guides/usage.md', file=stream)
        return
    if command not in COMMANDS:
        raise UsageError(f'Unknown command: {command}. Run make help to list commands.')
    entry = COMMANDS[command]
    print(f'{command} — {entry.summary}\n', file=stream)
    print(f'Usage: make {command} {entry.usage}'.rstrip(), file=stream)
    if entry.required:
        print('Required: ' + ', '.join(entry.required), file=stream)
    if entry.details:
        print(file=stream)
        paragraph(entry.details, stream)
    if entry.options:
        print('\nOptions:', file=stream)
        for name in entry.options:
            print('  ' + name, file=stream)
            paragraph(OPTIONS[name], stream, '    ')
    print('\nExamples:', file=stream)
    for example in entry.examples or (f'make {command}',):
        print('  ' + example, file=stream)
    print('\nHelp only (never executes the command):', file=stream)
    print(f'  make {command} HELP=1', file=stream)
    print('Guide: docs/guides/usage.md', file=stream)


def needs_help(command):
    """Bare argument-taking commands explain themselves instead of operating."""
    entry = COMMANDS.get(command)
    if not entry:
        return False  # Private VS Code commands keep their own contract.
    missing = [key for key in entry.required if not c.option(key)]
    if not missing:
        return False
    if any(c.option(key) for key in entry.required):
        raise UsageError('Missing required options: ' + ', '.join(missing))
    show(command)
    return True

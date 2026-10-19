<p align="center">
  <img src="docs/assets/maple-leaf.svg" width="128" height="128" alt="A red and amber maple leaf">
</p>

# Mapple

Explore Linux from the kernel, through an HTTPS API and a terminal interface.

Mapple combines a C kernel module, a Go server, and a Go TUI. Inspect processes
and threads, query module capabilities, and send echo requests through a live
kernel session. An Ubuntu/QEMU development lab brings the whole stack together,
with source breakpoints, kernel stepping, and navigation into Linux definitions.

```text
Ubuntu host                                  Ubuntu QEMU guest

mapple TUI ── HTTPS ── mappled ── TCP ──────── mapple.ko
                           Go server          C kernel module

VS Code / GDB TUI ── QEMU GDB stub ────────── kernel + module
```

The server also supports a local `/dev/mapple` character device when it runs on
the module's machine. The TUI communicates through the server's authenticated
HTTPS API.

## Get started

Use an **x86-64 Ubuntu 24.04 host** with sudo and an internet connection. KVM is
recommended. Allow at least **10 GiB free for the kernel build**, plus room for
the Ubuntu image and VM disk growth. The default guest uses 4 GiB RAM and 2 CPUs;
see the [setup guide](docs/guides/setup.md) for prerequisites and smaller VM settings.

```bash
sudo apt-get update
sudo apt-get install -y git make python3
git clone https://github.com/GhostDog788/Mapple.git
cd Mapple
make setup
make vm-create NAME=dev
make vm-up
make vm-setup
make run-kernel
```

`make setup` installs the remaining host tools. The first VM is selected automatically.
`vm-setup` builds a Linux 6.6.6 debug kernel, installs it in the guest, and reboots
the guest. This first build takes several minutes; subsequent setups reuse it.

Start the server in one terminal:

```bash
make run-server
```

From the repository root in another terminal, launch the TUI:

```bash
make run-tui
```

Inside the TUI:

```text
session create dev
command info
command echo hello
command pslist
```

You now have a session to the module running in the guest. `command pslist`
opens a process and thread snapshot; press Esc to return to the command prompt.
For a terminal-only host, launch the TUI
with `make run-tui FOREGROUND=1`.

## Develop and debug

Build any component with `make kernel`, `make server`, or `make tui`. Reload the
module with `make run-kernel`. Run the repository checks with `make check`.

For source debugging, open the repository in VS Code, install the recommended
C/C++ and clangd extensions, select **Kernel: cppdbg (full GDB MI)**, and press
**F5**. Set breakpoints in `kernel/src/` and trigger requests from the TUI. To use
GDB TUI instead, run `make run-kernel-debug`, then `make gdb`.
The [usage guide](docs/guides/usage.md#kernel-debugging) covers both workflows and
Ctrl+click navigation into the matching Linux source.

## Documentation

The [documentation index](docs/README.md) organizes guides, architecture, and
reference material.

| Guide | What you will find |
| --- | --- |
| [Setup](docs/guides/setup.md) | From a fresh Ubuntu host to a working lab; new or existing QEMU VMs |
| [Usage](docs/guides/usage.md) | Daily development, every Make target and option, debugging, and troubleshooting |
| [Architecture](docs/architecture/overview.md) | Components, transports, API, and session ownership |
| [Server installation](docs/guides/server-installation.md) | Deploying the Go server and TUI outside the local lab |

The application lives in [`kernel/src/`](kernel/src), [`shared/`](shared), and
[`user/`](user). Development tooling lives in [`scripts/`](scripts) and
[`kernel/scripts/`](kernel/scripts). Private lab state and VM disks stay in the
gitignored `.lab/` directory.

## Meet Mapple

This project is named after my dog, **Mapple**. “Mapple” is how I spell her name
in English. It started as a spellig mistake when I was 14 and I caried it on :)

<img src="docs/assets/mapple.jpg" width="480" alt="Mapple, a brown and white dog, curled up asleep on a tiled floor">

# Mapple documentation

[Project overview](../README.md)

Start with the setup guide to build a local Ubuntu/QEMU lab. Use the usage guide
while developing, and the architecture and reference sections when working on
internals or integrating with the API.

## Start here

1. **[Set up a development lab](guides/setup.md)** — prepare a fresh Ubuntu host,
   create or register a VM, and make your first request.
2. **[Work with Mapple](guides/usage.md)** — follow the daily development loop,
   choose a VM, build and run components, and debug the kernel in VS Code or GDB TUI.
3. **[Understand the architecture](architecture/overview.md)** — see how the
   module, HTTPS server, TUI, transports, and session leases fit together.

## Guides

Instructions for getting things done.

| Guide | Use it to |
| --- | --- |
| [Setup](guides/setup.md) | Prepare the host and guest, configure VM slots, and run the stack |
| [Usage](guides/usage.md) | Find every Make target and option, navigate Linux source, debug, and troubleshoot |
| [Server installation](guides/server-installation.md) | Install the server and TUI outside the local lab, including TLS, authentication, systemd, and networking |

## Architecture

Explanations of how the system works and the invariants its components maintain.

| Document | Covers |
| --- | --- |
| [Overview](architecture/overview.md) | Components, module transports, HTTPS API, authentication, and lease ownership |
| [Kernel support tiers](architecture/kernel-support-tiers.md) | Standalone requirements for build/run and each debugging level, four proposed kernel tiers, and worked examples |
| [Kernel sessions](architecture/sessions.md) | Registration, reference ownership, parser cleanup, and shutdown |
| [Process snapshots](architecture/process-snapshots.md) | Collection groups, shared objects, streaming records, races, and resource lifetimes |

## Reference

Contracts for consumers and implementers.

| Reference | Covers |
| --- | --- |
| [Process snapshot JSON](reference/process-snapshot-json.md) | Schema v1 shape, scalar types, raw bytes, statuses, and collection bounds |
| [HTTPS API](../user/api/openapi.yaml) | Endpoint definitions, request/response types, authentication, and errors; maintained with the generated API bindings |

## Documentation layout

```text
docs/
├── README.md                         # This index
├── guides/
│   ├── setup.md
│   ├── usage.md
│   └── server-installation.md
├── architecture/
│   ├── overview.md
│   ├── kernel-support-tiers.md
│   ├── sessions.md
│   └── process-snapshots.md
├── reference/
│   └── process-snapshot-json.md
└── assets/                           # README and documentation images
```

Keep each topic in one place and link to it from related documents. Add practical
workflows to `guides/`, system explanations to `architecture/`, and precise data
contracts to `reference/`. Store shared images in `assets/` and use relative
links so the documentation works both in the checkout and on GitHub.

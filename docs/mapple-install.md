# Installing mappled and mapple

This covers taking the Go control plane (`docs/usermode-architecture-plan.md`)
from a checked-out repo to a running `mappled` daemon on a management host
and a `mapple` TUI on an operator's workstation. It assumes the kernel module
side (`kernel/scripts/0*.sh`) is already set up per the top-level `Makefile`
and `kernel/scripts/`; this document is only about the Go side (`user/`).

## 1. Build

```
cd user
CGO_ENABLED=0 go build -o bin/mappled ./cmd/mappled
CGO_ENABLED=0 go build -o bin/mapple ./cmd/mapple
```

(`make user` from the repo root does this, plus `go vet`/`go test`/the
OpenAPI regeneration check, and drops the binaries in the same place.)
`CGO_ENABLED=0` matters here, not as a style preference: it's what makes both
binaries statically linked, so they run on a minimal server image without
matching libc, and can be dropped straight into a container `FROM scratch`
if you go that route later. Verify with `file bin/mappled` --
"statically linked" should appear; `ldd bin/mappled` should refuse to treat
it as a dynamic executable at all.

## 2. Install the binaries

```
sudo install -m 0755 user/bin/mappled /usr/local/bin/mappled
sudo install -m 0755 user/bin/mapple  /usr/local/bin/mapple
```

`mapple` (the TUI) is the only piece meant to run unprivileged, on whatever
machine an operator is sitting at -- it never needs to be on the same host
as `mappled` or the kernel module. `mappled` runs as root (see the
"Explicit Assumptions" section of the architecture plan: arbitrary
TCP/chardev destinations are opened as root by design), so install it only
on hosts you're comfortable running a root network service on.

## 3. TLS certificate and key

`mappled` terminates HTTPS itself (see `cmd/mappled/main.go`) and refuses to
start without both `tls_cert_file` and `tls_key_file` configured
(`server/config.go`'s `LoadConfig`). Two realistic paths:

**Private management network, self-signed (recommended default):** since
the plan requires "a private management network... trusted by TUI and
automation clients" rather than public HTTPS, a self-signed cert plus
distributing its public cert to clients (as `ca_file` in the TUI profile,
see step 6) is the normal case, not a shortcut:

```
sudo mkdir -p /etc/mapple
sudo openssl req -x509 -newkey rsa:4096 -nodes \
  -keyout /etc/mapple/mappled.key -out /etc/mapple/mappled.crt \
  -days 825 -subj "/CN=mappled.internal" \
  -addext "subjectAltName=DNS:mappled.internal,IP:<mappled's real LAN IP>"
sudo chmod 600 /etc/mapple/mappled.key
```

Distribute `/etc/mapple/mappled.crt` (the public half only) to every host
that will run `mapple` or otherwise call the API, for use as `ca_file`.

**Real CA (public-facing or already have internal PKI):** point
`tls_cert_file`/`tls_key_file` at whatever your CA/ACME client (certbot,
step-ca, your org's internal CA) issues, and skip `ca_file` on clients
entirely since the system trust store already covers it.

`user/scripts/dev-setup.sh` automates the self-signed path for local
development only -- it writes into a gitignored `user/dev/` and is not
meant for production paths like `/etc/mapple`.

## 4. Bearer tokens

`mappled` never stores plaintext tokens -- only a name and a SHA-256 digest
per identity (`server/config.go`'s `Identity`), compared in constant time.
Generate one token per identity/operator:

```
token=$(openssl rand -hex 32)
token_sha256=$(printf '%s' "$token" | openssl dgst -sha256 -hex | awk '{print $NF}')
echo "give this token to the operator, store nowhere else: $token"
echo "put this in mappled.json: $token_sha256"
```

Losing the plaintext token means generating a new one -- there is no
recovery path from the stored digest, which is the point.

## 5. `/etc/mapple/mappled.json`

Matches `server/config.go`'s `fileConfig`:

```json
{
  "listen_address": "0.0.0.0:8443",
  "tls_cert_file": "/etc/mapple/mappled.crt",
  "tls_key_file": "/etc/mapple/mappled.key",
  "handshake_timeout_seconds": 30,
  "lease_timeout_minutes": 15,
  "identities": [
    { "name": "alice", "token_sha256": "<64-hex-char digest from step 4>" },
    { "name": "ci-automation", "token_sha256": "<another digest>" }
  ]
}
```

`handshake_timeout_seconds` and `lease_timeout_minutes` are optional
(default to 30s and 15m, the plan's own defaults); `listen_address` and at
least one identity are required, and `LoadConfig` refuses to start without
both TLS paths. Restrict the file itself:

```
sudo chmod 600 /etc/mapple/mappled.json
sudo chown root:root /etc/mapple/mappled.json
```

Token/identity changes require a `systemctl restart mappled` -- the plan
is explicit that this is v1 behavior (no hot reload).

## 6. systemd service

`user/packaging/systemd/mappled.service` is provided, hardened per the
plan's "systemd restart and feasible root-service hardening" (
`NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`, capability
stripping, and more -- see the unit file's own comments for what's
deliberately *not* restricted and why: `DevicePolicy` is left alone
because the daemon's whole job is opening operator-supplied chardev paths
it can't know about at deploy time).

```
sudo cp user/packaging/systemd/mappled.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mappled
sudo systemctl status mappled
journalctl -u mappled -f    # structured JSON audit logs (request id, identity, method, path, status, duration)
```

Validate the unit file itself any time with
`systemd-analyze verify /etc/systemd/system/mappled.service`.

## 7. Firewall

The plan requires that "the private management network must prevent
ordinary clients from reaching module TCP port 4444 directly. Modules may
allow multiple designated server machines." Concretely: only `mappled`
hosts should be able to reach port 4444 on any module machine, and
everything else should be able to reach only `mappled`'s HTTPS port.

On a module host (nftables example, adjust the mappled host list):

```
sudo nft add table inet mapple
sudo nft add chain inet mapple input '{ type filter hook input priority filter ; policy accept ; }'
sudo nft add rule inet mapple input tcp dport 4444 ip saddr != { 10.0.0.10, 10.0.0.11 } drop
```

(`10.0.0.10`/`10.0.0.11` standing in for your `mappled` hosts' addresses.)
On the `mappled` host itself, restrict 8443 (or whatever `listen_address`
uses) to your operator/automation network the same way, and leave 4444
closed entirely -- `mappled` reaches it over the private network, nothing
else should.

## 8. TUI profile configuration (nonprivileged, per-operator)

Each operator runs `mapple` on their own machine with their own token --
nothing in this step needs root. `user/tui/config.go` writes/reads
`~/.config/mapple/config.json` at mode `0600`:

```
mapple -init                    # writes a starter config at the default path
$EDITOR ~/.config/mapple/config.json
```

Fill in the identity's token from step 4, the daemon's URL, and either
`ca_file` (pointing at the `mappled.crt` distributed in step 3) or
`insecure_skip_verify: true` for throwaway/dev setups only:

```json
{
  "active_profile": "prod",
  "profiles": {
    "prod": {
      "server_url": "https://mappled.internal:8443",
      "bearer_token": "<the plaintext token from step 4>",
      "ca_file": "/home/alice/mappled.crt"
    }
  }
}
```

Multiple profiles can coexist in the same file (e.g. one per environment);
`mapple -profile <name>` selects one, otherwise `active_profile` is used.
Run `mapple` and you're in the TUI: `n` to create a session against a
module's `host:port` or chardev path, `enter` for detail, `i`/`e`/`p` for
info/echo/pslist, `/` to filter a list, `s` to cycle pslist sort order.

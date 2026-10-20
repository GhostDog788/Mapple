# Installing mappled and mapple

[Documentation](../README.md) / Guides

Install the Go server on a management host and the TUI on an operator's
workstation. For the local Ubuntu/QEMU lab, follow [setup](setup.md) instead.
This guide assumes the module's machine is already prepared and reachable.
See [architecture](../architecture/overview.md) for components, transports, and session ownership.
Run shell commands from the repository root unless indicated otherwise.

## 1. Build

On a fresh x86-64 Ubuntu 24.04 build host, install the bootstrap tools and clone:

```bash
sudo apt-get update
sudo apt-get install -y git make python3 golang-go build-essential openssl
git clone https://github.com/GhostDog788/Mapple.git
cd Mapple
```

An installed `go` executable is required. Make selects and downloads the toolchain
specified by `user/go.mod`; the destination machines only need the built binaries.
From the repository root:

```bash
make server-build tui-build
make check
```

The binaries are `.lab/builds/go/mappled` and `.lab/builds/go/mapple`. Both are built with
`CGO_ENABLED=0`; they do not need a Go installation on the destination machine.

## 2. Install the binaries

```
sudo install -m 0755 .lab/builds/go/mappled /usr/local/bin/mappled
sudo install -m 0755 .lab/builds/go/mapple  /usr/local/bin/mapple
```

`mapple` runs unprivileged on an operator's workstation. The installed `mappled`
service runs as root and opens the TCP destinations and local character-device
paths supplied by authenticated operators.

## 3. TLS certificate and key

`mappled` terminates HTTPS itself (see `user/cmd/mappled/main.go`) and refuses to
start without both `tls_cert_file` and `tls_key_file` configured
(`user/server/config.go`'s `LoadConfig`). Two realistic paths:

**Private management network, self-signed:** distribute the public certificate
to clients and set `ca_file` in each TUI profile (step 8):

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

For local development, follow the [setup guide](setup.md). `make server-run`
and `make tui-run` create matching TLS/authentication configurations per machine
under `.lab/runs/`; they do not use the production paths below.

## 4. Bearer tokens

`mappled` never stores plaintext tokens -- only a name and a SHA-256 digest
per identity (`user/server/config.go`'s `Identity`), compared in constant time.
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

Matches `user/server/config.go`'s `fileConfig`:

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
(default to 30 seconds and 15 minutes); `listen_address` and at
least one identity are required, and `LoadConfig` refuses to start without
both TLS paths. Restrict the file itself:

```
sudo chmod 600 /etc/mapple/mappled.json
sudo chown root:root /etc/mapple/mappled.json
```

Restart with `sudo systemctl restart mappled` after changing tokens or identities.

## 6. systemd service

The provided `user/packaging/systemd/mappled.service` uses restart-on-failure,
`NoNewPrivileges`, `ProtectSystem=strict`, and private temporary storage.
See the unit file for its permissions and filesystem restrictions.

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

Allow only designated `mappled` hosts to reach module TCP port 4444. Operators
and automation reach the server's HTTPS port on the private management network.

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
Run `mapple` and use `session create` to connect to a module by TCP or character
device. Use `command info`, `command echo <text>`, and `command pslist` after
connecting. `help` and `session help` list the current commands.

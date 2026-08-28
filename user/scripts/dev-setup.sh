#!/usr/bin/env bash
# Generate a self-signed TLS cert, a bearer token, and a mappled.json config
# under user/dev/ (gitignored) for local curl testing. Prints the plaintext
# token and ready-to-use curl examples; the token is never written to disk
# anywhere except your own terminal scrollback.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
user_dir="$(cd "$script_dir/.." && pwd)"
dev_dir="$user_dir/dev"
mkdir -p "$dev_dir"

listen_address="${1:-127.0.0.1:8443}"

openssl req -x509 -newkey rsa:2048 -nodes \
	-keyout "$dev_dir/mappled.key" -out "$dev_dir/mappled.crt" \
	-days 365 -subj "/CN=localhost" \
	-addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

token="$(openssl rand -hex 32)"
token_sha256="$(printf '%s' "$token" | openssl dgst -sha256 -hex | awk '{print $NF}')"

cat >"$dev_dir/mappled.json" <<EOF
{
  "listen_address": "$listen_address",
  "tls_cert_file": "$dev_dir/mappled.crt",
  "tls_key_file": "$dev_dir/mappled.key",
  "handshake_timeout_seconds": 30,
  "lease_timeout_minutes": 15,
  "identities": [
    { "name": "dev", "token_sha256": "$token_sha256" }
  ]
}
EOF

cat <<EOF
Wrote $dev_dir/mappled.json (cert/key alongside it).

Bearer token (save it now, it is not stored anywhere else):
  $token

Start the daemon:
  go run ./cmd/mappled -config $dev_dir/mappled.json

Example curl commands (self-signed cert, hence -k):
  curl -sk https://$listen_address/v1/health/live
  curl -sk -H "Authorization: Bearer $token" https://$listen_address/v1/status
  curl -sk -H "Authorization: Bearer $token" -H "Content-Type: application/json" \\
    -d '{"type":"tcp","host":"192.168.122.77","port":4444}' \\
    https://$listen_address/v1/sessions
EOF

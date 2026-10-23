#!/usr/bin/env bash
# Compatibility entry point. Credentials persist in .lab/dev and both the
# server and TUI configurations are generated together.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if (( $# )); then
	echo "Use make dev-config, then edit .lab/dev/mappled.json for a custom listen address." >&2
	exit 2
fi
exec python3 "$repo_root/scripts/lab.py" dev-config

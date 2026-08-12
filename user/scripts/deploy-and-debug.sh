#!/usr/bin/env bash
# Build the usermode client, upload it, and start gdbserver on <target> so
# VS Code (cppdbg) attaches directly to TARGET_IP:PORT (no tunnel). Writes
# user/.gdb/current-debug.gdb with the endpoint; launch.json sources it.
# Blocks while gdbserver runs; "Listening on port" gates the VS Code launch.
# Port: TARGET_USER_GDB_PORT in lab.local.env (default 2345; env override
# LAB_GDB_PORT).
set -euo pipefail

usage() {
	echo "usage: $0 <target> [prog-args...]" >&2
	exit 2
}

target="${1:-}"
if [[ $# -gt 0 ]]; then shift; fi
prog_args=("$@")

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
user_dir="$(cd "$script_dir/.." && pwd)"
repo_root="$(cd "$user_dir/.." && pwd)"

# shellcheck source=../../kernel/scripts/lib/common.sh
source "$repo_root/kernel/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target> [prog-args...]"

env_file="$(lab_env_file "$repo_root")"
# shellcheck source=/dev/null
source "$env_file"
lab_load_target "$target"

port="${LAB_GDB_PORT:-$(target_cfg "$target" USER_GDB_PORT)}"
port="${port:-2345}"
[[ "$port" =~ ^[0-9]+$ ]] ||
	die "TARGET_USER_GDB_PORT[$target] must be a port number, got '$port'"
endpoint="$LAB_TARGET_IP:$port"

# The binary name is user/Makefile's business; resolve it via build/current.
echo "[1/4] building usermode client"
make -C "$user_dir"
bin="$(readlink -f "$user_dir/build/current" 2>/dev/null || true)"
[[ -n "$bin" && -x "$bin" ]] ||
	die "build did not produce $user_dir/build/current (run make -C user and check BIN in user/Makefile)"
bin_name="$(basename "$bin")"

echo "[2/4] uploading $bin_name to $LAB_SSH_TARGET"
lab_check_connection
remote_bin="$LAB_REMOTE_DIR/$bin_name"
lab_ssh "mkdir -p '$LAB_REMOTE_DIR'"
lab_scp_to "$bin" "$remote_bin"
lab_ssh "chmod +x '$remote_bin'"

lab_ssh "command -v gdbserver >/dev/null 2>&1" ||
	die "gdbserver not found on $target; install it: ssh $LAB_SSH_TARGET sudo apt-get install -y gdbserver"

# Written before gdbserver starts so it exists when the launch sources it.
gdb_dir="$user_dir/.gdb"
mkdir -p "$gdb_dir"
gdb_file="$gdb_dir/$target.gdb"
echo "[3/4] writing $gdb_file (endpoint $endpoint)"
cat > "$gdb_file" <<EOF
set confirm off
set pagination off
set breakpoint pending on
set print pretty on
set tcp connect-timeout 60
set remotetimeout 60
target remote $endpoint
EOF
ln -sfn "$target.gdb" "$gdb_dir/current-debug.gdb"

# Args after <target> become the program's argv, quoted for the remote shell.
remote_args="'$remote_bin'"
for arg in "${prog_args[@]}"; do
	remote_args+=" '$arg'"
done

echo "[4/4] starting gdbserver on $target — GDB connects directly to $endpoint"
echo "      attach with: VS Code F5 'Usermode: remote (gdbserver)'"
echo "      or by hand:  gdb $bin -x $gdb_file"
echo

# sudo: /dev node is root-owned. Password via ssh stdin (never argv);
# gdbserver lives and dies with this blocking session.
printf '%s\n' "$LAB_SUDO_PASS" |
	SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" \
		"sudo -k -S -p '' gdbserver :$port $remote_args"

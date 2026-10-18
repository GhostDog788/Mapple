#!/usr/bin/env bash
# Non-mutating health check: ssh / sudo / os / kernel / headers / vmlinux /
# kernel-source / debug endpoints. Method arg optional (both checked if omitted).
set -euo pipefail

usage() {
	echo "usage: $0 <target> [kgdb|qemu]" >&2
	exit 2
}

target="${1:-}"
debug_method="${2:-}"
case "$debug_method" in ""|kgdb|qemu) ;; *) usage ;; esac

kernel_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "$kernel_root/.." && pwd)"
# shellcheck source=lib/common.sh
source "$kernel_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target> [kgdb|qemu]"

env_file="$(lab_env_file "$repo_root")"
# shellcheck source=/dev/null
source "$env_file"
lab_load_target "$target"
local_source="$(target_cfg "$target" LOCAL_SOURCE)"
local_vmlinux="$(target_cfg "$target" LOCAL_VMLINUX)"

if [[ -t 1 ]]; then
	GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; RESET='\033[0m'
else
	GREEN=''; RED=''; YELLOW=''; RESET=''
fi
ok()   { printf "  ${GREEN}OK${RESET}    %s\n" "$1"; }
fail() { printf "  ${RED}FAIL${RESET}  %s\n" "$1"; failures=$((failures+1)); }
warn() { printf "  ${YELLOW}WARN${RESET}  %s\n" "$1"; }
failures=0

echo "checking $LAB_TARGET ($LAB_SSH_TARGET:$LAB_SSH_PORT)"

# --- ssh ---
if lab_ssh 'true' 2>/dev/null; then
	ok "ssh: reachable"
else
	fail "ssh: cannot reach $LAB_SSH_TARGET on port $LAB_SSH_PORT"
	echo
	echo "fix one of:"
	echo "  - check host / ssh_port / user in .lab/slots/$target.json"
	echo "  - the target VM is up and openssh-server is running"
	echo "  - verify the slot's SSH key or password"
	exit 1
fi

# --- sudo ---
if lab_ssh_sudo 'true' 2>/dev/null; then
	ok "sudo: works"
else
	fail "sudo: configured password rejected or sudo not installed"
	echo
	echo "fix one of:"
	echo "  - TARGET_SUDO_PASS[$target] in lab.local.env (defaults to TARGET_SSH_PASS)"
	echo "  - configure NOPASSWD for the lab user on the target"
fi

# --- os ---
# shellcheck disable=SC2016  # $ID and $VERSION_CODENAME are intentionally evaluated on the remote.
remote_os="$(lab_ssh '. /etc/os-release 2>/dev/null && printf "%s %s" "$ID" "${VERSION_CODENAME:-unknown}"' || echo unknown)"
case "$remote_os" in
	"ubuntu "*) ok  "os: $remote_os" ;;
	*)          fail "os: $remote_os (only ubuntu targets are implemented)" ;;
esac

# --- kernel ---
live_kernel="$(lab_ssh 'uname -r' 2>/dev/null || echo unknown)"
ok "kernel: $live_kernel"

cache_dir="$kernel_root/kernel-cache/$target"
cached_kernel="$(cat "$cache_dir/kernel.release" 2>/dev/null || echo)"
if [[ -n "$cached_kernel" && "$cached_kernel" != "$live_kernel" ]]; then
	warn "cached kernel ($cached_kernel) differs from live; re-run kernel/scripts/02-setup-host-build.sh $target"
fi

# --- headers ---
if lab_ssh "test -d /lib/modules/$live_kernel/build" 2>/dev/null; then
	ok "headers: /lib/modules/$live_kernel/build present"
else
	warn "headers: missing on target; run kernel/scripts/01-provision-target.sh $target <method>"
fi

# --- vmlinux (debug image) ---
if [[ -n "$local_vmlinux" && -f "$local_vmlinux" ]]; then
	ok "vmlinux: matching local image configured"
elif lab_ssh_sudo "test -r /usr/lib/debug/boot/vmlinux-$live_kernel" 2>/dev/null; then
	ok "vmlinux: /usr/lib/debug/boot/vmlinux-$live_kernel readable (debug symbols installed)"
else
	warn "vmlinux: missing on target; for source debugging, re-run kernel/scripts/01-... with --debug-symbols"
fi

# --- kernel source ---
src_found="$(lab_ssh 'ls -1d /usr/src/linux-source-*/ 2>/dev/null | head -1 || true' 2>/dev/null)"
if [[ -n "$local_source" && -f "$local_source/init/main.c" ]]; then
	ok "kernel-source: matching local tree configured"
elif [[ -n "$src_found" ]]; then
	ok "kernel-source: ${src_found%/} present (step-into-kernel will resolve source)"
else
	warn "kernel-source: missing on target; for step-into-kernel, re-run kernel/scripts/01-... with --debug-symbols"
fi

# --- debug endpoint(s) reachable from this host ---
check_endpoint() {
	local label="$1" key="$2"
	local endpoint
	endpoint="$(target_cfg "$target" "$key")"
	if [[ -z "$endpoint" ]]; then
		warn "$label: TARGET_${key}[$target] is unset (skip if you don't use this method)"
		return
	fi
	local host="${endpoint%:*}" port="${endpoint##*:}"
	if command -v nc >/dev/null 2>&1; then
		if nc -z -w 3 "$host" "$port" 2>/dev/null; then
			ok "$label: $endpoint reachable from this host"
		else
			warn "$label: $endpoint NOT reachable from this host (start your GDB stub / serial bridge)"
		fi
	else
		warn "$label: cannot test (nc not installed); endpoint configured as $endpoint"
	fi
}

case "$debug_method" in
	kgdb) check_endpoint "debug-kgdb" DEBUG_ENDPOINT_KGDB ;;
	qemu) check_endpoint "debug-qemu" DEBUG_ENDPOINT_QEMU ;;
	"")
		check_endpoint "debug-kgdb" DEBUG_ENDPOINT_KGDB
		check_endpoint "debug-qemu" DEBUG_ENDPOINT_QEMU
		;;
esac

echo
if [[ $failures -eq 0 ]]; then
	echo "all checks passed."
	echo "next: make vm-setup (provision), or make run-kernel-debug (already prepared)"
else
	echo "$failures check(s) FAILED; resolve before running kernel/scripts/01."
	exit 1
fi

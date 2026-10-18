#!/usr/bin/env bash
# Shared helpers sourced by every numbered script. `lab_*` helpers need a
# prior `lab_load_target`; use them instead of raw ssh/scp binaries.

die() {
	echo "$*" >&2
	exit 1
}

validate_target() {
	local target="$1" usage="$2"
	[[ -n "$target" ]] || die "$usage"
	[[ "$target" =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]] ||
		die "invalid target '$target'; use letters, numbers, '_' or '-', starting with a letter"
}

target_is_configured() {
	local target="$1" known
	declare -p TARGETS >/dev/null 2>&1 ||
		die "missing TARGETS array in lab.local.env"
	for known in "${TARGETS[@]}"; do
		[[ "$known" == "$target" ]] && return 0
	done
	die "unknown target '$target'; add it to TARGETS in lab.local.env"
}

target_cfg() {
	local target="$1" key="$2" map_name="TARGET_$2"
	declare -p "$map_name" >/dev/null 2>&1 || return 0
	local -n map="$map_name"
	printf '%s' "${map[$target]:-}"
}

target_require_cfg() {
	local value
	value="$(target_cfg "$1" "$2")"
	[[ -n "$value" ]] || die "missing TARGET_$2[$1] in lab.local.env"
	printf '%s' "$value"
}

require_debian_host() {
	command -v apt-get >/dev/null 2>&1 ||
		die "the development host must be Debian-based (apt-get not found)"
}

# Callers `source` the returned path directly so TARGET_* assignments land
# in script-global scope.
lab_env_file() {
	local env_file="${MAPPLE_LAB_ENV:-$1/lab.local.env}"
	[[ -f "$env_file" ]] || die "missing lab configuration; run make setup, then make vm-add or make vm-create"
	printf '%s' "$env_file"
}

# Populate LAB_* globals from TARGET_* config. LAB_SSH_CMD / LAB_SCP_CMD are
# flag-only (no host) so callers can insert options before the host.
lab_load_target() {
	local target="$1"
	target_is_configured "$target"

	# shellcheck disable=SC2034  # consumed by sourcing scripts.
	LAB_TARGET="$target"
	LAB_TARGET_IP="$(target_require_cfg "$target" IP)"
	LAB_SSH_PORT="$(target_cfg "$target" SSH_PORT)"
	LAB_SSH_USER="$(target_require_cfg "$target" SSH_USER)"
	LAB_SSH_PASS="$(target_cfg "$target" SSH_PASS)"
	LAB_SUDO_PASS="$(target_cfg "$target" SUDO_PASS)"
	LAB_REMOTE_DIR="$(target_cfg "$target" REMOTE_DIR)"
	local ssh_key
	ssh_key="$(target_cfg "$target" SSH_KEY)"
	local known_hosts
	known_hosts="$(target_cfg "$target" KNOWN_HOSTS)"

	LAB_SSH_PORT="${LAB_SSH_PORT:-22}"
	LAB_SUDO_PASS="${LAB_SUDO_PASS:-$LAB_SSH_PASS}"
	LAB_REMOTE_DIR="${LAB_REMOTE_DIR:-/tmp/kmod-debug-lab-$LAB_SSH_USER}"

	local ssh_bin scp_bin sshpass_bin
	ssh_bin="${SSH_BIN:-ssh}"
	scp_bin="${SCP_BIN:-scp}"
	sshpass_bin="${SSHPASS_BIN:-sshpass}"

	if [[ -n "$LAB_SSH_PASS" ]] && ! command -v "$sshpass_bin" >/dev/null 2>&1; then
		die "TARGET_SSH_PASS[$target] is set but '$sshpass_bin' isn't installed; install with: sudo apt-get install -y sshpass"
	fi

	LAB_SSH_TARGET="$LAB_SSH_USER@$LAB_TARGET_IP"

	local ssh_opts=(
		-o StrictHostKeyChecking=accept-new
		-o ConnectTimeout=10
		-o ServerAliveInterval=15
	)
	if [[ -n "$ssh_key" ]]; then
		ssh_opts+=(-i "$ssh_key" -o IdentitiesOnly=yes)
	fi
	if [[ -n "$known_hosts" ]]; then
		ssh_opts+=(-o "UserKnownHostsFile=$known_hosts")
	fi
	if [[ -z "$LAB_SSH_PASS" ]]; then
		ssh_opts+=(-o BatchMode=yes)
	fi
	LAB_SSH_CMD=("$ssh_bin" "${ssh_opts[@]}" -p "$LAB_SSH_PORT")
	LAB_SCP_CMD=("$scp_bin" "${ssh_opts[@]}" -P "$LAB_SSH_PORT")
	printf -v LAB_RSYNC_RSH '%q ' "$ssh_bin" "${ssh_opts[@]}" -p "$LAB_SSH_PORT"
	if [[ -n "$LAB_SSH_PASS" ]]; then
		LAB_SSH_CMD=("$sshpass_bin" -e "${LAB_SSH_CMD[@]}")
		LAB_SCP_CMD=("$sshpass_bin" -e "${LAB_SCP_CMD[@]}")
		LAB_RSYNC_RSH="$sshpass_bin -e $LAB_RSYNC_RSH"
	fi
}

lab_ssh() {
	SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" "$@"
}

# Run a command on the target as root. The password reaches `sudo -k -S` via
# ssh's stdin (never in argv); `-k` forces the stdin read even with cached
# creds so the password can't leak into the next stdin reader. NOT safe for
# remote commands that read stdin themselves (bash -s) — use the
# SUDO_ASKPASS pattern in scripts/01 for those.
lab_ssh_sudo() {
	printf '%s\n' "$LAB_SUDO_PASS" |
		SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" "sudo -k -S -p '' $*"
}

lab_scp_to() {
	SSHPASS="$LAB_SSH_PASS" "${LAB_SCP_CMD[@]}" "$1" "$LAB_SSH_TARGET:$2"
}

# Extra rsync flags may be appended (e.g. --rsync-path='sudo rsync', --delete).
lab_rsync_from() {
	local remote_src="$1" local_dst="$2"
	shift 2
	SSHPASS="$LAB_SSH_PASS" "${RSYNC_BIN:-rsync}" -a -e "$LAB_RSYNC_RSH" "$@" \
		"$LAB_SSH_TARGET:$remote_src" "$local_dst"
}

# `--no-sudo` skips the sudo test (for scripts/01, which may be running TO
# configure sudo).
# shellcheck disable=SC2120
lab_check_connection() {
	local check_sudo=1
	[[ "${1:-}" == "--no-sudo" ]] && check_sudo=0

	if ! lab_ssh 'true' 2>/dev/null; then
		die "cannot reach $LAB_SSH_TARGET over ssh (port $LAB_SSH_PORT).
- check TARGET_IP / TARGET_SSH_PORT / TARGET_SSH_USER for '$LAB_TARGET' in lab.local.env
- try by hand: ssh -p $LAB_SSH_PORT $LAB_SSH_TARGET
- if using passwords, confirm TARGET_SSH_PASS[$LAB_TARGET] is set"
	fi
	if (( check_sudo )) && ! lab_ssh_sudo 'true' 2>/dev/null; then
		die "ssh reaches $LAB_SSH_TARGET but sudo doesn't work there.
- check TARGET_SUDO_PASS[$LAB_TARGET] in lab.local.env (defaults to TARGET_SSH_PASS)
- on the target, confirm: sudo -n -v"
	fi
}

#!/usr/bin/env bash
# Build the module against <target>'s synced headers via kernel/Makefile.
# Result: .ko under build/artifacts/<target>/<kernel>/.
set -euo pipefail

target="${1:-}"
kernel_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "$kernel_root/.." && pwd)"
# shellcheck source=lib/common.sh
source "$kernel_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target>"

cache_dir="$kernel_root/kernel-cache/$target"
kernel_file="$cache_dir/kernel.release"
kdir="$cache_dir/build"

[[ -f "$kernel_file" && -d "$kdir" ]] ||
	die "missing kernel cache for $target; run: kernel/scripts/02-setup-host-build.sh $target"

module_dir="$kernel_root"
[[ -e "$module_dir/Kbuild" ]] ||
	die "no Kbuild under $module_dir; the kernel module sources live in $module_dir/{src,include} with $module_dir/Kbuild"

kernel="$(<"$kernel_file")"
intermediate_dir="$kernel_root/build/intermediate/$target/$kernel"
artifact_dir="$kernel_root/build/artifacts/$target/$kernel"

echo "building $module_dir for $target kernel $kernel"
make -C "$kernel_root" \
	KDIR="$kdir" \
	BUILD_ID="$target/$kernel" \
	INTERMEDIATE_DIR="$intermediate_dir" \
	ARTIFACT_DIR="$artifact_dir" \
	MODULE_DIR="$module_dir" \
	clean modules

mapfile -t kos < <(find "$artifact_dir" -maxdepth 1 -name '*.ko' -type f | sort)
[[ ${#kos[@]} -gt 0 ]] || die "build produced no .ko under $artifact_dir; check the make output above"

for ko in "${kos[@]}"; do
	echo "built $ko"
done
echo "next: kernel/scripts/04-deploy-and-prep-gdb.sh $target <kgdb|qemu>"

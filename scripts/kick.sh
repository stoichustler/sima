#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
invoke_dir="$(pwd)"

qemu_bin="${QEMU_SYSTEM_AARCH64:-qemu-system-aarch64}"
kernel="${CLAN_KERNEL:-${repo_root}/build/acrn.out}"
smp="${CLAN_QEMU_SMP:-8}"
memory="${CLAN_QEMU_MEM:-1024M}"
dry_run=0
extra_args=()

usage() {
	cat <<EOF
Usage: $(basename "$0") [options] [-- extra-qemu-args]

Options:
  -k, --kernel PATH   Kernel image to boot. Default: build/acrn.out
  --qemu PATH         qemu-system-aarch64 binary. Default: qemu-system-aarch64
  --smp N             Number of QEMU CPUs. Default: 8
  -m, --memory SIZE   QEMU memory size. Default: 1024M
  -n, --dry-run       Print the QEMU command without executing it
  -h, --help          Show this help

Environment:
  QEMU_SYSTEM_AARCH64  qemu-system-aarch64 binary
  CLAN_KERNEL          kernel image path
  CLAN_QEMU_SMP        QEMU CPU count
  CLAN_QEMU_MEM        QEMU memory size
EOF
}

abs_path() {
	case "$1" in
	/*) printf '%s\n' "$1" ;;
	*) printf '%s/%s\n' "$invoke_dir" "$1" ;;
	esac
}

while (($# > 0)); do
	case "$1" in
	-k|--kernel)
		[[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 2; }
		kernel="$(abs_path "$2")"
		shift 2
		;;
	--qemu)
		[[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 2; }
		qemu_bin="$2"
		shift 2
		;;
	--smp)
		[[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 2; }
		smp="$2"
		shift 2
		;;
	-m|--memory)
		[[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 2; }
		memory="$2"
		shift 2
		;;
	-n|--dry-run)
		dry_run=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	--)
		shift
		extra_args+=("$@")
		break
		;;
	*)
		extra_args+=("$1")
		shift
		;;
	esac
done

if [[ ! -f "$kernel" ]]; then
	cat >&2 <<EOF
Kernel image not found: $kernel
Build it with:
  PATH=/home/beau/clan-arm64-none-elf/bin:\$PATH make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf-
EOF
	exit 1
fi

if ! command -v "$qemu_bin" >/dev/null 2>&1; then
	echo "QEMU binary not found: $qemu_bin" >&2
	exit 1
fi

cmd=(
	"$qemu_bin"
	-machine virt,virtualization=on,gic-version=3
	-cpu cortex-a57
	-smp "$smp"
	-m "$memory"
	-nographic
	-serial mon:stdio
	-kernel "$kernel"
	"${extra_args[@]}"
)

if [[ "$dry_run" -eq 1 ]]; then
	printf '%q ' "${cmd[@]}"
	printf '\n'
	exit 0
fi

cd "$repo_root"
exec "${cmd[@]}"

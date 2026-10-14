#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Boot HavenOS under QEMU with the self-test entry selected, capture the
# serial log, and exit non-zero if any check failed or the kernel did not
# finish.
#
# This is the only test that exercises the real boot path on a real CPU:
# the host suite in tests/host can attack an allocator far harder, but it
# cannot tell whether a context switch returns to the right stack or
# whether the timer interrupt actually preempts a spinning thread.
#
# Usage: scripts/run-selftest.sh [uefi]

set -eu

cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
TIMEOUT=${TIMEOUT:-120}
MODE=${1:-bios}
LOG=$(mktemp -t havenos-selftest.XXXXXX)

cleanup() { rm -f "$LOG"; }
trap cleanup EXIT

if ! command -v "$QEMU" >/dev/null 2>&1; then
	echo "  SKIP    $QEMU not installed; cannot run the in-kernel tests"
	exit 0
fi

echo "  BUILD   self-test image"
rm -f build/havenos-*.iso
GRUB_DEFAULT=3 make --no-print-directory iso >/dev/null

ISO=$(ls build/havenos-*.iso | head -1)
if [ ! -f "$ISO" ]; then
	echo "  FAIL    no ISO was produced"
	exit 1
fi

FIRMWARE=""
if [ "$MODE" = "uefi" ]; then
	OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
	if [ ! -f "$OVMF" ]; then
		echo "  SKIP    OVMF firmware not found at $OVMF"
		exit 0
	fi
	FIRMWARE="-bios $OVMF"
fi

echo "  BOOT    $MODE, up to ${TIMEOUT}s"
# shellcheck disable=SC2086
timeout "$TIMEOUT" "$QEMU" \
	-m 512 -smp "${CPUS:-1}" \
	-cdrom "$ISO" -boot d \
	-serial stdio -display none -no-reboot \
	$FIRMWARE > "$LOG" 2>&1 || true

# The kernel idles forever after the tests, so a timeout is expected and
# is not itself a failure.  What matters is whether the verdict appeared.
if grep -q "PASSED:" "$LOG"; then
	grep -E "selftest:.*(PASSED|FAILED|skipped)" "$LOG" | sed 's/^/  /'
	echo "  PASS    in-kernel self-tests passed ($MODE)"
	exit 0
fi

if grep -q "FAILED:" "$LOG"; then
	echo "  FAIL    in-kernel self-tests reported failures ($MODE):"
	grep -E "selftest:.*(FAIL|FAILED)" "$LOG" | sed 's/^/    /'
	exit 1
fi

echo "  FAIL    the kernel did not reach a self-test verdict ($MODE)."
echo "          Last 30 lines of the boot log:"
tail -30 "$LOG" | sed 's/^/    /'
exit 1

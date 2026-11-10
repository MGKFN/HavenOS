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
#
# Environment:
#   CPUS=n            number of processors (default 1)
#   DISK_IF=ide|virtio   which block driver the test disk hangs off
#   CPU_MODEL=name    QEMU -cpu model; `max` turns on SMEP and SMAP, which
#                     the default model lacks
#   TIMEOUT=seconds   give up after this long (default 120)

set -eu

cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
TIMEOUT=${TIMEOUT:-120}
MODE=${1:-bios}
LOG=$(mktemp -t havenos-selftest.XXXXXX)

# The self-test image has GRUB_DEFAULT=3 baked in, so leaving it behind
# makes it look like an ordinary up-to-date ISO to the next `make iso`.
# That is how a release once shipped an image that booted into the test
# suite.  It is a test artefact and it does not outlive the test.
cleanup() { rm -f "$LOG"; rm -f build/havenos-*.iso; }
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

# A partitioned, FAT-formatted disk, so the filesystem and block-layer
# tests have real media rather than reporting "skipped".  Built by mtools
# rather than by HavenOS: a FAT driver tested only against volumes it
# wrote itself will agree with its own mistakes.
DISK_IF=${DISK_IF:-ide}          # ide or virtio: which block driver is on trial
DISK="build/selftest-disk.img"
DISK_ARGS=""
if python3 tools/make_test_disk.py "$DISK" 64 >/dev/null 2>&1; then
	DISK_ARGS="-drive file=$DISK,format=raw,if=$DISK_IF"
	echo "  DISK    $DISK (MBR + FAT, written by mtools)"
else
	echo "  DISK    none (mtools unavailable); filesystem tests will skip"
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

echo "  BOOT    $MODE, ${CPUS:-1} CPU, disk on ${DISK_IF}, cpu model ${CPU_MODEL:-default}, up to ${TIMEOUT}s"

CPU_ARGS=""
[ -n "${CPU_MODEL:-}" ] && CPU_ARGS="-cpu $CPU_MODEL"

# shellcheck disable=SC2086
"$QEMU" \
	-m 512 -smp "${CPUS:-1}" $CPU_ARGS \
	-cdrom "$ISO" -boot d \
	-serial stdio -display none -no-reboot \
	$DISK_ARGS $FIRMWARE > "$LOG" 2>&1 < /dev/null &
QEMU_PID=$!

# The kernel idles forever after the tests, so nothing ever makes QEMU
# exit.  Waiting out the whole timeout cost two minutes per configuration
# for a suite that finishes in three seconds; watch for the verdict
# instead and stop the machine as soon as it appears.
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
	if grep -qE "PASSED:|FAILED:|KERNEL PANIC" "$LOG" 2>/dev/null; then
		sleep 1   # let the lines after the verdict reach the log
		break
	fi
	kill -0 "$QEMU_PID" 2>/dev/null || break
	sleep 1
	waited=$((waited + 1))
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

if grep -q "KERNEL PANIC" "$LOG"; then
	echo "  FAIL    the kernel panicked ($MODE).  Last 40 lines:"
	tail -40 "$LOG" | sed 's/^/    /'
	exit 1
fi

if grep -q "PASSED:" "$LOG"; then
	grep -aE "PASSED:|skipped" "$LOG" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/^/  /'
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

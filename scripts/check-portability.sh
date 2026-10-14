#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Enforce the rule the HAL exists for:
#
#   Nothing outside arch/<arch>/ and drivers/ may contain inline
#   assembly, architecture-conditional code, or an include of an
#   architecture-private header.
#
# Without a check, that rule decays.  HavenDOS had no HAL at all --
# include/io.h was included by filesystem code, which is why its sources
# could not be built for another architecture.  The ARM64 port skeleton in
# arch/arm64/ is the evidence that HavenOS's rule holds: the generic
# kernel compiles against it unchanged.
#
# Exit non-zero on any violation.  Run by `make check`.

set -u

cd "$(dirname "$0")/.."

fail=0

report() {
	echo "  VIOLATION  $1"
	fail=1
}

# Directories that must stay architecture-neutral.
GENERIC="kernel fs lib"

echo "portability: checking that $GENERIC contain no architecture-specific code"

# 1. Inline assembly.
#
# One exception is allowed and named: the atomic builtins and the
# compiler barrier in include/haven/compiler.h, which are compiler
# intrinsics rather than instructions.  Anything else means a subsystem
# has reached past the HAL.
for dir in $GENERIC; do
	[ -d "$dir" ] || continue
	hits=$(grep -rn '__asm__\|\basm\b' --include='*.c' --include='*.h' \
	       "$dir" 2>/dev/null | grep -v 'compiler\.h' || true)
	if [ -n "$hits" ]; then
		echo "$hits" | while IFS= read -r line; do
			report "inline assembly outside arch/: $line"
		done
		fail=1
	fi
done

# 2. Architecture-conditional compilation.
#
# vmlayout.h and elf.c are the two deliberate exceptions, and both are
# narrow: vmlayout.h selects which architecture's memlayout.h to forward,
# and elf.c picks the native ELF machine type.  Both are single switches
# over the architecture rather than architecture-specific logic.
for dir in $GENERIC; do
	[ -d "$dir" ] || continue
	hits=$(grep -rn '__x86_64__\|__i386__\|__aarch64__\|__arm__' \
	       --include='*.c' --include='*.h' "$dir" 2>/dev/null \
	       | grep -v 'vmlayout\.h' | grep -v 'proc/elf\.c' || true)
	if [ -n "$hits" ]; then
		echo "$hits" | while IFS= read -r line; do
			report "architecture conditional outside arch/: $line"
		done
		fail=1
	fi
done

# 3. Includes of architecture-private headers.
for dir in $GENERIC; do
	[ -d "$dir" ] || continue
	hits=$(grep -rn '#include <haven/arch/' --include='*.c' --include='*.h' \
	       "$dir" 2>/dev/null || true)
	if [ -n "$hits" ]; then
		echo "$hits" | while IFS= read -r line; do
			report "architecture-private header included outside arch/: $line"
		done
		fail=1
	fi
done

# 4. Port I/O and MSR access by name, which is x86-only even without
#    inline assembly -- a generic file calling outb() has reached past
#    the HAL just as surely as one containing `out %al`.
for dir in $GENERIC; do
	[ -d "$dir" ] || continue
	hits=$(grep -rnE '\b(outb|outw|outl|inb|inw|inl|rdmsr|wrmsr|rdtsc|cpuid_raw)\s*\(' \
	       --include='*.c' --include='*.h' "$dir" 2>/dev/null || true)
	if [ -n "$hits" ]; then
		echo "$hits" | while IFS= read -r line; do
			report "x86 port I/O outside arch/: $line"
		done
		fail=1
	fi
done

# 5. Every architecture must define the five memory-layout constants the
#    generic VMM needs, or kernel/mm/vmm.c will not build for it.
for arch in x86_64 arm64; do
	hdr="include/haven/arch/$arch/memlayout.h"
	if [ ! -f "$hdr" ]; then
		report "missing $hdr"
		continue
	fi
	for sym in HHDM_BASE KHEAP_BASE KHEAP_SIZE MMIO_BASE MMIO_SIZE \
	           KSTACK_BYTES USER_MIN USER_MAX; do
		if ! grep -q "define $sym" "$hdr"; then
			report "$arch/memlayout.h does not define $sym"
		fi
	done
done

# 6. Every function declared in the HAL must be defined by every
#    architecture.  A missing one is a link error for that architecture
#    only, which is exactly the kind of breakage that goes unnoticed
#    until somebody tries the other port.
# The pattern has to cope with every return type the HAL actually uses,
# not just the single-word ones.  It matched `void arch_init(` and
# `paddr_t arch_pt_translate(` but not `const char *arch_cpu_vendor(` or
# `struct arch_pagetable *arch_pt_create(`, so four entry points were
# silently exempt from this check -- which is the same class of bug the
# check exists to catch, one level up.  A declaration is now any run of
# identifiers, optionally followed by a `*`, then the name.
# The pattern has to cope with every return type the HAL actually uses.
# An earlier version matched `void arch_init(` and
# `paddr_t arch_pt_translate(` but not `const char *arch_cpu_vendor(` or
# `struct arch_pagetable *arch_pt_create(`, so four entry points were
# silently exempt from this check -- the same class of bug the check
# exists to catch, one level up.
#
# sed rather than grep -o, and the name is captured from immediately
# before the '(' rather than matched anywhere on the line: with grep -o
# the return type `struct arch_pagetable *` contributed a second match
# and the check then looked for a non-existent arch_pagetable().
missing=0
decls=$(sed -nE 's/^([a-z_0-9]+[ \t]+)+\*?[ \t]*(arch_[a-z_0-9]+)\(.*/\2/p' \
        include/haven/arch.h 2>/dev/null | sort -u)

for fn in $decls; do
	for arch in x86_64 arm64; do
		[ -d "arch/$arch" ] || continue
		if ! grep -rqE "\\b$fn\\b[ \t]*\\(" "arch/$arch" 2>/dev/null; then
			report "arch/$arch does not define $fn() from the HAL"
			missing=$((missing + 1))
		fi
	done
done

echo "portability: checked $(echo "$decls" | wc -w) HAL entry points across 2 architectures"

if [ "$fail" -eq 0 ]; then
	echo "  PASS       the generic kernel contains no architecture-specific code"
	exit 0
fi

echo "  FAIL       see the violations above"
exit 1

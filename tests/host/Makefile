# SPDX-License-Identifier: MIT
#
# HavenOS host test suite.
#
# Each test links the REAL kernel source it exercises — never a copy — so
# the test compiles against the code as it actually ships.  The
# architecture layer is satisfied by stubs.c, which simulates physical
# memory as one host allocation; that is possible only because the kernel
# reaches physical memory exclusively through arch_phys_to_virt().
#
# This is the practice HavenDOS established in its own tests/test_pmm.c
# and the reasoning it gave there still holds: a bug in the allocator
# hands out memory belonging to the kernel, which is worth testing
# directly rather than inferring from a successful boot.

TOP      := ../..
CC       ?= gcc
# -fno-builtin is essential, not cosmetic: without it GCC replaces calls
# to memcpy/strncpy/strlen with its own builtins (or constant-folds them
# outright), so the test would pass while never executing lib/string.c at
# all.  The whole value of linking the real source is lost silently.
#
# -fno-sanitize=address for the same reason: ASan's interceptors would
# replace our memcpy and memset.  UBSan is kept, since it instruments our
# code rather than substituting for it.
#
# -Wno-stringop-truncation / -Wno-stringop-overread / -Wno-format-truncation:
# these tests deliberately truncate and deliberately pass a bound longer
# than the source, because that is the behaviour under test.  The warnings
# are correct about the code and wrong about the intent.
#
# _FORTIFY_SOURCE=0 is the other half of the -fno-builtin point, and the
# more important half.  With fortification on, glibc's headers redirect
# snprintf() to __builtin___snprintf_chk() and strncpy() to
# __builtin___strncpy_chk() -- so a test that calls snprintf() would
# exercise GLIBC and report a pass while the kernel's own lib/printf.c
# never ran.  Turning it off makes the plain calls resolve to the kernel
# objects on the link line.
#
# The suite proves this at runtime rather than trusting the flags:
# test_printf checks %b, a HavenOS extension glibc does not implement, so
# if the wrong printf were linked that check would fail.
CFLAGS   := -std=gnu11 -O1 -g -I$(TOP)/include -I. \
            -Wall -Wextra -Werror \
            -Wno-unused-parameter \
            -Wno-stringop-truncation -Wno-stringop-overread \
            -Wno-format-truncation \
            -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
            -fno-strict-aliasing -fno-builtin \
            -fsanitize=undefined -fno-sanitize-recover=all \
            -fno-omit-frame-pointer
LDFLAGS  := -fsanitize=undefined

# Kernel sources under test, by suite.
KSRC_LIB   := $(TOP)/lib/string.c $(TOP)/lib/printf.c
KSRC_PMM   := $(TOP)/kernel/mm/pmm.c
KSRC_KMAL  := $(TOP)/kernel/mm/pmm.c $(TOP)/kernel/mm/kmalloc.c \
              $(TOP)/kernel/mm/vmm.c
KSRC_VFS   := $(TOP)/fs/vfs.c $(TOP)/fs/ramfs.c $(TOP)/kernel/sync/spinlock.c \
              $(TOP)/kernel/sync/mutex.c $(TOP)/kernel/errno.c
KSRC_FAT   := $(TOP)/fs/fat/fat.c
KSRC_ELF   := $(TOP)/kernel/proc/elf.c $(TOP)/kernel/errno.c
KSRC_CMDL  := $(TOP)/kernel/cmdline.c
KSRC_SCHED := $(TOP)/kernel/sched/sched_core.c

COMMON := stubs.c $(KSRC_LIB)

TESTS := test_string test_printf test_list test_cmdline test_pmm \
         test_vfs test_elf

# Suites are added here as the subsystems they cover land.
-include tests.mk

.PHONY: all run clean
all: $(TESTS)

test_string: test_string.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_printf: test_printf.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_list: test_list.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_cmdline: test_cmdline.c $(COMMON) $(KSRC_CMDL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_pmm: test_pmm.c $(COMMON) $(KSRC_PMM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Upper layers get stubs_mm.c instead of the real allocators: these tests
# are about path resolution and segment validation, and routing every
# allocation through a buddy allocator would add a second subject.
test_vfs: test_vfs.c $(COMMON) stubs_mm.c $(KSRC_VFS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_elf: test_elf.c $(COMMON) stubs_mm.c $(KSRC_ELF)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: all
	@fail=0; \
	for t in $(TESTS); do \
		echo ""; \
		echo "=============================================="; \
		./$$t || fail=1; \
	done; \
	echo ""; \
	if [ $$fail -eq 0 ]; then \
		echo "  all host test suites PASSED"; \
	else \
		echo "  host tests FAILED"; \
	fi; \
	exit $$fail

clean:
	rm -f $(TESTS) *.o

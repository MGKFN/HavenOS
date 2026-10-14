# AArch64 boot path

Empty on purpose. The x86_64 port's `boot/boot.S` exists to climb from
16-bit real mode through 32-bit protected mode to 64-bit long mode, and
**none of that applies here**: an AArch64 kernel is entered by firmware
already in 64-bit mode, at EL1 or EL2, with the MMU off and a device tree
or ACPI pointer in `x0`.

So the AArch64 boot path is short, and what it has to do instead is:

1. if entered at EL2, drop to EL1 (set `HCR_EL2.RW`, program `SPSR_EL2`
   and `ELR_EL2`, then `eret`) — firmware entry level varies by platform
   and a kernel that assumes one will fail on the other;
2. program `MAIR_EL1` **before** building any translation table, because
   a table entry's memory type is an index into it;
3. program `TCR_EL1` for a 4 KiB granule and a 48-bit range in both
   `TTBR0_EL1` and `TTBR1_EL1`;
4. build initial tables, set `SCTLR_EL1.M`, and `isb`;
5. set up a stack, zero `.bss`, and call `arm64_boot_main()` with the
   device-tree pointer.

Step 2 is the one that is easy to miss and hard to debug: with `MAIR_EL1`
unprogrammed, mappings get memory type Device-nGnRnE and every ordinary
memory access becomes uncached and strongly ordered. The kernel appears
to work and runs roughly a hundred times slower.

The equivalent of this project's `tools/verify_multiboot2.py` would be a
check that the image is a valid flat binary with the right entry
alignment, since AArch64 firmware loads a raw image rather than parsing
an ELF.

See `docs/PORTING_ARM64.md` for the order the whole port should be
tackled in.

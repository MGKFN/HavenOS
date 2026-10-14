#!/usr/bin/env python3
"""Verify the Multiboot2 header inside a built HavenOS kernel ELF.

A malformed Multiboot2 header does not fail the build — it fails the boot,
with GRUB printing a one-line complaint and no kernel output at all.  That
is an expensive way to find a checksum mistake, so the build runs this
check on every link.

Validates:
  * the header appears within the first 32 KiB of the file (spec requirement)
  * magic, architecture and header length
  * the checksum sums to zero mod 2^32
  * every tag is 8-byte aligned and inside the header
  * an entry-address tag exists and points inside a PT_LOAD segment's
    physical range, in the low 4 GiB where 32-bit GRUB can jump to it
  * the tag list is terminated by an end tag
"""
import struct
import sys

MB2_MAGIC = 0xE85250D6
MB2_SEARCH_LIMIT = 32768
TAG_NAMES = {
    0: "end", 1: "information request", 2: "address", 3: "entry address",
    4: "console flags", 5: "framebuffer", 6: "module alignment",
    7: "efi boot services", 8: "efi i386 entry", 9: "efi amd64 entry",
    10: "relocatable",
}


def load_segments(data):
    """Return [(file_off, filesz, vaddr, paddr, memsz)] for each PT_LOAD."""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("not a 64-bit ELF")
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    out = []
    for i in range(e_phnum):
        base = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", data, base)
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<QQQQQ", data, base + 0x08)
        out.append((p_offset, p_filesz, p_vaddr, p_paddr, p_memsz))
    return out


def main(path):
    with open(path, "rb") as fh:
        data = fh.read()

    errors, notes = [], []

    # The header must be 8-byte aligned within the first 32 KiB.
    offset = None
    for off in range(0, min(len(data), MB2_SEARCH_LIMIT) - 16, 8):
        if struct.unpack_from("<I", data, off)[0] == MB2_MAGIC:
            offset = off
            break
    if offset is None:
        raise SystemExit(
            f"FAIL: no Multiboot2 magic in the first {MB2_SEARCH_LIMIT} bytes")

    magic, arch, length, checksum = struct.unpack_from("<IIII", data, offset)
    notes.append(f"header at file offset 0x{offset:x}, length {length}")

    if arch != 0:
        errors.append(f"architecture is {arch}, expected 0 (i386 protected mode)")
    if length < 16 or offset + length > len(data):
        errors.append(f"header length {length} is out of range")
    if (magic + arch + length + checksum) & 0xFFFFFFFF != 0:
        errors.append(
            f"checksum 0x{checksum:08x} does not zero the sum "
            f"(got 0x{(magic + arch + length + checksum) & 0xFFFFFFFF:08x})")

    segments = load_segments(data)
    if not segments:
        errors.append("ELF has no PT_LOAD segment")

    # Walk the tags.
    pos = offset + 16
    end_seen = False
    entry_addr = None
    seen = []
    while pos < offset + length:
        if pos % 8:
            errors.append(f"tag at 0x{pos:x} is not 8-byte aligned")
            break
        t_type, t_flags, t_size = struct.unpack_from("<HHI", data, pos)
        name = TAG_NAMES.get(t_type, f"unknown({t_type})")
        seen.append(name)
        if t_size < 8:
            errors.append(f"{name} tag has size {t_size}, minimum is 8")
            break
        if pos + t_size > offset + length:
            errors.append(f"{name} tag at 0x{pos:x} runs past the header end")
            break
        if t_type == 0:
            end_seen = True
            break
        if t_type == 3:
            if t_size != 12:
                errors.append(f"entry address tag size {t_size}, expected 12")
            else:
                entry_addr, = struct.unpack_from("<I", data, pos + 8)
        pos += (t_size + 7) & ~7

    if not end_seen:
        errors.append("tag list is not terminated by an end tag")

    if entry_addr is None:
        errors.append(
            "no entry address tag: GRUB would jump to the ELF e_entry, "
            "which for a higher-half kernel is unreachable in 32-bit mode")
    else:
        notes.append(f"entry address 0x{entry_addr:08x}")
        if entry_addr >= 1 << 32:
            errors.append("entry address is above 4 GiB")
        inside = any(p <= entry_addr < p + m for _, _, _, p, m in segments)
        if not inside:
            errors.append(
                f"entry address 0x{entry_addr:08x} is not inside any PT_LOAD "
                "physical range")

    print(f"multiboot2: {path}")
    for n in notes:
        print(f"  note  {n}")
    print(f"  note  tags: {', '.join(seen)}")
    for _, _, v, p, m in segments:
        print(f"  note  PT_LOAD phys 0x{p:x}..0x{p + m:x} virt 0x{v:x}")
    for e in errors:
        print(f"  FAIL  {e}")

    if errors:
        return 1
    print("  ok    Multiboot2 header is valid")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_multiboot2.py <kernel.elf>")
    sys.exit(main(sys.argv[1]))

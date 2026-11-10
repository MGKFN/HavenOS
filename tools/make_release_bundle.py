#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bundle the current HavenOS release into one zip.

    python3 tools/make_release_bundle.py

Run `make release` first.  This collects what that produced, plus the
kernel and the documents that describe the state of the tree, into

    dist/HavenOS-<ver>-bundle.zip

so that one file carries the release into a new conversation or onto
another machine.  The version is read from include/haven/version.h and is
never typed here.

It refuses to build a bundle that would mislead:
  - no release archive or ISO for this version
  - the kernel on its own differs from the kernel inside the ISO
  - the ISO's default boot entry is not the normal one (a self-test image)
  - the changelog has no section for this version
  - HANDOFF.md does not name this version as current
"""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile
import zipfile

TOP = pathlib.Path(__file__).resolve().parent.parent


def die(msg: str) -> "None":
    print(f"  REFUSED {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def iso_extract(iso: pathlib.Path, inner: str) -> bytes:
    """Read one file out of the ISO with xorriso, without mounting it."""
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "member"
        r = subprocess.run(
            ["xorriso", "-osirrox", "on", "-indev", str(iso),
             "-extract", inner, str(out)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0 or not out.is_file():
            die(f"cannot read {inner} from {iso.name} (is xorriso installed?)")
        return out.read_bytes()


def main() -> int:
    header = (TOP / "include/haven/version.h").read_text()
    m = re.search(r'#define HAVENOS_VERSION\s+"([^"]+)"', header)
    c = re.search(r'#define HAVENOS_CODENAME\s+"([^"]+)"', header)
    if not m or not c:
        die("cannot read the version from include/haven/version.h")
    ver, codename = m.group(1), c.group(1)

    dist = TOP / "dist"
    rel = dist / f"havenos-{ver}"
    source_zip = dist / f"havenos-{ver}.zip"
    iso = rel / "image" / f"havenos-{ver}.iso"
    kernel = TOP / "build" / "havenos.elf"

    for p in (source_zip, iso, kernel):
        if not p.is_file():
            die(f"{p.relative_to(TOP)} is missing; run `make release` first")

    # The ISO must be the one a user wants: it once shipped booting
    # straight into the self-test suite.
    grub_cfg = iso_extract(iso, "/boot/grub/grub.cfg").decode("utf-8", "replace")
    default = re.search(r"^set default=(\S+)", grub_cfg, re.M)
    if not default or default.group(1) != "0":
        die(f"the ISO's default boot entry is {default.group(1) if default else '?'}, not 0")
    if ver not in grub_cfg:
        die(f"the ISO's boot menu does not mention version {ver}")

    # The loose kernel must be the kernel that is on the ISO.
    iso_kernel = iso_extract(iso, "/boot/havenos.elf")
    if hashlib.sha256(iso_kernel).hexdigest() != sha256(kernel):
        die("build/havenos.elf is not the kernel inside the ISO; "
            "rebuild with `make release`")

    changelog = (TOP / "CHANGELOG.md").read_text()
    if f"## [{ver}]" not in changelog:
        die(f"CHANGELOG.md has no section for {ver}")

    handoff = (TOP / "HANDOFF.md").read_text()
    if f"HavenOS {ver}" not in handoff:
        die(f"HANDOFF.md does not name {ver} as the current release")

    root = f"HavenOS-{ver}"
    readme = f"""# HavenOS {ver} "{codename}" — release bundle

Everything for this release in one place.

| File | What it is |
|---|---|
| `HANDOFF.md` | the state of the tree — read this first |
| `CHANGELOG.md` | what changed in every release |
| `RELEASE_NOTES.md` | this release in detail, including what is not verified |
| `HavenOS-{ver}.iso` | bootable image, BIOS and UEFI |
| `HavenOS-{ver}-source.zip` | the full source tree; builds on its own |
| `kernel/havenos.elf` | the kernel, the same bytes as on the ISO |
| `docs/AUDIT.md` | the technical audit |
| `docs/ROADMAP.md` | what comes next |
| `SHA256SUMS` | checksums of the files above |

## Try it

```sh
qemu-system-x86_64 -m 512 -cdrom HavenOS-{ver}.iso -boot d -serial stdio
```

Press Enter once, then:

```
haven:/$ run /bin/hello one two
```

## Starting a new chat

Attach this zip, or at least `HANDOFF.md` and `HavenOS-{ver}-source.zip`.
The handoff says what works, what does not, and what to do next.

Verified under QEMU only. Not yet booted on real hardware.
"""

    members = [
        ("HANDOFF.md", TOP / "HANDOFF.md"),
        ("CHANGELOG.md", TOP / "CHANGELOG.md"),
        ("RELEASE_NOTES.md", TOP / "RELEASE_NOTES.md"),
        (f"HavenOS-{ver}.iso", iso),
        (f"HavenOS-{ver}-source.zip", source_zip),
        ("kernel/havenos.elf", kernel),
        ("docs/AUDIT.md", TOP / "docs/AUDIT.md"),
        ("docs/ROADMAP.md", TOP / "docs/ROADMAP.md"),
    ]
    sums = "".join(f"{sha256(p)}  {name}\n" for name, p in members)

    out = dist / f"HavenOS-{ver}-bundle.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"{root}/README-FIRST.md", readme)
        z.writestr(f"{root}/SHA256SUMS", sums)
        for name, p in members:
            z.write(p, f"{root}/{name}")

    with zipfile.ZipFile(out) as z:
        if z.testzip() is not None:
            die("the bundle failed its own integrity check")
        count = len(z.namelist())

    print(f"  BUNDLE  {out.relative_to(TOP)}  "
          f"({count} files, {out.stat().st_size / (1 << 20):.1f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

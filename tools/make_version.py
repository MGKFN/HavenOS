#!/usr/bin/env python3
"""Generate version-bearing build artefacts from include/haven/version.h.

HavenDOS shipped a release whose configuration file reported VERSION=0.6.1
while the kernel was 0.7.18, and whose interface text still said "Beta",
because the version appeared as a literal in several files.  Its own
v0.7.18 Makefile added a generation step so that "version.txt and the GRUB
menu titles are generated from include/version.h so they cannot drift from
the running kernel again".

HavenOS keeps that rule from the first commit: this script is the only
thing permitted to write a version string into a build artefact.

usage: make_version.py <version.h> <template.in> <output> [version.txt]
"""
import datetime
import os
import re
import subprocess
import sys


def read_defines(path):
    """Pull the #define string and integer macros out of version.h."""
    text = open(path, encoding="utf-8").read()
    out = {}
    for name, value in re.findall(
            r'^#define\s+(HAVENOS_\w+)\s+"([^"]*)"', text, re.M):
        out[name] = value
    for name, value in re.findall(
            r'^#define\s+(HAVENOS_\w+)\s+(\d+)\s*$', text, re.M):
        out.setdefault(name, value)
    return out


def git_rev():
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
            check=True).stdout.strip() or "nogit"
    except (subprocess.SubprocessError, OSError):
        return "nogit"


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)

    version_h, template, output = argv[1], argv[2], argv[3]
    version_txt = argv[4] if len(argv) > 4 else None

    d = read_defines(version_h)
    missing = [k for k in ("HAVENOS_VERSION", "HAVENOS_CODENAME") if k not in d]
    if missing:
        raise SystemExit(f"{version_h}: missing {', '.join(missing)}")

    subs = {
        "VERSION": d["HAVENOS_VERSION"],
        "CODENAME": d["HAVENOS_CODENAME"],
        "BUILD_DATE": datetime.datetime.now(datetime.timezone.utc)
                              .strftime("%Y-%m-%d"),
        "BUILD_REV": git_rev(),
        "ARCH": os.environ.get("ARCH", "x86_64"),
    }

    text = open(template, encoding="utf-8").read()
    for key, value in subs.items():
        text = text.replace(f"@{key}@", value)

    # Any @TOKEN@ left over is a template referring to something this
    # script does not know how to fill, which would otherwise ship to the
    # ISO verbatim and be noticed by a user instead of by the build.
    leftover = sorted(set(re.findall(r"@[A-Z_]+@", text)))
    if leftover:
        raise SystemExit(
            f"{template}: unsubstituted placeholders {', '.join(leftover)}")

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"  GEN     {output} (version {subs['VERSION']})")

    if version_txt:
        os.makedirs(os.path.dirname(os.path.abspath(version_txt)),
                    exist_ok=True)
        with open(version_txt, "w", encoding="utf-8") as fh:
            fh.write(
                f"HavenOS {subs['VERSION']} \"{subs['CODENAME']}\"\n"
                f"arch {subs['ARCH']}\n"
                f"built {subs['BUILD_DATE']}\n"
                f"revision {subs['BUILD_REV']}\n")
        print(f"  GEN     {version_txt}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

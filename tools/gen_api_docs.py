#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# gen_api_docs.py -- generate docs/API.md from the kernel headers.
#
# DESIGN NOTE (HavenDOS -> HavenOS)
# HavenDOS had no API documentation.  What it had instead was a set of
# headers whose comments drifted away from the code, because nothing ever
# read them again after they were written.
#
# The lesson taken here is not "write a document" but "make the document a
# build product".  docs/API.md is generated from the headers by this
# script, so a declaration that changes without its comment changing is
# visible in the generated diff, and a function added without a comment
# appears in the generated file as an undocumented entry.  `make docs`
# regenerates it; the result is committed so it can be read on the web.
#
# The parser is deliberately simple-minded.  It understands the house
# style used throughout include/haven -- a block comment immediately above
# the thing it documents, a trailing comment on the same line as a short
# declaration, and box-drawing banners separating sections -- and nothing
# else.  It is not a C parser and does not try to be one: anything it
# cannot classify is listed at the end of the generated document rather
# than silently dropped, which is the property that matters.  An empty
# "could not classify" section is the goal, and the Makefile does not fail
# the build over it, because a parser bug should not block a kernel build.
#
# Usage: gen_api_docs.py <include-dir> <output.md>

import os
import re
import sys

BANNER_CHARS = set("-=_ ─━┄┅═")

# Headers are grouped so the generated document reads top-down rather than
# alphabetically.  A header missing from this table still appears, under
# "Other interfaces" -- a new header should be placed deliberately.
GROUPS = [
    ("Foundations", [
        "compiler.h", "types.h", "errno.h", "list.h", "string.h",
        "printf.h", "panic.h", "console.h", "version.h", "random.h",
    ]),
    ("Boot and platform", [
        "boot.h", "multiboot2.h", "cmdline.h", "arch.h", "vmlayout.h",
        "fault.h",
    ]),
    ("Memory", [
        "mm.h", "dma.h",
    ]),
    ("Execution", [
        "sched.h", "smp.h", "spinlock.h", "mutex.h", "irq.h", "time.h",
    ]),
    ("Processes and system calls", [
        "proc.h", "elf.h", "syscall.h",
    ]),
    ("Filesystems", [
        "vfs.h", "ramfs.h", "devfs.h", "fat.h",
    ]),
    ("Devices and drivers", [
        "device.h", "pci.h", "block.h", "ata.h", "virtio.h", "input.h",
        "fbcon.h",
    ]),
    ("Diagnostics", [
        "selftest.h", "shell.h",
    ]),
]

SKIP = {"font8x8.h"}          # a 2 KiB glyph table; nothing to document
ARCH_GROUP = "Architecture-private headers"

ATTR_RE = re.compile(
    r"\s*(?:HAVEN_NORETURN"
    r"|HAVEN_PRINTF\([^)]*\)"
    r"|HAVEN_PACKED"
    r"|HAVEN_ALIGNED\([^)]*\)"
    r"|HAVEN_SECTION\([^)]*\)"
    r"|HAVEN_USED|HAVEN_WEAK|HAVEN_UNUSED"
    r"|__attribute__\s*\(\(.*?\)\))")


# ------------------------------------------------------------------ model

class Entry:
    def __init__(self, kind, name, signature, doc, body=None, noreturn=False):
        self.kind = kind          # 'func' | 'type' | 'macro' | 'var'
        self.name = name
        self.signature = signature
        self.doc = doc            # list of paragraphs
        self.body = body          # full text, for aggregates and macros
        self.noreturn = noreturn


class Section:
    def __init__(self, title):
        self.title = title
        self.entries = []


class Header:
    def __init__(self, path, relpath):
        self.path = path
        self.relpath = relpath
        self.title = ""
        self.sections = []
        self.unparsed = []


# ----------------------------------------------------------------- parsing

def strip_inline_comments(raw):
    """Remove comments that follow code on the same line.

    A declaration with a short trailing comment is the most common form in
    these headers, and leaving the comment in the text stream makes the
    declaration look as if it never ended.  The comment is taken out of the
    code and remembered against the line it came from, so it can still be
    used as that declaration's documentation.  Comments that start their own
    line are left exactly where they are.
    """
    out, inline, origin = [], {}, []
    i, n = 0, len(raw)
    while i < n:
        line = raw[i]
        s = line.strip()
        if not s or s.startswith("/*") or s.startswith("*") or s.startswith("//"):
            out.append(line)
            origin.append(i)
            i += 1
            continue
        pos = line.find("/*")
        if pos < 0:
            out.append(line)
            origin.append(i)
            i += 1
            continue
        first = i
        code = line[:pos].rstrip()
        parts = [line[pos:]]
        while "*/" not in parts[-1] and i + 1 < n:
            i += 1
            parts.append(raw[i])
        last = parts[-1]
        end = last.find("*/")
        tail = last[end + 2:].strip() if end >= 0 else ""
        if end >= 0:
            parts[-1] = last[:end]
        text = " ".join(p.replace("/*", " ").replace("*/", " ").strip(" \t*")
                        for p in parts)
        out.append(code + ((" " + tail) if tail else ""))
        origin.append(first)
        inline[len(out) - 1] = " ".join(text.split())
        i += 1
    return out, inline, origin


def strip_comment(lines):
    """Turn a raw block comment into paragraphs of plain text."""
    out = []
    for ln in lines:
        ln = ln.strip()
        if ln.startswith("/*"):
            ln = ln[2:]
        if ln.endswith("*/"):
            ln = ln[:-2]
        ln = ln.lstrip("*").strip()
        out.append(ln)
    paras, cur = [], []
    for ln in out:
        if ln:
            cur.append(ln)
        elif cur:
            paras.append(" ".join(cur))
            cur = []
    if cur:
        paras.append(" ".join(cur))
    return paras


def is_banner(lines):
    if len(lines) != 1:
        return False
    text = lines[0].strip()
    if not (text.startswith("/*") and text.endswith("*/")):
        return False
    inner = text[2:-2]
    return len([c for c in inner if c in BANNER_CHARS]) >= 8


def banner_title(lines):
    inner = lines[0].strip()[2:-2]
    return inner.strip("".join(BANNER_CHARS)).strip()


def header_title(lines):
    """The 'name.h - description' line from the file's leading comment."""
    for i, ln in enumerate(lines):
        m = re.match(r"^\s*\*?\s*([A-Za-z0-9_./]+\.h)\s*[—-]+\s*(.*)$", ln)
        if m:
            text = m.group(2).strip()
            j = i + 1
            while not text.endswith(".") and j < len(lines):
                nxt = lines[j].strip().lstrip("*").strip()
                if not nxt:
                    break
                text += " " + nxt
                j += 1
            return text.rstrip()
        if i > 6:
            break
    return ""


FUNC_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w\s\*\(\),]*?)\b(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<args>.*)\)\s*;$")
AGG_RE = re.compile(r"^(?:typedef\s+)?(struct|union|enum)\s+(\w+)?[\w\s]*\{")
FWD_RE = re.compile(r"^(struct|union|enum)\s+(\w+)\s*;$")
MACRO_RE = re.compile(r"^#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)$")
TYPEDEF_RE = re.compile(r"^typedef\s+(.+?)\s*(\w+)\s*(\[[^\]]*\])?;$")
MACRO_DECL_RE = re.compile(r"^([A-Z][A-Z0-9_]+)\s*\((.*)\)\s*;$")
PP_SKIP_RE = re.compile(
    r"^#\s*(include|endif|if|ifdef|ifndef|else|elif|error|pragma|undef)\b")


def parse_header(path, relpath):
    hdr = Header(path, relpath)
    with open(path, encoding="utf-8") as fh:
        source = fh.read().split("\n")

    hdr.title = header_title(source[:30])
    raw, inline, origin = strip_inline_comments(source)

    section = Section("")
    hdr.sections.append(section)

    pending = []
    i, n = 0, len(raw)

    def doc_for(start, fallback_only=False):
        if pending and not fallback_only:
            return pending
        note = inline.get(start)
        return [note] if note else []

    while i < n:
        start = i
        line = raw[i]
        stripped = line.strip()

        if not stripped:
            pending = []
            i += 1
            continue

        # ---- block comments: either a section banner or a doc comment
        if stripped.startswith("/*"):
            block = [line]
            while "*/" not in block[-1] and i + 1 < n:
                i += 1
                block.append(raw[i])
            i += 1
            if is_banner(block):
                section = Section(banner_title(block))
                hdr.sections.append(section)
                pending = []
            else:
                pending = strip_comment(block)
            continue

        if stripped.startswith("//"):
            i += 1
            continue

        # ---- preprocessor
        if stripped.startswith("#"):
            text = stripped
            while text.endswith("\\") and i + 1 < n:
                i += 1
                text = text[:-1].rstrip() + " " + raw[i].strip()
            i += 1
            if PP_SKIP_RE.match(text):
                pending = []
                continue
            m = MACRO_RE.match(text)
            if m:
                name, args, value = m.group(1), m.group(2) or "", m.group(3).strip()
                if name.startswith("_HAVEN_") or name == "SYSCALL_DECLARE":
                    pending = []
                    continue
                section.entries.append(
                    Entry("macro", name, name + args, doc_for(start), value))
            pending = []
            continue

        # ---- aggregate definitions
        m = AGG_RE.match(stripped)
        if m:
            block = [line]
            depth = line.count("{") - line.count("}")
            while depth > 0 and i + 1 < n:
                i += 1
                block.append(raw[i])
                depth += raw[i].count("{") - raw[i].count("}")
            i += 1
            kw, name = m.group(1), m.group(2)
            if not name:
                tm = re.search(r"\}\s*(\w+)\s*;", block[-1])
                name = tm.group(1) if tm else "(anonymous)"
            o_end = (origin[i] - 1) if i < len(origin) else len(source) - 1
            body = [ln.rstrip() for ln in source[origin[start]:o_end + 1]]
            section.entries.append(
                Entry("type", name, "%s %s" % (kw, name), doc_for(start),
                      "\n".join(body)))
            pending = []
            continue

        # ---- a declaration, possibly spanning several lines
        decl = stripped
        while not decl.endswith(";") and not decl.endswith("{") and i + 1 < n:
            i += 1
            decl += " " + raw[i].strip()
        i += 1
        decl = re.sub(r"\s+", " ", decl).strip()

        noreturn = "HAVEN_NORETURN" in decl
        decl = ATTR_RE.sub("", decl).strip()

        # a static inline definition: keep the signature, skip the body
        if decl.endswith("{"):
            sig = decl[:-1].strip()
            depth = 1
            while depth > 0 and i < n:
                depth += raw[i].count("{") - raw[i].count("}")
                i += 1
            fm = FUNC_RE.match(sig + ";")
            if fm:
                section.entries.append(
                    Entry("func", fm.group("name"), sig, doc_for(start),
                          noreturn=noreturn))
            else:
                hdr.unparsed.append(sig)
            pending = []
            continue

        fw = FWD_RE.match(decl)
        if fw:
            section.entries.append(
                Entry("type", fw.group(2), decl[:-1], doc_for(start)))
            pending = []
            continue

        md = MACRO_DECL_RE.match(decl)
        if md and md.group(1) == "SYSCALL_DECLARE":
            name = md.group(2).strip()
            section.entries.append(
                Entry("func", "sys_" + name,
                      "int64_t sys_%s(const struct syscall_args *a)" % name,
                      doc_for(start)))
            pending = []
            continue

        if decl.startswith("typedef") and "(" not in decl:
            tm = TYPEDEF_RE.match(decl)
            if tm:
                section.entries.append(
                    Entry("type", tm.group(2), decl[:-1], doc_for(start)))
                pending = []
                continue

        fm = FUNC_RE.match(decl)
        if fm:
            section.entries.append(
                Entry("func", fm.group("name"), decl[:-1].strip(),
                      doc_for(start), noreturn=noreturn))
            pending = []
            continue

        if decl.startswith("extern") or re.match(r"^[A-Za-z_][\w\s\*\[\]]*;$", decl):
            name = re.sub(r"[^\w]+$", "", decl.split()[-1]).lstrip("*")
            section.entries.append(
                Entry("var", name, decl[:-1], doc_for(start)))
            pending = []
            continue

        if decl not in (";", ""):
            hdr.unparsed.append(decl)
        pending = []

    hdr.sections = [s for s in hdr.sections if s.entries]
    return hdr


# ------------------------------------------------------------------ output

def anchor(text):
    """GitHub's heading anchor: lowercase, drop punctuation, spaces to -.

    The headings are code spans (`haven/mm.h`), so the backticks, slash and
    dot all disappear and the anchor is `havenmmh`.  Getting this wrong
    produces a table of contents whose links all appear to work and none
    of which actually scroll anywhere, so it is worth being exact.
    """
    keep = "".join(c for c in text.lower()
                   if c.isalnum() or c in " -_")
    return keep.replace(" ", "-")


def emit_entry(out, e):
    if e.kind == "macro" and e.body and len(e.body) < 64 and "(" not in e.signature:
        out.append("**`%s`** = `%s`" % (e.signature, e.body))
    else:
        out.append("**`%s`**" % e.signature)
    out.append("")
    for para in e.doc:
        out.append(para)
        out.append("")
    if e.noreturn:
        out.append("Does not return.")
        out.append("")
    if e.kind == "type" and e.body:
        out.append("```c")
        out.extend(e.body.split("\n"))
        out.append("```")
        out.append("")


def emit_header(out, hdr, level):
    out.append("%s `%s`" % ("#" * level, hdr.relpath))
    out.append("")
    if hdr.title:
        out.append(hdr.title)
        out.append("")
    total = sum(len(s.entries) for s in hdr.sections)
    documented = sum(1 for s in hdr.sections for e in s.entries if e.doc)
    out.append("*%d declarations, %d carrying documentation.*"
               % (total, documented))
    out.append("")
    for s in hdr.sections:
        if s.title:
            out.append("%s %s" % ("#" * (level + 1), s.title))
            out.append("")
        for e in s.entries:
            emit_entry(out, e)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_api_docs.py <include-dir> <output.md>\n")
        return 2
    incdir, outfile = argv[1], argv[2]

    plain, arch = {}, {}
    for root, dirs, files in os.walk(incdir):
        dirs.sort()
        for f in sorted(files):
            if not f.endswith(".h"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, os.path.dirname(incdir))
            if os.sep + "arch" + os.sep in path:
                arch[rel] = path
            else:
                plain[f] = (rel, path)

    placed, groups = set(), []
    for title, names in GROUPS:
        items = []
        for nm in names:
            if nm in SKIP or nm not in plain:
                continue
            rel, path = plain[nm]
            items.append(parse_header(path, rel))
            placed.add(nm)
        if items:
            groups.append((title, items))

    leftovers = [parse_header(p, r) for nm, (r, p) in sorted(plain.items())
                 if nm not in placed and nm not in SKIP]
    if leftovers:
        groups.append(("Other interfaces", leftovers))

    arch_items = [parse_header(p, r) for r, p in sorted(arch.items())]
    all_headers = [h for _, items in groups for h in items] + arch_items

    nfunc = nmacro = ntype = nvar = ndoc = 0
    for h in all_headers:
        for s in h.sections:
            for e in s.entries:
                nfunc += e.kind == "func"
                nmacro += e.kind == "macro"
                ntype += e.kind == "type"
                nvar += e.kind == "var"
                ndoc += bool(e.doc)

    out = []
    out.append("# HavenOS kernel API reference")
    out.append("")
    out.append("Generated from the headers in `include/haven` by")
    out.append("`tools/gen_api_docs.py`; run `make docs` to regenerate it.")
    out.append("Edit the header comments, not this file.")
    out.append("")
    out.append("HavenOS has no stable user-space ABI yet, so every interface here")
    out.append("is kernel-internal and may change. The one interface meant to be")
    out.append("stable is the hardware abstraction layer in `haven/arch.h`: a port")
    out.append("to a new architecture implements exactly that set of functions and")
    out.append("nothing else, which is checked mechanically by")
    out.append("`scripts/check-portability.sh`. See")
    out.append("[PORTING_ARM64.md](PORTING_ARM64.md).")
    out.append("")
    out.append("| | Count |")
    out.append("|---|---|")
    out.append("| Headers | %d |" % len(all_headers))
    out.append("| Functions | %d |" % nfunc)
    out.append("| Types | %d |" % ntype)
    out.append("| Constants and macros | %d |" % nmacro)
    out.append("| Exported variables | %d |" % nvar)
    out.append("| Declarations with documentation | %d |" % ndoc)
    out.append("")
    out.append("## Contents")
    out.append("")
    for title, items in groups:
        out.append("- **%s** — %s" % (title, ", ".join(
            "[`%s`](#%s)" % (os.path.basename(h.relpath), anchor(h.relpath))
            for h in items)))
    if arch_items:
        out.append("- **%s** — %s" % (ARCH_GROUP, ", ".join(
            "[`%s`](#%s)" % (h.relpath, anchor(h.relpath)) for h in arch_items)))
    out.append("")

    for title, items in groups:
        out.append("## %s" % title)
        out.append("")
        for h in items:
            emit_header(out, h, 3)

    if arch_items:
        out.append("## %s" % ARCH_GROUP)
        out.append("")
        out.append("Reachable only from `arch/<arch>/` and from the architecture's")
        out.append("own code. Generic kernel code that includes one of these is a")
        out.append("portability bug, and `scripts/check-portability.sh` reports it.")
        out.append("")
        for h in arch_items:
            emit_header(out, h, 3)

    unparsed = [(h.relpath, u) for h in all_headers for u in h.unparsed]
    if unparsed:
        out.append("## Declarations this generator could not classify")
        out.append("")
        out.append("Listed rather than dropped, so no interface is missing from")
        out.append("this document without saying so.")
        out.append("")
        for rel, u in unparsed:
            out.append("- `%s`: `%s`" % (rel, u))
        out.append("")

    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out).rstrip()) + "\n"
    with open(outfile, "w", encoding="utf-8") as fh:
        fh.write(text)

    sys.stderr.write(
        "gen_api_docs: %s (%d headers, %d functions, %d types, %d macros, "
        "%d unclassified)\n"
        % (outfile, len(all_headers), nfunc, ntype, nmacro, len(unparsed)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Extract top-level library documentation from lib/*.ir4 into docs/lib.md."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
LIB_DIR = ROOT / "lib"
OUT_PATH = ROOT / "docs" / "lib.md"

ORDER = [
    "stdio.ir4",
    "mem.ir4",
    "math.ir4",
    "i32.ir4",
    "gfx.ir4",
    "sprite.ir4",
    "sfx.ir4",
    "joy.ir4",
]

FN_RE = re.compile(r"^((?:(?:default|hot)\s+)*)fn\s+(\S+)\s*\(([^)]*)\)")


def parse_lib(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    header = []
    i = 0
    while i < len(lines) and lines[i].startswith("\\"):
        header.append(lines[i][1:].strip())
        i += 1

    entries = []
    pending = []
    for line in lines[i:]:
        stripped = line.strip()
        if stripped.startswith("\\"):
            pending.append(stripped[1:].strip())
            continue
        m = FN_RE.match(stripped)
        if m:
            name, sig = m.group(2), m.group(3).strip()
            if not name.startswith("__"):
                doc = " ".join(pending)
                sig_str = f"**{name}** ( {sig} )" if sig else f"**{name}** ( )"
                entries.append((name, sig_str, doc))
            pending = []
            continue
        if stripped == "":
            pending = []
        elif not stripped.startswith(("var ", "data ", "require ", "fwd ")):
            pending = []
    return header, entries


def strip_mod_prefix(line: str, name: str) -> str:
    for pfx in (f"lib/{name} - ", f"{name} - "):
        if line.startswith(pfx):
            return line[len(pfx):]
    return line


def main():
    files = [LIB_DIR / name for name in ORDER if (LIB_DIR / name).exists()]
    extra = sorted(p for p in LIB_DIR.glob("*.ir4") if p.name not in ORDER)
    files.extend(extra)

    out = [
        "# IR4th Standard Library Reference",
        "",
        "> Generated from `lib/*.ir4` by `tools/gen_lib_docs.py`.",
        "",
    ]

    parsed = [(p, *parse_lib(p)) for p in files]
    out.append("## Modules")
    out.append("")
    for p, header, _ in parsed:
        anchor = p.name.replace(".", "")
        summary = strip_mod_prefix(header[0], p.name) if header else ""
        out.append(f"- [lib/{p.name}](#{anchor}) — {summary}")
    out.append("")

    for p, header, entries in parsed:
        out.append(f"## lib/{p.name}")
        out.append("")
        body_hdr = [strip_mod_prefix(h, p.name) for h in header]
        if body_hdr:
            out.extend(body_hdr)
            out.append("")
        out.append("| Function | Description |")
        out.append("| --- | --- |")
        for _, sig_md, doc in entries:
            doc_escaped = doc.replace("|", "\\|")
            out.append(f"| {sig_md} | {doc_escaped} |")
        out.append("")

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text("\n".join(out), encoding="utf-8")
    print(f"Wrote {OUT_PATH.relative_to(ROOT)} ({len(parsed)} modules)")


if __name__ == "__main__":
    main()

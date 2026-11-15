#!/usr/bin/env python3
"""Generate src/be_z80_math48.h from tools/math48.asm.

Prepends an 8-entry jump table, assembles at 0x0000 and 0x0100 to detect all
16-bit internal relocations, and emits a self-contained C89 header preserving
the original math48.asm header comments.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

WRAPPER_HEAD = """\
    JP FPADD
    JP FPSUB
    JP FPMUL
    JP FPDIV
    JP FPNEG
    JP CMP
    JP FLOAT
    JP FIX
    INCLUDE "math48.asm"
"""


def find_assembler(preferred=None):
    candidates = [preferred] if preferred else ["sjasmplus", "pasmo", "z80asm", "zmac"]
    for name in candidates:
        if name and shutil.which(name):
            return name
    return None


def extract_header_comments(asm_text):
    comments = []
    for line in asm_text.splitlines():
        stripped = line.strip()
        if not stripped:
            if comments:
                break
            continue
        if stripped.startswith(";"):
            comments.append(stripped.lstrip(";").strip())
        else:
            break
    return comments


def assemble(asm_tool, wrapper_path, inc_dir, org, out_bin, tmpdir):
    src_copy = os.path.join(tmpdir, f"rt_{org:04x}.asm")
    with open(wrapper_path, "r") as f:
        body = f.read()
    with open(src_copy, "w") as f:
        f.write(f"    ORG 0x{org:04X}\n")
        f.write(body)

    if asm_tool == "sjasmplus":
        subprocess.run([asm_tool, f"-I{inc_dir}", f"--raw={out_bin}", src_copy], check=True)
    elif asm_tool == "pasmo":
        subprocess.run([asm_tool, "-I", inc_dir, src_copy, out_bin], check=True)
    elif asm_tool == "z80asm":
        subprocess.run([asm_tool, f"-I{inc_dir}", f"-o{out_bin}", src_copy], check=True)
    elif asm_tool == "zmac":
        zout = os.path.join(tmpdir, "zout")
        subprocess.run([asm_tool, "-I", inc_dir, "--oo", "cim", "-o", out_bin, src_copy], check=True)
        if not os.path.exists(out_bin) and os.path.exists(os.path.join(zout, os.path.basename(out_bin))):
            shutil.copyfile(os.path.join(zout, os.path.basename(out_bin)), out_bin)
    else:
        raise RuntimeError(f"Unsupported assembler: {asm_tool}")

    with open(out_bin, "rb") as f:
        return f.read()


def compute_relocs(bin0, bin1):
    if len(bin0) != len(bin1):
        raise RuntimeError(f"Binary size mismatch: {len(bin0)} vs {len(bin1)}")
    relocs = []
    i = 0
    while i < len(bin0):
        if bin0[i] != bin1[i]:
            if i == 0 or bin1[i] != ((bin0[i] + 1) & 0xFF):
                raise RuntimeError(
                    f"Unexpected diff at offset 0x{i:04X}: 0x{bin0[i]:02X} vs 0x{bin1[i]:02X}"
                )
            relocs.append(i - 1)
        i += 1
    return relocs


def write_header(out_path, input_path, bin0, relocs, header_comments):
    lines = [
        "/*",
        " * Relocatable Z80 48-bit floating-point runtime (math48) for IR4th.",
        f" * Auto-generated from {input_path} by tools/gen_math48.py (do not edit manually).",
        " * Regenerate with: make rt-z80",
    ]
    if header_comments:
        lines.append(" *")
        lines.append(" * Original math48.asm header:")
        for c in header_comments:
            lines.append(f" *   {c}" if c else " *")
    lines.extend([
        " */",
        "#ifndef BE_Z80_MATH48_H",
        "#define BE_Z80_MATH48_H",
        "",
        "/* Jump-table offsets (3 bytes per JP entry) */",
        "enum {",
        "    RT_FP_ADD  = 0x00,",
        "    RT_FP_SUB  = 0x03,",
        "    RT_FP_MUL  = 0x06,",
        "    RT_FP_DIV  = 0x09,",
        "    RT_FP_NEG  = 0x0C,",
        "    RT_FP_CMP  = 0x0F,",
        "    RT_FP_ITOF = 0x12,",
        "    RT_FP_FTOI = 0x15",
        "};",
        "",
        f"static const u8 be_z80_math48[{len(bin0)}] = {{",
    ])

    for off in range(0, len(bin0), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in bin0[off : off + 12])
        comma = "," if off + 12 < len(bin0) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")

    if relocs:
        lines.append(f"static const u16 be_z80_math48_relocs[{len(relocs)}] = {{")
        for off in range(0, len(relocs), 10):
            chunk = ", ".join(f"0x{r:04X}" for r in relocs[off : off + 10])
            comma = "," if off + 10 < len(relocs) else ""
            lines.append(f"    {chunk}{comma}")
        lines.append("};")
    else:
        lines.append("static const u16 be_z80_math48_relocs[1] = { 0 };")

    lines.append(f"#define BE_Z80_MATH48_RELOC_CNT {len(relocs)}")
    lines.append("")
    lines.append("#endif /* BE_Z80_MATH48_H */")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="Generate Z80 math48 C header.")
    parser.add_argument("--input", default="tools/math48.asm", help="Path to math48.asm")
    parser.add_argument("--output", default="src/be_z80_math48.h")
    parser.add_argument("--asm", default=None, help="Assembler binary (sjasmplus, pasmo, z80asm, zmac)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"Missing {args.input}")

    asm_tool = find_assembler(args.asm)
    if not asm_tool:
        sys.exit("No Z80 assembler found (install sjasmplus, pasmo, z80asm, or zmac)")

    with open(args.input, "r") as f:
        asm_text = f.read()
    header_comments = extract_header_comments(asm_text)
    inc_dir = os.path.dirname(os.path.abspath(args.input))

    with tempfile.TemporaryDirectory() as tmpdir:
        wrapper_path = os.path.join(tmpdir, "wrapper.asm")
        with open(wrapper_path, "w") as f:
            f.write(WRAPPER_HEAD)

        b0 = assemble(asm_tool, wrapper_path, inc_dir, 0x0000, os.path.join(tmpdir, "b0.bin"), tmpdir)
        b1 = assemble(asm_tool, wrapper_path, inc_dir, 0x0100, os.path.join(tmpdir, "b1.bin"), tmpdir)

    relocs = compute_relocs(b0, b1)
    write_header(args.output, args.input, b0, relocs, header_comments)
    print(f"Generated {args.output}: {len(b0)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()

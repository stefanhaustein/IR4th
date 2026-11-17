#!/usr/bin/env python3
"""Generate relocatable 6502 32-bit integer C header from tools/i32_6502.asm."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from rt_util import compute_relocs


def assemble(input_path, org, tmpdir):
    if not shutil.which("ca65") or not shutil.which("ld65"):
        sys.exit("ca65 and ld65 (cc65) are required to build i32_6502")

    obj_path = os.path.join(tmpdir, f"i32_{org:04x}.o")
    cfg_path = os.path.join(tmpdir, f"i32_{org:04x}.cfg")
    bin_path = os.path.join(tmpdir, f"i32_{org:04x}.bin")

    with open(cfg_path, "w") as f:
        f.write(
            f"MEMORY {{\n    RAM: start = ${org:04X}, size = $2000, fill = no, file = %O;\n}}\n"
            "SEGMENTS {\n    CODE: load = RAM, type = ro;\n}\n"
        )

    subprocess.run(["ca65", input_path, "-o", obj_path], check=True)
    subprocess.run(["ld65", "-C", cfg_path, obj_path, "-o", bin_path], check=True)

    with open(bin_path, "rb") as f:
        return bytearray(f.read())


def main():
    parser = argparse.ArgumentParser(description="Generate 6502 i32 C header.")
    parser.add_argument("--input", default="tools/i32_6502.asm")
    parser.add_argument("--output", default="src/be_6502_i32.h")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmpdir:
        b0 = assemble(args.input, 0x4000, tmpdir)
        b1 = assemble(args.input, 0x4100, tmpdir)

    relocs = compute_relocs(b0, b1)
    for r in relocs:
        b0[r + 1] = (b0[r + 1] - 0x40) & 0xFF

    lines = [
        "/*",
        " * Relocatable 6502 32-bit integer runtime for IR4th.",
        f" * Auto-generated from {args.input} by tools/gen_i32_6502.py (do not edit manually).",
        " */",
        "#ifndef BE_6502_I32_H",
        "#define BE_6502_I32_H",
        "",
        "enum {",
        "    RT_I32_MUL     = 0x00,",
        "    RT_I32_UDIVMOD = 0x03,",
        "    RT_I32_SDIVMOD = 0x06,",
        "    RT_I32_SHL     = 0x09,",
        "    RT_I32_SHR     = 0x0C,",
        "    RT_I32_ASHR    = 0x0F",
        "};",
        "",
        "enum {",
        "    I32_ZP_A = 0x40,",
        "    I32_ZP_B = 0x44,",
        "    I32_ZP_R = 0x48",
        "};",
        "",
        f"static const u8 be_6502_i32[{len(b0)}] = {{",
    ]
    for off in range(0, len(b0), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in b0[off : off + 12])
        comma = "," if off + 12 < len(b0) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")
    if relocs:
        lines.append(f"static const u16 be_6502_i32_relocs[{len(relocs)}] = {{")
        for off in range(0, len(relocs), 10):
            chunk = ", ".join(f"0x{r:04X}" for r in relocs[off : off + 10])
            comma = "," if off + 10 < len(relocs) else ""
            lines.append(f"    {chunk}{comma}")
        lines.append("};")
        lines.append(f"#define BE_6502_I32_RELOC_CNT {len(relocs)}")
    else:
        lines.append("static const u16 be_6502_i32_relocs[1] = { 0 };")
        lines.append("#define BE_6502_I32_RELOC_CNT 0")
    lines.append("")
    lines.append("#endif")

    with open(args.output, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Generated {args.output}: {len(b0)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()

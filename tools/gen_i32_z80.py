#!/usr/bin/env python3
"""Generate relocatable Z80 32-bit integer C header from tools/i32_z80.asm."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from rt_util import compute_relocs


def assemble(input_path, org, tmpdir):
    if not shutil.which("z80asm"):
        sys.exit("z80asm is required to build i32_z80")

    with open(input_path, "r") as f:
        src = f.read()

    src_path = os.path.join(tmpdir, f"i32_{org:04x}.asm")
    bin_path = os.path.join(tmpdir, f"i32_{org:04x}.bin")
    with open(src_path, "w") as f:
        f.write(f"    ORG 0x{org:04X}\n" + src)

    subprocess.run(["z80asm", f"-o{bin_path}", src_path], check=True)
    with open(bin_path, "rb") as f:
        return bytearray(f.read())


def main():
    parser = argparse.ArgumentParser(description="Generate Z80 i32 C header.")
    parser.add_argument("--input", default="tools/i32_z80.asm")
    parser.add_argument("--output", default="src/be_z80_i32.h")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmpdir:
        b0 = assemble(args.input, 0x4000, tmpdir)
        b1 = assemble(args.input, 0x4100, tmpdir)

    relocs = compute_relocs(b0, b1)
    for r in relocs:
        b0[r + 1] = (b0[r + 1] - 0x40) & 0xFF

    lines = [
        "/*",
        " * Relocatable Z80 32-bit integer runtime for IR4th.",
        f" * Auto-generated from {args.input} by tools/gen_i32_z80.py (do not edit manually).",
        " */",
        "#ifndef BE_Z80_I32_H",
        "#define BE_Z80_I32_H",
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
        "    I32_OFF_A = 0x12,",
        "    I32_OFF_B = 0x16,",
        "    I32_OFF_R = 0x1A",
        "};",
        "",
        f"static const u8 be_z80_i32[{len(b0)}] = {{",
    ]
    for off in range(0, len(b0), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in b0[off : off + 12])
        comma = "," if off + 12 < len(b0) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")
    if relocs:
        lines.append(f"static const u16 be_z80_i32_relocs[{len(relocs)}] = {{")
        for off in range(0, len(relocs), 10):
            chunk = ", ".join(f"0x{r:04X}" for r in relocs[off : off + 10])
            comma = "," if off + 10 < len(relocs) else ""
            lines.append(f"    {chunk}{comma}")
        lines.append("};")
        lines.append(f"#define BE_Z80_I32_RELOC_CNT {len(relocs)}")
    else:
        lines.append("static const u16 be_z80_i32_relocs[1] = { 0 };")
        lines.append("#define BE_Z80_I32_RELOC_CNT 0")
    lines.append("")
    lines.append("#endif")

    with open(args.output, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Generated {args.output}: {len(b0)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate relocatable Z80 48-bit math48 C header from tools/math48.asm."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from rt_util import compute_relocs, extract_header_comments, write_rt_header

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


def find_assembler(preferred):
    candidates = [preferred] if preferred else ["sjasmplus", "pasmo", "z80asm", "zmac"]
    for name in candidates:
        if name and shutil.which(name):
            return name
    return None


def assemble(asm_tool, inc_dir, org, tmpdir):
    src_copy = os.path.join(tmpdir, f"src_{org:04x}.asm")
    out_bin = os.path.join(tmpdir, f"out_{org:04x}.bin")
    with open(src_copy, "w") as f:
        f.write(f"    ORG 0x{org:04X}\n" + WRAPPER_HEAD)

    if asm_tool == "sjasmplus":
        subprocess.run([asm_tool, f"-I{inc_dir}", f"--raw={out_bin}", src_copy], check=True)
    elif asm_tool == "pasmo":
        subprocess.run([asm_tool, "-I", inc_dir, src_copy, out_bin], check=True)
    elif asm_tool == "z80asm":
        subprocess.run([asm_tool, f"-I{inc_dir}", f"-o{out_bin}", src_copy], check=True)
    elif asm_tool == "zmac":
        zout = os.path.join(tmpdir, "zout")
        subprocess.run([asm_tool, f"-I{inc_dir}", "--oo", "cim", "-o", out_bin, src_copy], check=True)
        if not os.path.exists(out_bin) and os.path.exists(os.path.join(zout, os.path.basename(out_bin))):
            shutil.copyfile(os.path.join(zout, os.path.basename(out_bin)), out_bin)
    else:
        raise RuntimeError(f"Unsupported Z80 assembler: {asm_tool}")

    with open(out_bin, "rb") as f:
        return bytearray(f.read())


def main():
    parser = argparse.ArgumentParser(description="Generate Z80 math48 C header.")
    parser.add_argument("--input", default="tools/math48.asm")
    parser.add_argument("--output", default="src/be_z80_math48.h")
    parser.add_argument("--asm", default=None, help="Z80 assembler (sjasmplus, pasmo, z80asm, zmac)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"Missing {args.input}")

    asm_tool = find_assembler(args.asm)
    if not asm_tool:
        sys.exit("No Z80 assembler found (install sjasmplus, pasmo, z80asm, or zmac)")

    with open(args.input, "r") as f:
        header_comments = extract_header_comments(f.read())

    inc_dir = os.path.dirname(os.path.abspath(args.input))
    with tempfile.TemporaryDirectory() as tmpdir:
        b0 = assemble(asm_tool, inc_dir, 0x0000, tmpdir)
        b1 = assemble(asm_tool, inc_dir, 0x0100, tmpdir)

    relocs = compute_relocs(b0, b1)
    write_rt_header(
        args.output,
        args.input,
        b0,
        relocs,
        header_comments,
        guard="BE_Z80_MATH48_H",
        sym="be_z80_math48",
        rel_sym="be_z80_math48_relocs",
        rel_cnt_macro="BE_Z80_MATH48_RELOC_CNT",
        desc="Relocatable Z80 48-bit floating-point runtime (math48)",
        gen_script="tools/gen_math48.py",
        regen_cmd="make rt-z80",
    )
    print(f"Generated {args.output}: {len(b0)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()

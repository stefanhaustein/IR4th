#!/usr/bin/env python3
"""Generate relocatable 6502 32-bit wozfp C header from tools/wozfp.asm."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from rt_util import compute_relocs, extract_header_comments, write_rt_header

WOZFP_ZP_BASE = 0x40

WOZFP_JUMP_TABLE = """\
    JMP FADD
    JMP FSUB
    JMP FMUL
    JMP FDIV
    JMP FNEG
    JMP FCMP
    JMP FLOAT
    JMP FIX
"""

WOZFP_HELPERS = """\
FNEG:
    JMP FCOMPL

; Compare EXP/MANT2 vs EXP/MANT1 via FSUB (EXP/MANT2 - EXP/MANT1 -> EXP/MANT1).
; Returns A = $FF if < 0, A = 0 if == 0, A = 1 if > 0.
FCMP:
    JSR FSUB
    LDA M1
    BEQ @chk_zero
    BMI @neg
    LDA #1
    RTS
@neg:
    LDA #$FF
    RTS
@chk_zero:
    LDA M1+1
    ORA M1+2
    BNE @pos
    RTS
@pos:
    LDA #1
    RTS
"""


def assemble(input_path, org, tmpdir):
    if not shutil.which("ca65") or not shutil.which("ld65"):
        sys.exit("ca65 and ld65 (cc65) are required to build wozfp")

    with open(input_path, "r") as f:
        raw_lines = f.read().splitlines()

    body = []
    for line in raw_lines:
        s = line.strip()
        if s.startswith(".org ") or s.startswith(".export ") or s.startswith(".include ") or s.startswith(".res "):
            continue
        if "SIGN = *" in line:
            body.append(f"SIGN = ${WOZFP_ZP_BASE:02X}")
            continue
        body.append(line)

    src_path = os.path.join(tmpdir, f"wozfp_{org:04x}.s")
    obj_path = os.path.join(tmpdir, f"wozfp_{org:04x}.o")
    cfg_path = os.path.join(tmpdir, f"wozfp_{org:04x}.cfg")
    bin_path = os.path.join(tmpdir, f"wozfp_{org:04x}.bin")

    with open(src_path, "w") as f:
        f.write(".code\n" + WOZFP_JUMP_TABLE + "\n".join(body) + "\n" + WOZFP_HELPERS)

    with open(cfg_path, "w") as f:
        f.write(
            f"MEMORY {{\n    RAM: start = ${org:04X}, size = $2000, fill = no, file = %O;\n}}\n"
            "SEGMENTS {\n    CODE: load = RAM, type = ro;\n}\n"
        )

    subprocess.run(["ca65", src_path, "-o", obj_path], check=True)
    subprocess.run(["ld65", "-C", cfg_path, obj_path, "-o", bin_path], check=True)

    with open(bin_path, "rb") as f:
        return bytearray(f.read())


def main():
    parser = argparse.ArgumentParser(description="Generate 6502 wozfp C header.")
    parser.add_argument("--input", default="tools/wozfp.asm")
    parser.add_argument("--output", default="src/be_6502_wozfp.h")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"Missing {args.input}")

    with open(args.input, "r") as f:
        header_comments = extract_header_comments(f.read())

    with tempfile.TemporaryDirectory() as tmpdir:
        b0 = assemble(args.input, 0x4000, tmpdir)
        b1 = assemble(args.input, 0x4100, tmpdir)

    relocs = compute_relocs(b0, b1)
    for r in relocs:
        b0[r + 1] = (b0[r + 1] - 0x40) & 0xFF

    zp_defs = [
        "/* Zero-page workspace used by wozfp (29 bytes: SIGN..INT+3) */",
        "enum {",
        f"    WOZFP_ZP_SIGN = 0x{WOZFP_ZP_BASE:02X},",
        f"    WOZFP_ZP_X2   = 0x{WOZFP_ZP_BASE + 1:02X},",
        f"    WOZFP_ZP_M2   = 0x{WOZFP_ZP_BASE + 2:02X},",
        f"    WOZFP_ZP_X1   = 0x{WOZFP_ZP_BASE + 5:02X},",
        f"    WOZFP_ZP_M1   = 0x{WOZFP_ZP_BASE + 6:02X}",
        "};",
        "",
    ]

    write_rt_header(
        args.output,
        args.input,
        b0,
        relocs,
        header_comments,
        guard="BE_6502_WOZFP_H",
        sym="be_6502_wozfp",
        rel_sym="be_6502_wozfp_relocs",
        rel_cnt_macro="BE_6502_WOZFP_RELOC_CNT",
        desc="Relocatable 6502 32-bit floating-point runtime (wozfp)",
        gen_script="tools/gen_wozfp.py",
        regen_cmd="make rt-6502",
        extra_lines=zp_defs,
    )
    print(f"Generated {args.output}: {len(b0)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()

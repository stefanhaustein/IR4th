#!/usr/bin/env python3
"""Generate relocatable RV32IM IEEE-754 binary32 runtime header from tools/rv32_fp.c."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

LINKER_SCRIPT = """\
ENTRY(_fp_table)
SECTIONS {
  . = 0x00000000;
  .text : {
    *(.text.entry)
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
  }
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) *(.debug*) }
}
"""


def find_toolchain():
    gcc = shutil.which("riscv64-elf-gcc") or shutil.which("riscv64-unknown-elf-gcc")
    objcopy = shutil.which("riscv64-elf-objcopy") or shutil.which("riscv64-unknown-elf-objcopy")
    readelf = shutil.which("riscv64-elf-readelf") or shutil.which("riscv64-unknown-elf-readelf")
    if not (gcc and objcopy and readelf):
        sys.exit("riscv64-elf-gcc / binutils required (brew install riscv64-elf-gcc)")
    return gcc, objcopy, readelf


def parse_text_relocs(readelf_out):
    """Extract .rela.text R_RISCV_32 and paired (HI20, LO12_I) relocations."""
    in_text = False
    relocs32 = []
    hi20_by_sym = {}
    pairs = []

    for line in readelf_out.splitlines():
        if line.startswith("Relocation section "):
            in_text = "'.rela.text'" in line
            continue
        if not in_text:
            continue
        m = re.match(
            r"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_RISCV_\w+)\s+([0-9a-fA-F]+)\s+(.+?)\s*$",
            line,
        )
        if not m:
            continue
        off = int(m.group(1), 16)
        rtype = m.group(2)
        sym_val = int(m.group(3), 16)
        sym_expr = m.group(4).strip()
        add_m = re.match(r"^(.*?)\s*([+-])\s*([0-9a-fA-Fx]+)$", sym_expr)
        addend = 0
        if add_m:
            sign = -1 if add_m.group(2) == "-" else 1
            addend = sign * int(add_m.group(3), 0)
        target = sym_val + addend

        if rtype == "R_RISCV_32":
            relocs32.append((off, target))
        elif rtype == "R_RISCV_HI20":
            hi20_by_sym[(sym_expr, sym_val)] = (off, target)
        elif rtype in ("R_RISCV_LO12_I", "R_RISCV_LO12_S"):
            key = (sym_expr, sym_val)
            if key not in hi20_by_sym:
                raise RuntimeError(f"Unpaired {rtype} at 0x{off:x} for {sym_expr}")
            hi_off, hi_target = hi20_by_sym[key]
            pairs.append((hi_off, off, hi_target))
        elif rtype in ("R_RISCV_JAL", "R_RISCV_BRANCH", "R_RISCV_RVC_JUMP", "R_RISCV_RVC_BRANCH", "R_RISCV_RELAX"):
            continue
        else:
            raise RuntimeError(f"Unhandled relocation type {rtype} at 0x{off:x}")

    return relocs32, pairs


def write_header(out_path, bin_data, relocs32, pairs):
    lines = [
        "/*",
        " * Relocatable RV32IM IEEE-754 binary32 floating-point runtime for IR4th.",
        " * Auto-generated from tools/rv32_fp.c by tools/gen_rv32_fp.py (do not edit manually).",
        " * Regenerate with: make rt-rv32",
        " */",
        "#ifndef BE_RV32_FP_H",
        "#define BE_RV32_FP_H",
        "",
        "/* Jump-table offsets (4 bytes per J instruction) */",
        "enum {",
        "    RT_FP_ADD  = 0x00,",
        "    RT_FP_SUB  = 0x04,",
        "    RT_FP_MUL  = 0x08,",
        "    RT_FP_DIV  = 0x0C,",
        "    RT_FP_NEG  = 0x10,",
        "    RT_FP_CMP  = 0x14,",
        "    RT_FP_ITOF = 0x18,",
        "    RT_FP_FTOI = 0x1C",
        "};",
        "",
        f"static const u8 be_rv32_fp[{len(bin_data)}] = {{",
    ]
    for off in range(0, len(bin_data), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in bin_data[off : off + 12])
        comma = "," if off + 12 < len(bin_data) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")

    if relocs32:
        lines.append(f"static const u16 be_rv32_fp_relocs32[{len(relocs32) * 2}] = {{")
        for idx, (off, tgt) in enumerate(relocs32):
            comma = "," if idx + 1 < len(relocs32) else ""
            lines.append(f"    0x{off:04X}, 0x{tgt:04X}{comma}")
        lines.append("};")
    else:
        lines.append("static const u16 be_rv32_fp_relocs32[2] = { 0, 0 };")
    lines.append(f"#define BE_RV32_FP_RELOC32_CNT {len(relocs32)}")
    lines.append("")

    if pairs:
        lines.append(f"static const u16 be_rv32_fp_reloc_pairs[{len(pairs) * 3}] = {{")
        for idx, (hi_off, lo_off, tgt) in enumerate(pairs):
            comma = "," if idx + 1 < len(pairs) else ""
            lines.append(f"    0x{hi_off:04X}, 0x{lo_off:04X}, 0x{tgt:04X}{comma}")
        lines.append("};")
    else:
        lines.append("static const u16 be_rv32_fp_reloc_pairs[3] = { 0, 0, 0 };")
    lines.append(f"#define BE_RV32_FP_RELOC_PAIR_CNT {len(pairs)}")
    lines.append("")
    lines.append("#endif /* BE_RV32_FP_H */")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="Generate RV32 soft-float C header.")
    parser.add_argument("--input", default="tools/rv32_fp.c")
    parser.add_argument("--output", default="src/be_rv32_fp.h")
    args = parser.parse_args()

    gcc, objcopy, readelf = find_toolchain()
    with tempfile.TemporaryDirectory() as d:
        obj = os.path.join(d, "fp.o")
        lds = os.path.join(d, "ld.ld")
        elf = os.path.join(d, "fp.elf")
        binf = os.path.join(d, "fp.bin")
        with open(lds, "w") as f:
            f.write(LINKER_SCRIPT)

        subprocess.run([
            gcc, "-march=rv32im", "-mabi=ilp32", "-Os", "-ffreestanding",
            "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
            "-c", args.input, "-o", obj,
        ], check=True)
        subprocess.run([
            gcc, "-march=rv32im", "-mabi=ilp32", "-nostdlib",
            "-Wl,-q", "-T", lds, obj, "-lgcc", "-o", elf,
        ], check=True)
        subprocess.run([objcopy, "-O", "binary", elf, binf], check=True)
        rel_out = subprocess.run([readelf, "-r", elf], capture_output=True, text=True, check=True).stdout
        bin_data = open(binf, "rb").read()

    relocs32, pairs = parse_text_relocs(rel_out)
    write_header(args.output, bin_data, relocs32, pairs)
    print(
        f"Generated {args.output}: {len(bin_data)} bytes, "
        f"{len(relocs32)} reloc32s, {len(pairs)} hi/lo pairs"
    )


if __name__ == "__main__":
    main()

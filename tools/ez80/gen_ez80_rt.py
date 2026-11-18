#!/usr/bin/env python3
"""Generate relocatable eZ80 ADL IEEE-754 float32 + i32 runtime header from AgDev's libagon.a."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ASM_STUB = """\
\t.section .text.entry,"ax",@progbits
\t.global _rt_table
_rt_table:
\tjp __fadd
\tjp __fsub
\tjp __fmul
\tjp __fdiv
\tjp __fcmp
\tjp __ltof
\tjp __ftol
\tjp __lmuls
\tjp __ldivu
\tjp __lremu
\tjp __ldivs
\tjp __lrems
"""

LINKER_SCRIPT = """\
ENTRY(_rt_table)
SECTIONS {
  .text : {
    *(.text.entry)
    *(.text .text.*)
    *(.rodata .rodata.*)
  }
  /DISCARD/ : { *(.comment) *(.debug*) }
}
"""

ENTRIES = [
    ("EZ80_RT_FADD", 0),
    ("EZ80_RT_FSUB", 4),
    ("EZ80_RT_FMUL", 8),
    ("EZ80_RT_FDIV", 12),
    ("EZ80_RT_FCMP", 16),
    ("EZ80_RT_LTOF", 20),
    ("EZ80_RT_FTOL", 24),
    ("EZ80_RT_LMULS", 28),
    ("EZ80_RT_LDIVU", 32),
    ("EZ80_RT_LREMU", 36),
    ("EZ80_RT_LDIVS", 40),
    ("EZ80_RT_LREMS", 44),
]


def find_toolchain(agondev_dir):
    bins = [os.path.join(agondev_dir, "bin")] if agondev_dir else []
    bins.append(os.path.expanduser("~/opt/agondev/bin"))
    path_env = os.pathsep.join(bins + [os.environ.get("PATH", "")])
    as_bin = shutil.which("ez80-none-elf-as", path=path_env)
    ld_bin = shutil.which("ez80-none-elf-ld", path=path_env)
    objcopy_bin = shutil.which("ez80-none-elf-objcopy", path=path_env)
    if not (as_bin and ld_bin and objcopy_bin):
        sys.exit("ez80-none-elf-as/ld/objcopy not found (install AgDev or pass --agondev)")
    libagon = None
    for d in ([agondev_dir] if agondev_dir else []) + [
        os.path.expanduser("~/opt/agondev"),
        os.path.dirname(os.path.dirname(as_bin)),
    ]:
        cand = os.path.join(d, "lib", "libagon.a")
        if os.path.isfile(cand):
            libagon = cand
            break
    if not libagon:
        sys.exit("libagon.a not found")
    return as_bin, ld_bin, objcopy_bin, libagon


def find_relocs24(b0, b1, delta=0x040506):
    if len(b0) != len(b1):
        raise RuntimeError("binary lengths differ between base links")
    relocs = []
    i = 0
    while i < len(b0):
        if b0[i] != b1[i]:
            if i + 3 > len(b0):
                raise RuntimeError(f"truncated reloc at 0x{i:x}")
            v0 = int.from_bytes(b0[i : i + 3], "little")
            v1 = int.from_bytes(b1[i : i + 3], "little")
            if v1 - v0 != delta:
                raise RuntimeError(f"unexpected reloc delta at 0x{i:x}: {v0:#x} -> {v1:#x}")
            relocs.append(i)
            i += 3
        else:
            i += 1
    return relocs


def write_header(out_path, bin_data, relocs):
    lines = [
        "/*",
        " * Relocatable eZ80 ADL IEEE-754 float32 + 32-bit int runtime for IR4th.",
        " * Extracted from AgDev libagon.a (LGPL-3.0) by tools/gen_ez80_rt.py.",
        " * Do not edit manually. Regenerate with: make rt-ez80",
        " */",
        "#ifndef BE_EZ80_RT_H",
        "#define BE_EZ80_RT_H",
        "",
    ]
    for name, off in ENTRIES:
        lines.append(f"#define {name} {off}")
    lines.append("")
    lines.append(f"static const unsigned char ez80_rt_code[{len(bin_data)}] = {{")
    for i in range(0, len(bin_data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in bin_data[i : i + 12])
        comma = "," if i + 12 < len(bin_data) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")
    lines.append(f"static const unsigned short ez80_rt_relocs[{len(relocs)}] = {{")
    for i in range(0, len(relocs), 12):
        chunk = ", ".join(str(r) for r in relocs[i : i + 12])
        comma = "," if i + 12 < len(relocs) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agondev", default=None, help="Path to AgDev root directory")
    ap.add_argument("--output", default="src/be_ez80_rt.h")
    args = ap.parse_args()

    as_bin, ld_bin, objcopy_bin, libagon = find_toolchain(args.agondev)
    with tempfile.TemporaryDirectory() as tmp:
        s_path = os.path.join(tmp, "rt.s")
        o_path = os.path.join(tmp, "rt.o")
        ld_path = os.path.join(tmp, "rt.ld")
        with open(s_path, "w") as f:
            f.write(ASM_STUB)
        with open(ld_path, "w") as f:
            f.write(LINKER_SCRIPT)
        subprocess.check_call([as_bin, "-march=ez80+adl", s_path, "-o", o_path])

        bins = []
        for tag, base in [("0", "0x000000"), ("1", "0x040506")]:
            elf_p = os.path.join(tmp, f"rt{tag}.elf")
            bin_p = os.path.join(tmp, f"rt{tag}.bin")
            subprocess.check_call([ld_bin, "-T", ld_path, "-Ttext", base, o_path, libagon, "-o", elf_p])
            subprocess.check_call([objcopy_bin, "-O", "binary", elf_p, bin_p])
            with open(bin_p, "rb") as f:
                bins.append(f.read())

        relocs = find_relocs24(bins[0], bins[1])
        write_header(args.output, bins[0], relocs)
        print(f"Generated {args.output} ({len(bins[0])} bytes, {len(relocs)} relocs)")


if __name__ == "__main__":
    main()

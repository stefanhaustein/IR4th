"""Shared helpers for generating relocatable runtime C headers."""
import os


def extract_header_comments(asm_text):
    comments = []
    for line in asm_text.splitlines():
        s = line.strip()
        if not s:
            if comments:
                comments.append("")
            continue
        if s.startswith(";"):
            comments.append(s[1:].lstrip())
        else:
            break
    while comments and comments[-1] == "":
        comments.pop()
    return comments


def compute_relocs(bin0, bin1):
    """Find 16-bit absolute address offsets by diffing +0x0100-shifted builds."""
    if len(bin0) != len(bin1):
        raise RuntimeError(f"Binary size mismatch: {len(bin0)} vs {len(bin1)}")
    relocs = []
    for i in range(len(bin0)):
        if bin0[i] != bin1[i]:
            if i == 0 or bin1[i] != ((bin0[i] + 1) & 0xFF):
                raise RuntimeError(
                    f"Unexpected diff at offset 0x{i:04X}: 0x{bin0[i]:02X} vs 0x{bin1[i]:02X}"
                )
            relocs.append(i - 1)
    return relocs


def write_rt_header(
    out_path,
    input_path,
    bin0,
    relocs,
    header_comments,
    *,
    guard,
    sym,
    rel_sym,
    rel_cnt_macro,
    desc,
    gen_script,
    regen_cmd,
    extra_lines=(),
):
    lines = [
        "/*",
        f" * {desc} for IR4th.",
        f" * Auto-generated from {input_path} by {gen_script} (do not edit manually).",
        f" * Regenerate with: {regen_cmd}",
    ]
    if header_comments:
        lines.append(" *")
        lines.append(f" * Original {os.path.basename(input_path)} header:")
        for c in header_comments:
            lines.append(f" *   {c}" if c else " *")
    lines.extend([
        " */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "/* Jump-table offsets (3 bytes per jump entry) */",
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
    ])
    lines.extend(extra_lines)
    lines.append(f"static const u8 {sym}[{len(bin0)}] = {{")

    for off in range(0, len(bin0), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in bin0[off : off + 12])
        comma = "," if off + 12 < len(bin0) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")

    if relocs:
        lines.append(f"static const u16 {rel_sym}[{len(relocs)}] = {{")
        for off in range(0, len(relocs), 10):
            chunk = ", ".join(f"0x{r:04X}" for r in relocs[off : off + 10])
            comma = "," if off + 10 < len(relocs) else ""
            lines.append(f"    {chunk}{comma}")
        lines.append("};")
    else:
        lines.append(f"static const u16 {rel_sym}[1] = {{ 0 }};")

    lines.extend([
        f"#define {rel_cnt_macro} {len(relocs)}",
        "",
        f"#endif /* {guard} */",
        "",
    ])

    with open(out_path, "w") as f:
        f.write("\n".join(lines))

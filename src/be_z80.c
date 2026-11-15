/* be_z80.c - Z80 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: Hardware SP (16-bit slots for W: and A:)
 *   - Frame pointer: IX (locals stored at IX-2, IX-4, ...)
 */
#include "ir4c.h"

/* --- Z80 Raw Opcode Helpers --- */
static void z80_push_hl(void)      { emit8(0xE5); }
static void z80_pop_hl(void)       { emit8(0xE1); }
static void z80_push_de(void)      { emit8(0xD5); }
static void z80_pop_de(void)       { emit8(0xD1); }
static void z80_push_bc(void)      { emit8(0xC5); }
static void z80_pop_bc(void)       { emit8(0xC1); }
static void z80_ex_de_hl(void)     { emit8(0xEB); }
static void z80_ex_sp_hl(void)     { emit8(0xE3); }
static void z80_add_hl_de(void)    { emit8(0x19); }
static void z80_or_a(void)         { emit8(0xB7); }
static void z80_sbc_hl_de(void)    { emit8(0xED); emit8(0x52); }
static void z80_ret(void)          { emit8(0xC9); }
static void z80_ld_hl_imm(u16 v)   { emit8(0x21); emit16(v); }

static void z80_ld_l_ix(i8 disp)   { emit8(0xDD); emit8(0x6E); emit8((u8)disp); }
static void z80_ld_h_ix(i8 disp)   { emit8(0xDD); emit8(0x66); emit8((u8)disp); }
static void z80_ld_ix_l(i8 disp)   { emit8(0xDD); emit8(0x75); emit8((u8)disp); }
static void z80_ld_ix_h(i8 disp)   { emit8(0xDD); emit8(0x74); emit8((u8)disp); }

/* --- Backend Interface Implementation --- */
static u16 z80_cur_addr(void) {
    return cb_cur_addr();
}

static void z80_fn_begin(const char *name, u8 param_cnt,
                         const LocalVar *locals, u8 total_locals) {
    u8 i;
    (void)name;
    (void)locals;

    /* PUSH IX; LD IX, 0; ADD IX, SP */
    emit8(0xDD); emit8(0xE5);
    emit8(0xDD); emit8(0x21); emit16(0x0000);
    emit8(0xDD); emit8(0x39);

    /* Allocate local frame: LD HL, -(total_locals * 2); ADD HL, SP; LD SP, HL */
    if (total_locals > 0) {
        u16 frame_bytes = (u16)total_locals * 2;
        z80_ld_hl_imm((u16)(0 - frame_bytes));
        emit8(0x39); /* ADD HL, SP */
        emit8(0xF9); /* LD SP, HL */
    }

    /* Note: Incoming arguments sit above saved IX and return addr at IX+4, IX+6...
     * Copy incoming parameters into local frame slots IX-2, IX-4, ...
     * Zero-initialize scratch locals after '|' */
    for (i = 0; i < param_cnt; i++) {
        i8 src_disp = (i8)(4 + (param_cnt - 1 - i) * 2);
        i8 dst_disp = (i8)(-((i + 1) * 2));
        z80_ld_l_ix(src_disp);
        z80_ld_h_ix((i8)(src_disp + 1));
        z80_ld_ix_l(dst_disp);
        z80_ld_ix_h((i8)(dst_disp + 1));
    }

    if (total_locals > param_cnt) {
        emit8(0xAF); /* XOR A (A = 0) */
        for (i = param_cnt; i < total_locals; i++) {
            i8 dst_disp = (i8)(-((i + 1) * 2));
            emit8(0xDD); emit8(0x77); emit8((u8)dst_disp);       /* LD (IX+d), A */
            emit8(0xDD); emit8(0x77); emit8((u8)(dst_disp + 1)); /* LD (IX+d+1), A */
        }
    }
}

static void z80_fn_end(u8 param_cnt, u8 total_locals, u8 out_count) {
    u8 i;
    (void)total_locals;

    /* Pop return values from evaluation stack before unwinding frame */
    if (out_count >= 1) z80_pop_de();
    if (out_count >= 2) z80_pop_bc();
    if (out_count >= 3) {
        emit8(0xD9); /* EXX */
        z80_pop_de();
        if (out_count >= 4) z80_pop_bc();
        emit8(0xD9); /* EXX */
    }

    emit8(0xDD); emit8(0xF9); /* LD SP, IX */
    emit8(0xDD); emit8(0xE1); /* POP IX */

    if (param_cnt == 0 && out_count == 0) {
        z80_ret();
        return;
    }

    z80_pop_hl(); /* Return address */
    for (i = 0; i < param_cnt; i++) {
        emit8(0x33); /* INC SP */
        emit8(0x33); /* INC SP */
    }

    if (out_count >= 3) {
        emit8(0xD9); /* EXX */
        if (out_count >= 4) z80_push_bc();
        z80_push_de();
        emit8(0xD9); /* EXX */
    }
    if (out_count >= 2) z80_push_bc();
    if (out_count >= 1) z80_push_de();

    emit8(0xE9); /* JP (HL) */
}

static void z80_call(u16 target_addr) {
    emit8(0xCD); /* CALL nn */
    emit16(target_addr);
}

static void z80_push_w(i32 val) {
    z80_ld_hl_imm((u16)val);
    z80_push_hl();
}

static void z80_push_a(u32 val) {
    z80_ld_hl_imm((u16)val);
    z80_push_hl();
}

static void z80_local_get(u8 slot, u8 type) {
    i8 disp = (i8)(-((slot + 1) * 2));
    (void)type;
    z80_ld_l_ix(disp);
    z80_ld_h_ix((i8)(disp + 1));
    z80_push_hl();
}

static void z80_local_set(u8 slot, u8 type) {
    i8 disp = (i8)(-((slot + 1) * 2));
    (void)type;
    z80_pop_hl();
    z80_ld_ix_l(disp);
    z80_ld_ix_h((i8)(disp + 1));
}

static void z80_stack_op(u8 op) {
    if (op == OP_DUP) {
        z80_pop_hl();
        z80_push_hl();
        z80_push_hl();
    } else if (op == OP_DROP) {
        z80_pop_hl();
    } else if (op == OP_SWAP) {
        z80_pop_hl();
        z80_ex_sp_hl();
        z80_push_hl();
    } else if (op == OP_OVER) {
        z80_pop_de();
        z80_pop_hl();
        z80_push_hl();
        z80_push_de();
        z80_push_hl();
    } else if (op == OP_ROT) {
        /* ( a b c -- b c a ) */
        z80_pop_bc(); /* c */
        z80_pop_de(); /* b */
        z80_pop_hl(); /* a */
        z80_push_de();
        z80_push_bc();
        z80_push_hl();
    }
}

static void z80_alu_op(u8 op) {
    if (op == OP_INVERT) {
        z80_pop_hl();
        emit8(0x7D); emit8(0x2F); emit8(0x6F); /* LD A,L; CPL; LD L,A */
        emit8(0x7C); emit8(0x2F); emit8(0x67); /* LD A,H; CPL; LD H,A */
        z80_push_hl();
        return;
    }

    z80_pop_de(); /* rhs */
    z80_pop_hl(); /* lhs */

    if (op == OP_ADD || op == OP_AADD) {
        z80_add_hl_de();
    } else if (op == OP_SUB || op == OP_ASUB || op == OP_ADIFF) {
        z80_or_a();
        z80_sbc_hl_de();
    } else if (op == OP_AND) {
        emit8(0x7D); emit8(0xA3); emit8(0x6F); /* LD A,L; AND E; LD L,A */
        emit8(0x7C); emit8(0xA2); emit8(0x67); /* LD A,H; AND D; LD H,A */
    } else if (op == OP_OR) {
        emit8(0x7D); emit8(0xB3); emit8(0x6F); /* LD A,L; OR E; LD L,A */
        emit8(0x7C); emit8(0xB2); emit8(0x67); /* LD A,H; OR D; LD H,A */
    } else if (op == OP_XOR) {
        emit8(0x7D); emit8(0xAB); emit8(0x6F); /* LD A,L; XOR E; LD L,A */
        emit8(0x7C); emit8(0xAA); emit8(0x67); /* LD A,H; XOR D; LD H,A */
    } else if (op == OP_LSHIFT) {
        /* Shift HL left by E bits: LD B,E; INC B; JR +1; ADD HL,HL; DJNZ -3 */
        emit8(0x43); emit8(0x04);
        emit8(0x18); emit8(0x01);
        emit8(0x29);
        emit8(0x10); emit8(0xFD);
    } else if (op == OP_RSHIFT) {
        /* Logical shift HL right by E bits */
        emit8(0x43); emit8(0x04);
        emit8(0x18); emit8(0x04);
        emit8(0xCB); emit8(0x3C); /* SRL H */
        emit8(0xCB); emit8(0x1D); /* RR L */
        emit8(0x10); emit8(0xFA);
    } else if (op == OP_ARSHIFT) {
        /* Arithmetic shift HL right by E bits */
        emit8(0x43); emit8(0x04);
        emit8(0x18); emit8(0x04);
        emit8(0xCB); emit8(0x2C); /* SRA H */
        emit8(0xCB); emit8(0x1D); /* RR L */
        emit8(0x10); emit8(0xFA);
    } else {
        ir4_die("unimplemented Z80 ALU op (mul/div require runtime helper)");
    }
    z80_push_hl();
}

static void z80_cmp_op(u8 op) {
    z80_pop_de(); /* rhs */
    z80_pop_hl(); /* lhs */

    if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
        z80_ex_de_hl();
    }

    z80_or_a();
    z80_sbc_hl_de(); /* HL = a - b; sets Z, C, S, P/V flags */

    if (op == OP_EQ || op == OP_AEQ) {
        z80_ld_hl_imm(0xFFFF);
        emit8(0x28); emit8(0x03); /* JR Z, +3 */
        z80_ld_hl_imm(0x0000);
    } else if (op == OP_NE || op == OP_ANE) {
        z80_ld_hl_imm(0xFFFF);
        emit8(0x20); emit8(0x03); /* JR NZ, +3 */
        z80_ld_hl_imm(0x0000);
    } else if (op == OP_ULT || op == OP_UGT || op == OP_ALT || op == OP_AGT) {
        z80_ld_hl_imm(0xFFFF);
        emit8(0x38); emit8(0x03); /* JR C, +3 */
        z80_ld_hl_imm(0x0000);
    } else if (op == OP_UGE || op == OP_ULE) {
        z80_ld_hl_imm(0xFFFF);
        emit8(0x30); emit8(0x03); /* JR NC, +3 */
        z80_ld_hl_imm(0x0000);
    } else if (op == OP_LT || op == OP_GT) {
        z80_ld_hl_imm(0xFFFF);
        emit8(0xEA); emit16((u16)(cb_cur_addr() + 7));  /* JP PE, ovf */
        emit8(0xFA); emit16((u16)(cb_cur_addr() + 10)); /* JP M, done */
        emit8(0x18); emit8(0x03);                       /* JR false */
        /* ovf: */
        emit8(0xF2); emit16((u16)(cb_cur_addr() + 5));  /* JP P, done */
        /* false: */
        z80_ld_hl_imm(0x0000);
        /* done: */
    } else if (op == OP_GE || op == OP_LE) {
        z80_ld_hl_imm(0x0000);
        emit8(0xEA); emit16((u16)(cb_cur_addr() + 7));  /* JP PE, ovf */
        emit8(0xFA); emit16((u16)(cb_cur_addr() + 10)); /* JP M, done */
        emit8(0x18); emit8(0x03);                       /* JR true */
        /* ovf: */
        emit8(0xF2); emit16((u16)(cb_cur_addr() + 5));  /* JP P, done */
        /* true: */
        z80_ld_hl_imm(0xFFFF);
        /* done: */
    } else {
        ir4_die("unimplemented Z80 comparison op");
    }
    z80_push_hl();
}

static void z80_mem_op(u8 op) {
    if (op == OP_CFETCH) {
        z80_pop_hl();
        emit8(0x6E);              /* LD L, (HL) */
        emit8(0x26); emit8(0x00); /* LD H, 0 */
        z80_push_hl();
    } else if (op == OP_CSTORE) {
        z80_pop_hl(); /* addr */
        z80_pop_de(); /* val */
        emit8(0x73);  /* LD (HL), E */
    } else if (op == OP_FETCH || op == OP_AFETCH) {
        z80_pop_hl();
        emit8(0x5E); /* LD E, (HL) */
        emit8(0x23); /* INC HL */
        emit8(0x56); /* LD D, (HL) */
        z80_ex_de_hl();
        z80_push_hl();
    } else if (op == OP_STORE || op == OP_ASTORE) {
        z80_pop_hl(); /* addr */
        z80_pop_de(); /* val */
        emit8(0x73); /* LD (HL), E */
        emit8(0x23); /* INC HL */
        emit8(0x72); /* LD (HL), D */
    } else {
        ir4_die("unimplemented Z80 float memory op");
    }
}

static void z80_conv_op(u8 op) {
    if (op == OP_SEXT) {
        z80_pop_hl();
        emit8(0x7D);              /* LD A, L */
        emit8(0x17);              /* RLA (sign bit into carry) */
        emit8(0x9F);              /* SBC A, A ($FF if neg, $00 if pos) */
        emit8(0x67);              /* LD H, A */
        z80_push_hl();
    } else if (op == OP_W2A || op == OP_A2W) {
        /* 16-bit word and 16-bit address are identical on Z80: zero-cost no-op */
    } else {
        ir4_die("unimplemented Z80 float conversion op");
    }
}

static u16 z80_emit_jz_fwd(void) {
    u16 patch_pos;
    z80_pop_hl();
    emit8(0x7C); /* LD A, H */
    emit8(0xB5); /* OR L */
    emit8(0xCA); /* JP Z, nn */
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static u16 z80_emit_jmp_fwd(void) {
    u16 patch_pos;
    emit8(0xC3); /* JP nn */
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static void z80_emit_jmp_back(u16 target_addr) {
    emit8(0xC3); /* JP nn */
    emit16(target_addr);
}

static void z80_emit_jz_back(u16 target_addr) {
    z80_pop_hl();
    emit8(0x7C); /* LD A, H */
    emit8(0xB5); /* OR L */
    emit8(0xCA); /* JP Z, nn */
    emit16(target_addr);
}

static void z80_patch_fwd(u16 patch_offset) {
    patch16(patch_offset, cb_cur_addr());
}

const Backend be_z80 = {
    "z80",
    2, 2, 6,
    z80_cur_addr,
    z80_fn_begin,
    z80_fn_end,
    z80_call,
    z80_push_w,
    z80_push_a,
    z80_local_get,
    z80_local_set,
    z80_stack_op,
    z80_alu_op,
    z80_cmp_op,
    z80_mem_op,
    z80_conv_op,
    z80_emit_jz_fwd,
    z80_emit_jmp_fwd,
    z80_emit_jmp_back,
    z80_emit_jz_back,
    z80_patch_fwd
};

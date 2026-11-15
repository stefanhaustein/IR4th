/* be_z80.c - Z80 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: Hardware SP (16-bit slots for W: and A:)
 *   - Frame pointer: IX (locals stored at IX-2, IX-4, ...)
 */
#include "ir4c.h"

static u16 rt_mul_addr     = 0;
static u16 rt_udivmod_addr = 0;
static u16 rt_sdivmod_addr = 0;

/* File-local Z80 single-byte opcode constants */
enum {
    /* 8-bit load / ALU */
    Z80_LD_A_N     = 0x3E,
    Z80_LD_A_H     = 0x7C,
    Z80_LD_A_L     = 0x7D,
    Z80_LD_A_D     = 0x7A,
    Z80_LD_B_H     = 0x44,
    Z80_LD_B_E     = 0x43,
    Z80_LD_C_L     = 0x4D,
    Z80_LD_H_N     = 0x26,
    Z80_LD_H_A     = 0x67,
    Z80_LD_H_B     = 0x60,
    Z80_LD_L_A     = 0x6F,
    Z80_LD_L_C     = 0x69,
    Z80_LD_E_A     = 0x5F,
    Z80_LD_D_A     = 0x57,
    Z80_LD_L_MHL   = 0x6E,
    Z80_LD_E_MHL   = 0x5E,
    Z80_LD_D_MHL   = 0x56,
    Z80_LD_MHL_E   = 0x73,
    Z80_LD_MHL_D   = 0x72,
    Z80_INC_B      = 0x04,
    Z80_INC_C      = 0x0C,
    Z80_DEC_C      = 0x0D,
    Z80_DEC_A      = 0x3D,
    Z80_RLA        = 0x17,
    Z80_CPL        = 0x2F,
    Z80_SUB_D      = 0x92,
    Z80_SUB_E      = 0x93,
    Z80_SUB_H      = 0x94,
    Z80_SUB_L      = 0x95,
    Z80_SBC_A_A    = 0x9F,
    Z80_AND_D      = 0xA2,
    Z80_AND_E      = 0xA3,
    Z80_XOR_D      = 0xAA,
    Z80_XOR_E      = 0xAB,
    Z80_XOR_A      = 0xAF,
    Z80_OR_D       = 0xB2,
    Z80_OR_E       = 0xB3,
    Z80_OR_L       = 0xB5,
    Z80_OR_A       = 0xB7,

    /* 16-bit load / stack / exchange / arithmetic */
    Z80_LD_HL_NN   = 0x21,
    Z80_LD_SP_HL   = 0xF9,
    Z80_PUSH_BC    = 0xC5,
    Z80_POP_BC     = 0xC1,
    Z80_PUSH_DE    = 0xD5,
    Z80_POP_DE     = 0xD1,
    Z80_PUSH_HL    = 0xE5,
    Z80_POP_HL     = 0xE1,
    Z80_PUSH_AF    = 0xF5,
    Z80_POP_AF     = 0xF1,
    Z80_EX_DE_HL   = 0xEB,
    Z80_EX_SP_HL   = 0xE3,
    Z80_EXX        = 0xD9,
    Z80_ADD_HL_DE  = 0x19,
    Z80_ADD_HL_HL  = 0x29,
    Z80_ADD_HL_SP  = 0x39,
    Z80_INC_HL     = 0x23,
    Z80_INC_SP     = 0x33,

    /* Control flow */
    Z80_DJNZ       = 0x10,
    Z80_JR         = 0x18,
    Z80_JR_NZ      = 0x20,
    Z80_JR_Z       = 0x28,
    Z80_JR_NC      = 0x30,
    Z80_JR_C       = 0x38,
    Z80_JP         = 0xC3,
    Z80_JP_Z       = 0xCA,
    Z80_JP_PE      = 0xEA,
    Z80_JP_P       = 0xF2,
    Z80_JP_M       = 0xFA,
    Z80_JP_HL      = 0xE9,
    Z80_CALL       = 0xCD,
    Z80_RET        = 0xC9
};

/* --- Prefixed & Multi-byte Z80 Helpers --- */
static void z80_sbc_hl_de(void)     { emit8_8(0xED, 0x52); }
static void z80_adc_hl_hl(void)     { emit8_8(0xED, 0x6A); }

static void z80_rl_b(void)          { emit8_8(0xCB, 0x10); }
static void z80_rl_c(void)          { emit8_8(0xCB, 0x11); }
static void z80_rr_l(void)          { emit8_8(0xCB, 0x1D); }
static void z80_sla_c(void)         { emit8_8(0xCB, 0x21); }
static void z80_sra_h(void)         { emit8_8(0xCB, 0x2C); }
static void z80_srl_h(void)         { emit8_8(0xCB, 0x3C); }

static void z80_push_ix(void)       { emit8_8(0xDD, 0xE5); }
static void z80_pop_ix(void)        { emit8_8(0xDD, 0xE1); }
static void z80_ld_ix_imm(u16 v)    { emit8_8(0xDD, 0x21); emit16(v); }
static void z80_add_ix_sp(void)     { emit8_8(0xDD, 0x39); }
static void z80_ld_sp_ix(void)      { emit8_8(0xDD, 0xF9); }
static void z80_ld_l_ix(i8 disp)    { emit8_8_8(0xDD, 0x6E, (u8)disp); }
static void z80_ld_h_ix(i8 disp)    { emit8_8_8(0xDD, 0x66, (u8)disp); }
static void z80_ld_ix_l(i8 disp)    { emit8_8_8(0xDD, 0x75, (u8)disp); }
static void z80_ld_ix_h(i8 disp)    { emit8_8_8(0xDD, 0x74, (u8)disp); }
static void z80_ld_ix_a(i8 disp)    { emit8_8_8(0xDD, 0x77, (u8)disp); }

static void z80_neg_hl(void) {
    emit8(Z80_XOR_A);
    emit8(Z80_SUB_L);
    emit8(Z80_LD_L_A);
    emit8(Z80_SBC_A_A);
    emit8(Z80_SUB_H);
    emit8(Z80_LD_H_A);
}

static void z80_neg_de(void) {
    emit8(Z80_XOR_A);
    emit8(Z80_SUB_E);
    emit8(Z80_LD_E_A);
    emit8(Z80_SBC_A_A);
    emit8(Z80_SUB_D);
    emit8(Z80_LD_D_A);
}

/* --- Backend Interface Implementation --- */
static u16 z80_cur_addr(void) {
    return cb_cur_addr();
}

static void z80_fn_begin(const char *name, u8 param_cnt,
                         const LocalVar *locals, u8 total_locals) {
    u8 i;
    (void)name;
    (void)locals;

    if (cb_pc() <= 3) {
        rt_mul_addr     = 0;
        rt_udivmod_addr = 0;
        rt_sdivmod_addr = 0;
    }

    z80_push_ix();
    z80_ld_ix_imm(0x0000);
    z80_add_ix_sp();

    /* Allocate local frame: LD HL, -(total_locals * 2); ADD HL, SP; LD SP, HL */
    if (total_locals > 0) {
        u16 frame_bytes = (u16)total_locals * 2;
        emit8_16(Z80_LD_HL_NN, (u16)(0 - frame_bytes));
        emit8(Z80_ADD_HL_SP);
        emit8(Z80_LD_SP_HL);
    }

    /* Copy incoming args (at IX+4, IX+6...) into local slots IX-2, IX-4, ... */
    for (i = 0; i < param_cnt; i++) {
        i8 src_disp = (i8)(4 + (param_cnt - 1 - i) * 2);
        i8 dst_disp = (i8)(-((i + 1) * 2));
        z80_ld_l_ix(src_disp);
        z80_ld_h_ix((i8)(src_disp + 1));
        z80_ld_ix_l(dst_disp);
        z80_ld_ix_h((i8)(dst_disp + 1));
    }

    if (total_locals > param_cnt) {
        emit8(Z80_XOR_A);
        for (i = param_cnt; i < total_locals; i++) {
            i8 dst_disp = (i8)(-((i + 1) * 2));
            z80_ld_ix_a(dst_disp);
            z80_ld_ix_a((i8)(dst_disp + 1));
        }
    }
}

static void z80_fn_end(u8 param_cnt, u8 total_locals, u8 out_count) {
    u8 i;
    (void)total_locals;

    /* Pop return values from evaluation stack before unwinding frame */
    if (out_count >= 1) emit8(Z80_POP_DE);
    if (out_count >= 2) emit8(Z80_POP_BC);
    if (out_count >= 3) {
        emit8(Z80_EXX);
        emit8(Z80_POP_DE);
        if (out_count >= 4) emit8(Z80_POP_BC);
        emit8(Z80_EXX);
    }

    z80_ld_sp_ix();
    z80_pop_ix();

    if (param_cnt == 0 && out_count == 0) {
        emit8(Z80_RET);
        return;
    }

    emit8(Z80_POP_HL); /* Return address */
    for (i = 0; i < param_cnt; i++) {
        emit8(Z80_INC_SP);
        emit8(Z80_INC_SP);
    }

    if (out_count >= 3) {
        emit8(Z80_EXX);
        if (out_count >= 4) emit8(Z80_PUSH_BC);
        emit8(Z80_PUSH_DE);
        emit8(Z80_EXX);
    }
    if (out_count >= 2) emit8(Z80_PUSH_BC);
    if (out_count >= 1) emit8(Z80_PUSH_DE);

    emit8(Z80_JP_HL);
}

static void z80_call(u16 target_addr) {
    emit8_16(Z80_CALL, target_addr);
}

static void z80_push_w(i32 val) {
    emit8_16(Z80_LD_HL_NN, (u16)val);
    emit8(Z80_PUSH_HL);
}

static void z80_push_a(u32 val) {
    emit8_16(Z80_LD_HL_NN, (u16)val);
    emit8(Z80_PUSH_HL);
}

static void z80_local_get(u8 slot, u8 type) {
    i8 disp = (i8)(-((slot + 1) * 2));
    (void)type;
    z80_ld_l_ix(disp);
    z80_ld_h_ix((i8)(disp + 1));
    emit8(Z80_PUSH_HL);
}

static void z80_local_set(u8 slot, u8 type) {
    i8 disp = (i8)(-((slot + 1) * 2));
    (void)type;
    emit8(Z80_POP_HL);
    z80_ld_ix_l(disp);
    z80_ld_ix_h((i8)(disp + 1));
}

static void z80_stack_op(u8 op) {
    if (op == OP_DUP) {
        emit8(Z80_POP_HL);
        emit8(Z80_PUSH_HL);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_DROP) {
        emit8(Z80_POP_HL);
    } else if (op == OP_SWAP) {
        emit8(Z80_POP_HL);
        emit8(Z80_EX_SP_HL);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_OVER) {
        emit8(Z80_POP_DE);
        emit8(Z80_POP_HL);
        emit8(Z80_PUSH_HL);
        emit8(Z80_PUSH_DE);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_ROT) {
        /* ( a b c -- b c a ) */
        emit8(Z80_POP_BC); /* c */
        emit8(Z80_POP_DE); /* b */
        emit8(Z80_POP_HL); /* a */
        emit8(Z80_PUSH_DE);
        emit8(Z80_PUSH_BC);
        emit8(Z80_PUSH_HL);
    }
}

static void z80_emit_mul(void) {
    if (rt_mul_addr == 0) {
        emit8_8(Z80_JR, 19);
        rt_mul_addr = cb_cur_addr();
        /* HL = HL * DE (16-bit) */
        emit8(Z80_LD_B_H);
        emit8(Z80_LD_C_L);
        emit8_16(Z80_LD_HL_NN, 0);
        emit8_8(Z80_LD_A_N, 16);
        /* mul_loop: */
        emit8(Z80_ADD_HL_HL);
        z80_rl_c();
        z80_rl_b();
        emit8_8(Z80_JR_NC, 1);
        emit8(Z80_ADD_HL_DE);
        emit8(Z80_DEC_A);
        emit8_8(Z80_JR_NZ, (u8)-11);
        emit8(Z80_RET);
    }
    z80_call(rt_mul_addr);
}

static void z80_emit_udivmod_helper(void) {
    rt_udivmod_addr = cb_cur_addr();
    /* Input: HL = dividend, DE = divisor. Output: HL = quot, DE = rem */
    emit8(Z80_LD_B_H);
    emit8(Z80_LD_C_L);
    emit8_16(Z80_LD_HL_NN, 0);
    emit8_8(Z80_LD_A_N, 16);
    /* udiv_loop: */
    z80_sla_c();
    z80_rl_b();
    z80_adc_hl_hl();
    emit8(Z80_OR_A);
    z80_sbc_hl_de();
    emit8(Z80_INC_C);
    emit8_8(Z80_JR_NC, 2);
    emit8(Z80_ADD_HL_DE);
    emit8(Z80_DEC_C);
    emit8(Z80_DEC_A);
    emit8_8(Z80_JR_NZ, (u8)-17);
    emit8(Z80_EX_DE_HL); /* DE = rem */
    emit8(Z80_LD_H_B);
    emit8(Z80_LD_L_C);   /* HL = quot */
    emit8(Z80_RET);
}

static void z80_emit_udivmod(void) {
    if (rt_udivmod_addr == 0) {
        emit8_8(Z80_JR, 28);
        z80_emit_udivmod_helper();
    }
    z80_call(rt_udivmod_addr);
}

static void z80_emit_sdivmod(void) {
    if (rt_sdivmod_addr == 0) {
        u16 patch_over;
        emit8(Z80_JP);
        patch_over = cb_pc();
        emit16(0x0000);
        if (rt_udivmod_addr == 0) {
            z80_emit_udivmod_helper();
        }
        rt_sdivmod_addr = cb_cur_addr();
        /* Input: HL = a, DE = b. Output: HL = a / b, DE = a % b (trunc toward 0) */
        emit8(Z80_LD_A_H);
        emit8(Z80_XOR_D); /* bit 7 = quot sign */
        emit8(Z80_PUSH_AF);
        emit8(Z80_LD_A_H); /* bit 7 = rem sign */
        emit8(Z80_PUSH_AF);
        /* abs(HL) */
        emit8(Z80_OR_A);
        emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_hl();
        /* abs(DE) */
        emit8(Z80_LD_A_D);
        emit8(Z80_OR_A);
        emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_de();
        z80_call(rt_udivmod_addr);
        /* sign(rem) on DE */
        emit8(Z80_POP_AF);
        emit8(Z80_OR_A);
        emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_de();
        /* sign(quot) on HL */
        emit8(Z80_POP_AF);
        emit8(Z80_OR_A);
        emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_hl();
        emit8(Z80_RET);
        patch16(patch_over, cb_cur_addr());
    }
    z80_call(rt_sdivmod_addr);
}

static void z80_alu_op(u8 op) {
    if (op == OP_INVERT) {
        emit8(Z80_POP_HL);
        emit8(Z80_LD_A_L); emit8(Z80_CPL); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_CPL); emit8(Z80_LD_H_A);
        emit8(Z80_PUSH_HL);
        return;
    }

    emit8(Z80_POP_DE); /* rhs */
    emit8(Z80_POP_HL); /* lhs */

    if (op == OP_ADD || op == OP_AADD) {
        emit8(Z80_ADD_HL_DE);
    } else if (op == OP_SUB || op == OP_ASUB || op == OP_ADIFF) {
        emit8(Z80_OR_A);
        z80_sbc_hl_de();
    } else if (op == OP_MUL) {
        z80_emit_mul();
    } else if (op == OP_DIV) {
        z80_emit_sdivmod();
    } else if (op == OP_MOD) {
        z80_emit_sdivmod();
        emit8(Z80_EX_DE_HL);
    } else if (op == OP_UDIV) {
        z80_emit_udivmod();
    } else if (op == OP_UMOD) {
        z80_emit_udivmod();
        emit8(Z80_EX_DE_HL);
    } else if (op == OP_AND) {
        emit8(Z80_LD_A_L); emit8(Z80_AND_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_AND_D); emit8(Z80_LD_H_A);
    } else if (op == OP_OR) {
        emit8(Z80_LD_A_L); emit8(Z80_OR_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_OR_D); emit8(Z80_LD_H_A);
    } else if (op == OP_XOR) {
        emit8(Z80_LD_A_L); emit8(Z80_XOR_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_XOR_D); emit8(Z80_LD_H_A);
    } else if (op == OP_LSHIFT) {
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 1);
        emit8(Z80_ADD_HL_HL);
        emit8_8(Z80_DJNZ, (u8)-3);
    } else if (op == OP_RSHIFT) {
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 4);
        z80_srl_h();
        z80_rr_l();
        emit8_8(Z80_DJNZ, (u8)-6);
    } else if (op == OP_ARSHIFT) {
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 4);
        z80_sra_h();
        z80_rr_l();
        emit8_8(Z80_DJNZ, (u8)-6);
    } else {
        ir4_die("unimplemented Z80 ALU op");
    }
    emit8(Z80_PUSH_HL);
}

static void z80_cmp_op(u8 op) {
    emit8(Z80_POP_DE); /* rhs */
    emit8(Z80_POP_HL); /* lhs */

    if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
        emit8(Z80_EX_DE_HL);
    }

    emit8(Z80_OR_A);
    z80_sbc_hl_de(); /* HL = a - b; sets Z, C, S, P/V flags */

    if (op == OP_EQ || op == OP_AEQ) {
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_Z, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
    } else if (op == OP_NE || op == OP_ANE) {
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_NZ, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
    } else if (op == OP_ULT || op == OP_UGT || op == OP_ALT || op == OP_AGT) {
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_C, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
    } else if (op == OP_UGE || op == OP_ULE) {
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_NC, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
    } else if (op == OP_LT || op == OP_GT) {
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8));  /* ovf */
        emit8_16(Z80_JP_M,  (u16)(cb_cur_addr() + 11)); /* done */
        emit8_8(Z80_JR, 3);                             /* false */
        /* ovf: */
        emit8_16(Z80_JP_P,  (u16)(cb_cur_addr() + 6));  /* done */
        /* false: */
        emit8_16(Z80_LD_HL_NN, 0x0000);
        /* done: */
    } else if (op == OP_GE || op == OP_LE) {
        emit8_16(Z80_LD_HL_NN, 0x0000);
        emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8));  /* ovf */
        emit8_16(Z80_JP_M,  (u16)(cb_cur_addr() + 11)); /* done */
        emit8_8(Z80_JR, 3);                             /* true */
        /* ovf: */
        emit8_16(Z80_JP_P,  (u16)(cb_cur_addr() + 6));  /* done */
        /* true: */
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        /* done: */
    } else {
        ir4_die("unimplemented Z80 comparison op");
    }
    emit8(Z80_PUSH_HL);
}

static void z80_mem_op(u8 op) {
    if (op == OP_CFETCH) {
        emit8(Z80_POP_HL);
        emit8(Z80_LD_L_MHL);
        emit8_8(Z80_LD_H_N, 0x00);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_CSTORE) {
        emit8(Z80_POP_HL); /* addr */
        emit8(Z80_POP_DE); /* val */
        emit8(Z80_LD_MHL_E);
    } else if (op == OP_FETCH || op == OP_AFETCH) {
        emit8(Z80_POP_HL);
        emit8(Z80_LD_E_MHL);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_D_MHL);
        emit8(Z80_EX_DE_HL);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_STORE || op == OP_ASTORE) {
        emit8(Z80_POP_HL); /* addr */
        emit8(Z80_POP_DE); /* val */
        emit8(Z80_LD_MHL_E);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D);
    } else {
        ir4_die("unimplemented Z80 float memory op");
    }
}

static void z80_conv_op(u8 op) {
    if (op == OP_SEXT) {
        emit8(Z80_POP_HL);
        emit8(Z80_LD_A_L);
        emit8(Z80_RLA);
        emit8(Z80_SBC_A_A);
        emit8(Z80_LD_H_A);
        emit8(Z80_PUSH_HL);
    } else if (op == OP_W2A || op == OP_A2W) {
        /* 16-bit word and 16-bit address are identical on Z80: zero-cost no-op */
    } else {
        ir4_die("unimplemented Z80 float conversion op");
    }
}

static u16 z80_emit_jz_fwd(void) {
    u16 patch_pos;
    emit8(Z80_POP_HL);
    emit8(Z80_LD_A_H);
    emit8(Z80_OR_L);
    emit8(Z80_JP_Z);
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static u16 z80_emit_jmp_fwd(void) {
    u16 patch_pos;
    emit8(Z80_JP);
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static void z80_emit_jmp_back(u16 target_addr) {
    emit8_16(Z80_JP, target_addr);
}

static void z80_emit_jz_back(u16 target_addr) {
    emit8(Z80_POP_HL);
    emit8(Z80_LD_A_H);
    emit8(Z80_OR_L);
    emit8_16(Z80_JP_Z, target_addr);
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

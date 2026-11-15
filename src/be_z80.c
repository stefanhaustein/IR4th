/* be_z80.c - Z80 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: Hardware SP (16-bit slots for W: and A:, 48-bit for F:)
 *   - Locals & params: SP-relative offsets (no IX frame pointer)
 */
#include "ir4c.h"
#include "be_z80.h"
#include "be_z80_math48.h"
#include <math.h>

static u16 rt_mul_addr     = 0;
static u16 rt_udivmod_addr = 0;
static u16 rt_sdivmod_addr = 0;
static u16 rt_math48_addr  = 0;
static u16 z80_local_off[MAX_LOCALS];
static u16 z80_local_bytes = 0;
static u16 z80_last_push_pc = 0;
static u8  z80_last_push_op = 0;

static void z80_peep_barrier(void) {
    z80_last_push_op = 0;
}

static void z80_push(u8 op) {
    emit8(op);
    z80_last_push_op = op;
    z80_last_push_pc = cb_pc();
}

static void z80_pop(u8 op) {
    if (z80_last_push_op != 0 && cb_pc() == z80_last_push_pc) {
        if ((op == Z80_POP_HL && z80_last_push_op == Z80_PUSH_HL) ||
            (op == Z80_POP_DE && z80_last_push_op == Z80_PUSH_DE)) {
            cb_rewind(1);
            z80_last_push_op = 0;
            return;
        }
        if (op == Z80_POP_DE && z80_last_push_op == Z80_PUSH_HL) {
            cb_rewind(1);
            emit8(Z80_LD_D_H);
            emit8(Z80_LD_E_L);
            z80_last_push_op = 0;
            return;
        }
        if (op == Z80_POP_HL && z80_last_push_op == Z80_PUSH_DE) {
            cb_rewind(1);
            emit8(Z80_LD_H_D);
            emit8(Z80_LD_L_E);
            z80_last_push_op = 0;
            return;
        }
    }
    z80_last_push_op = 0;
    emit8(op);
}

/* --- Prefixed & Multi-byte Z80 Helpers --- */
static void z80_sbc_hl_de(void)     { emit8_8(0xED, 0x52); }
static void z80_adc_hl_hl(void)     { emit8_8(0xED, 0x6A); }

static void z80_rl_b(void)          { emit8_8(0xCB, 0x10); }
static void z80_rl_c(void)          { emit8_8(0xCB, 0x11); }
static void z80_rr_l(void)          { emit8_8(0xCB, 0x1D); }
static void z80_sla_c(void)         { emit8_8(0xCB, 0x21); }
static void z80_sra_h(void)         { emit8_8(0xCB, 0x2C); }
static void z80_srl_h(void)         { emit8_8(0xCB, 0x3C); }

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
    z80_peep_barrier();
    return cb_cur_addr();
}

static void z80_fn_begin(const char *name, u8 param_cnt,
                         const LocalVar *locals, u8 total_locals) {
    u8 i;
    u16 off = 0;
    (void)name;
    z80_peep_barrier();

    if (cb_pc() <= 3) {
        rt_mul_addr     = 0;
        rt_udivmod_addr = 0;
        rt_sdivmod_addr = 0;
        rt_math48_addr  = 0;
    }

    z80_local_bytes = 0;
    for (i = param_cnt; i < total_locals; i++) {
        u8 sz = (locals[i].type == TY_F) ? 6 : 2;
        z80_local_off[i] = z80_local_bytes;
        z80_local_bytes += sz;
    }

    /* Parameters stay in place above [local_bytes] + [2-byte return addr] */
    off = (u16)(z80_local_bytes + 2);
    for (i = param_cnt; i > 0; i--) {
        u8 idx = (u8)(i - 1);
        u8 sz = (locals[idx].type == TY_F) ? 6 : 2;
        z80_local_off[idx] = off;
        off += sz;
    }

    /* Allocate & zero-initialize non-parameter locals on SP */
    if (z80_local_bytes > 0) {
        u16 words = (u16)(z80_local_bytes / 2);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        for (off = 0; off < words; off++) {
            emit8(Z80_PUSH_HL);
        }
    }
}

static void z80_fn_end(const FuncSig *sig, const LocalVar *locals, u8 total_locals) {
    u8 i;
    u16 param_bytes = 0;
    u16 out_bytes = 0;
    u8 out_words;
    (void)locals;
    (void)total_locals;

    for (i = 0; i < sig->in_count; i++) {
        param_bytes += (sig->in_types[i] == TY_F) ? 6 : 2;
    }
    for (i = 0; i < sig->out_count; i++) {
        out_bytes += (sig->out_types[i] == TY_F) ? 6 : 2;
    }
    out_words = (u8)(out_bytes / 2);
    if (out_words > 4) ir4_die("Z80 return values exceed 8 bytes");

    /* Pop return values from evaluation stack before unwinding locals */
    if (out_words >= 1) z80_pop(Z80_POP_DE);
    if (out_words >= 2) emit8(Z80_POP_BC);
    if (out_words >= 3) {
        emit8(Z80_EXX);
        emit8(Z80_POP_DE);
        if (out_words >= 4) emit8(Z80_POP_BC);
        emit8(Z80_EXX);
    }

    /* Drop non-parameter locals without disturbing DE/BC/DE'/BC' */
    if (z80_local_bytes > 0 && z80_local_bytes <= 8) {
        u16 words = (u16)(z80_local_bytes / 2);
        u16 k;
        for (k = 0; k < words; k++) {
            emit8(Z80_POP_AF);
        }
    } else if (z80_local_bytes > 8) {
        emit8_16(Z80_LD_HL_NN, z80_local_bytes);
        emit8(Z80_ADD_HL_SP);
        emit8(Z80_LD_SP_HL);
    }

    if (param_bytes == 0 && out_words == 0) {
        emit8(Z80_RET);
        return;
    }

    emit8(Z80_POP_HL); /* Return address */
    for (i = 0; i < (u8)(param_bytes / 2); i++) {
        emit8(Z80_POP_AF);
    }

    if (out_words >= 3) {
        emit8(Z80_EXX);
        if (out_words >= 4) emit8(Z80_PUSH_BC);
        emit8(Z80_PUSH_DE);
        emit8(Z80_EXX);
    }
    if (out_words >= 2) emit8(Z80_PUSH_BC);
    if (out_words >= 1) emit8(Z80_PUSH_DE);

    emit8(Z80_JP_HL);
}

static void z80_call(u16 target_addr) {
    emit8_16(Z80_CALL, target_addr);
}

static void z80_call_extern(const FuncSig *sig) {
    u8 used_bc = 0;
    u8 used_de = 0;
    u8 used_hl = 0;
    u8 i;

    if (!sig->has_addr) ir4_die("Z80 extern requires a fixed address");
    if (sig->out_count > 1) ir4_die("Z80 extern supports at most 1 return value");

    z80_peep_barrier();
    for (i = sig->in_count; i > 0; i--) {
        const char *r = sig->in_regs[i - 1];
        if (strcmp(r, "BC") == 0 || strcmp(r, "C") == 0) {
            emit8(Z80_POP_BC); used_bc = 1;
        } else if (strcmp(r, "B") == 0) {
            emit8(Z80_POP_BC); emit8(0x41); used_bc = 1; /* LD B, C */
        } else if (strcmp(r, "DE") == 0 || strcmp(r, "E") == 0) {
            emit8(Z80_POP_DE); used_de = 1;
        } else if (strcmp(r, "D") == 0) {
            emit8(Z80_POP_DE); emit8(0x53); used_de = 1; /* LD D, E */
        } else if (strcmp(r, "HL") == 0 || strcmp(r, "L") == 0) {
            emit8(Z80_POP_HL); used_hl = 1;
        } else if (strcmp(r, "H") == 0) {
            emit8(Z80_POP_HL); emit8(0x65); used_hl = 1; /* LD H, L */
        } else if (strcmp(r, "A") == 0) {
            if (!used_hl)      { emit8(Z80_POP_HL); emit8(Z80_LD_A_L); }
            else if (!used_de) { emit8(Z80_POP_DE); emit8(0x7B); } /* LD A, E */
            else if (!used_bc) { emit8(Z80_POP_BC); emit8(0x79); } /* LD A, C */
            else ir4_die("Z80 extern: no free register pair for A");
        } else {
            ir4_die("unknown Z80 extern register");
        }
    }

    emit8_16(Z80_CALL, sig->addr);

    if (sig->out_count == 1) {
        const char *r = sig->out_reg;
        if (strcmp(r, "HL") == 0) {
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "DE") == 0) {
            z80_push(Z80_PUSH_DE);
        } else if (strcmp(r, "BC") == 0) {
            emit8(Z80_PUSH_BC);
        } else if (strcmp(r, "A") == 0) {
            emit8(Z80_LD_L_A); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "B") == 0) {
            emit8(0x68); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "C") == 0) {
            emit8(Z80_LD_L_C); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "D") == 0) {
            emit8(0x6A); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "E") == 0) {
            emit8(Z80_LD_L_E); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "H") == 0) {
            emit8(0x6C); emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "L") == 0) {
            emit8_8(Z80_LD_H_N, 0x00); z80_push(Z80_PUSH_HL);
        } else {
            ir4_die("unknown Z80 extern return register");
        }
    }
}

static void z80_push_w(i32 val) {
    emit8_16(Z80_LD_HL_NN, (u16)val);
    z80_push(Z80_PUSH_HL);
}

static void z80_push_a(u32 val) {
    emit8_16(Z80_LD_HL_NN, (u16)val);
    z80_push(Z80_PUSH_HL);
}

static void z80_push_f(double val) {
    u8 bytes[6];
    if (val == 0.0) {
        memset(bytes, 0, sizeof(bytes));
    } else {
        int e, i;
        int sign = (val < 0.0);
        double m = frexp(sign ? -val : val, &e);
        m += 0.5 / 1099511627776.0; /* round to 40-bit mantissa (2^-41) */
        if (m >= 1.0) {
            m *= 0.5;
            e++;
        }
        if (e + 128 <= 0) {
            memset(bytes, 0, sizeof(bytes));
        } else if (e + 128 > 255) {
            ir4_die("Z80 float literal overflow");
        } else {
            for (i = 0; i < 5; i++) {
                int b;
                m *= 256.0;
                b = (int)m;
                if (b > 255) b = 255;
                m -= (double)b;
                bytes[5 - i] = (u8)b;
            }
            bytes[5] = (u8)((bytes[5] & 0x7F) | (sign ? 0x80 : 0x00));
            bytes[0] = (u8)(e + 128);
        }
    }
    emit8_16(Z80_LD_HL_NN, (u16)(bytes[4] | ((u16)bytes[5] << 8)));
    emit8(Z80_PUSH_HL);
    emit8_16(Z80_LD_HL_NN, (u16)(bytes[2] | ((u16)bytes[3] << 8)));
    emit8(Z80_PUSH_HL);
    emit8_16(Z80_LD_HL_NN, (u16)(bytes[0] | ((u16)bytes[1] << 8)));
    z80_push(Z80_PUSH_HL);
}

static void z80_push_data(u16 offset) {
    emit8(Z80_LD_HL_NN);
    cb_add_data_reloc(cb_pc(), offset);
    emit16(0x0000);
    z80_push(Z80_PUSH_HL);
}

static void z80_local_get(u8 slot, u8 type, u16 stack_bytes) {
    u16 off = (u16)(z80_local_off[slot] + stack_bytes);
    if (type == TY_F) {
        u8 i;
        emit8_16(Z80_LD_HL_NN, (u16)(off + 5));
        emit8(Z80_ADD_HL_SP);
        for (i = 0; i < 3; i++) {
            emit8(Z80_LD_D_MHL);
            emit8(Z80_DEC_HL);
            emit8(Z80_LD_E_MHL);
            if (i < 2) emit8(Z80_DEC_HL);
            z80_push(Z80_PUSH_DE);
        }
    } else {
        emit8_16(Z80_LD_HL_NN, off);
        emit8(Z80_ADD_HL_SP);
        emit8(Z80_LD_E_MHL);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_D_MHL);
        z80_push(Z80_PUSH_DE);
    }
}

static void z80_local_set(u8 slot, u8 type, u16 stack_bytes) {
    u16 off = (u16)(z80_local_off[slot] + stack_bytes);
    if (type == TY_F) {
        u8 i;
        emit8_16(Z80_LD_HL_NN, (u16)(off + 6));
        emit8(Z80_ADD_HL_SP);
        for (i = 0; i < 3; i++) {
            emit8(Z80_POP_DE);
            emit8(Z80_LD_MHL_E);
            emit8(Z80_INC_HL);
            emit8(Z80_LD_MHL_D);
            if (i < 2) emit8(Z80_INC_HL);
        }
    } else {
        z80_pop(Z80_POP_DE);
        emit8_16(Z80_LD_HL_NN, off);
        emit8(Z80_ADD_HL_SP);
        emit8(Z80_LD_MHL_E);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D);
    }
}

static void z80_pop_f_ac(void) {
    z80_pop(Z80_POP_HL);
    emit8(Z80_POP_DE);
    emit8(Z80_POP_BC);
}

static void z80_push_f_ac(void) {
    emit8(Z80_PUSH_BC);
    emit8(Z80_PUSH_DE);
    z80_push(Z80_PUSH_HL);
}

static void z80_local_op(LocalOp op, u8 slot, StackType type, u16 stack_bytes) {
    if (op == LOCAL_GET) z80_local_get(slot, (u8)type, stack_bytes);
    else                 z80_local_set(slot, (u8)type, stack_bytes);
}

static void z80_stack_op(StackOp op, StackType t1, StackType t2) {
    switch (op) {
    case OP_DUP:
        if (t1 == TY_F) {
            z80_pop_f_ac();
            z80_push_f_ac();
            z80_push_f_ac();
        } else {
            z80_pop(Z80_POP_HL);
            emit8(Z80_PUSH_HL);
            z80_push(Z80_PUSH_HL);
        }
        break;
    case OP_DROP:
        z80_pop(Z80_POP_HL);
        if (t1 == TY_F) {
            emit8(Z80_POP_HL);
            emit8(Z80_POP_HL);
        }
        break;
    case OP_SWAP:
        if (t1 != TY_F && t2 != TY_F) {
            z80_pop(Z80_POP_HL);
            emit8(Z80_EX_SP_HL);
            z80_push(Z80_PUSH_HL);
        } else if (t1 == TY_F && t2 == TY_F) {
            z80_pop_f_ac();
            emit8(Z80_EXX); z80_pop_f_ac(); emit8(Z80_EXX);
            z80_push_f_ac();
            emit8(Z80_EXX); z80_push_f_ac(); emit8(Z80_EXX);
        } else if (t1 == TY_F) {
            z80_pop_f_ac();
            emit8(Z80_POP_AF);
            z80_push_f_ac();
            emit8(Z80_PUSH_AF);
        } else {
            emit8(Z80_POP_AF);
            z80_pop_f_ac();
            emit8(Z80_PUSH_AF);
            z80_push_f_ac();
        }
        break;
    case OP_OVER:
        if (t1 != TY_F && t2 != TY_F) {
            z80_pop(Z80_POP_DE);
            emit8(Z80_POP_HL);
            emit8(Z80_PUSH_HL);
            emit8(Z80_PUSH_DE);
            z80_push(Z80_PUSH_HL);
        } else if (t1 == TY_F && t2 == TY_F) {
            z80_pop_f_ac();
            emit8(Z80_EXX); z80_pop_f_ac(); z80_push_f_ac(); emit8(Z80_EXX);
            z80_push_f_ac();
            emit8(Z80_EXX); z80_push_f_ac(); emit8(Z80_EXX);
        } else if (t1 == TY_F) {
            z80_pop_f_ac();
            emit8(Z80_POP_AF);
            emit8(Z80_PUSH_AF);
            z80_push_f_ac();
            emit8(Z80_PUSH_AF);
        } else {
            emit8(Z80_POP_AF);
            z80_pop_f_ac();
            z80_push_f_ac();
            emit8(Z80_PUSH_AF);
            z80_push_f_ac();
        }
        break;
    default:
        ir4_die("unimplemented Z80 stack op");
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

static void z80_ensure_math48(void) {
    u16 skip_patch;
    u16 base_off;
    u16 i;

    if (rt_math48_addr != 0) return;

    emit8(Z80_JP);
    skip_patch = cb_pc();
    emit16(0x0000);

    rt_math48_addr = cb_cur_addr();
    base_off = cb_pc();
    for (i = 0; i < (u16)sizeof(be_z80_math48); i++) {
        emit8(be_z80_math48[i]);
    }
    for (i = 0; i < BE_Z80_MATH48_RELOC_CNT; i++) {
        u16 rel_off = be_z80_math48_relocs[i];
        u16 rel_val = (u16)(be_z80_math48[rel_off] |
                            ((u16)be_z80_math48[rel_off + 1] << 8));
        patch16((u16)(base_off + rel_off), (u16)(rt_math48_addr + rel_val));
    }

    patch16(skip_patch, cb_cur_addr());
}

static void z80_call_math48(u16 entry_offset) {
    z80_ensure_math48();
    z80_call((u16)(rt_math48_addr + entry_offset));
}

static void z80_int_alu(IrOp op) {
    z80_pop(Z80_POP_DE); /* rhs */
    emit8(Z80_POP_HL); /* lhs */

    switch (op) {
    case OP_ADD:
    case OP_AADD:
        emit8(Z80_ADD_HL_DE);
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        emit8(Z80_OR_A);
        z80_sbc_hl_de();
        break;
    case OP_MUL:
        z80_emit_mul();
        break;
    case OP_DIV:
        z80_emit_sdivmod();
        break;
    case OP_MOD:
        z80_emit_sdivmod();
        emit8(Z80_EX_DE_HL);
        break;
    case OP_UDIV:
        z80_emit_udivmod();
        break;
    case OP_UMOD:
        z80_emit_udivmod();
        emit8(Z80_EX_DE_HL);
        break;
    case OP_AND:
        emit8(Z80_LD_A_L); emit8(Z80_AND_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_AND_D); emit8(Z80_LD_H_A);
        break;
    case OP_OR:
        emit8(Z80_LD_A_L); emit8(Z80_OR_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_OR_D); emit8(Z80_LD_H_A);
        break;
    case OP_XOR:
        emit8(Z80_LD_A_L); emit8(Z80_XOR_E); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_XOR_D); emit8(Z80_LD_H_A);
        break;
    case OP_LSHIFT:
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 1);
        emit8(Z80_ADD_HL_HL);
        emit8_8(Z80_DJNZ, (u8)-3);
        break;
    case OP_RSHIFT:
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 4);
        z80_srl_h();
        z80_rr_l();
        emit8_8(Z80_DJNZ, (u8)-6);
        break;
    case OP_ARSHIFT:
        emit8(Z80_LD_B_E); emit8(Z80_INC_B);
        emit8_8(Z80_JR, 4);
        z80_sra_h();
        z80_rr_l();
        emit8_8(Z80_DJNZ, (u8)-6);
        break;
    default:
        ir4_die("unimplemented Z80 ALU op");
    }
    z80_push(Z80_PUSH_HL);
}

static void z80_int_cmp(IrOp op) {
    if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
        z80_pop(Z80_POP_HL); /* rhs */
        emit8(Z80_POP_DE);   /* lhs */
    } else {
        z80_pop(Z80_POP_DE); /* rhs */
        emit8(Z80_POP_HL);   /* lhs */
    }

    emit8(Z80_OR_A);
    z80_sbc_hl_de(); /* HL = a - b; sets Z, C, S, P/V flags */

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_Z, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_NE:
    case OP_ANE:
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_NZ, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_C, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_UGE:
    case OP_ULE:
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8(Z80_JR_NC, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_LT:
    case OP_GT:
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8));  /* ovf */
        emit8_16(Z80_JP_M,  (u16)(cb_cur_addr() + 11)); /* done */
        emit8_8(Z80_JR, 3);                             /* false */
        /* ovf: */
        emit8_16(Z80_JP_P,  (u16)(cb_cur_addr() + 6));  /* done */
        /* false: */
        emit8_16(Z80_LD_HL_NN, 0x0000);
        /* done: */
        break;
    case OP_GE:
    case OP_LE:
        emit8_16(Z80_LD_HL_NN, 0x0000);
        emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8));  /* ovf */
        emit8_16(Z80_JP_M,  (u16)(cb_cur_addr() + 11)); /* done */
        emit8_8(Z80_JR, 3);                             /* true */
        /* ovf: */
        emit8_16(Z80_JP_P,  (u16)(cb_cur_addr() + 6));  /* done */
        /* true: */
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        /* done: */
        break;
    default:
        ir4_die("unimplemented Z80 comparison op");
    }
    z80_push(Z80_PUSH_HL);
}

static void z80_op(IrOp op) {
    switch (op) {
    case OP_ADD: case OP_AADD:
    case OP_SUB: case OP_ASUB: case OP_ADIFF:
    case OP_MUL: case OP_DIV: case OP_MOD: case OP_UDIV: case OP_UMOD:
    case OP_AND: case OP_OR: case OP_XOR:
    case OP_LSHIFT: case OP_RSHIFT: case OP_ARSHIFT:
        z80_int_alu(op);
        break;
    case OP_INVERT:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_A_L); emit8(Z80_CPL); emit8(Z80_LD_L_A);
        emit8(Z80_LD_A_H); emit8(Z80_CPL); emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_FNEG:
        z80_pop_f_ac();
        z80_call_math48(RT_FP_NEG);
        z80_push_f_ac();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        u16 entry = (op == OP_FADD) ? RT_FP_ADD :
                    (op == OP_FSUB) ? RT_FP_SUB :
                    (op == OP_FMUL) ? RT_FP_MUL : RT_FP_DIV;
        emit8(Z80_EXX); z80_pop_f_ac(); emit8(Z80_EXX); /* rhs -> AC' */
        z80_pop_f_ac();                                  /* lhs -> AC */
        z80_call_math48(entry);
        z80_push_f_ac();
        break;
    }
    case OP_EQ: case OP_AEQ:
    case OP_NE: case OP_ANE:
    case OP_LT: case OP_GE: case OP_GT: case OP_LE:
    case OP_ULT: case OP_ALT: case OP_UGE: case OP_UGT: case OP_AGT: case OP_ULE:
        z80_int_cmp(op);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
        if (op == OP_FGT) {
            z80_pop_f_ac();                                  /* rhs -> AC */
            emit8(Z80_EXX); z80_pop_f_ac(); emit8(Z80_EXX); /* lhs -> AC' */
        } else {
            emit8(Z80_EXX); z80_pop_f_ac(); emit8(Z80_EXX); /* rhs -> AC' */
            z80_pop_f_ac();                                  /* lhs -> AC */
        }
        z80_call_math48(RT_FP_CMP);
        emit8_16(Z80_LD_HL_NN, 0xFFFF);
        emit8_8((op == OP_FEQ) ? Z80_JR_Z : (op == OP_FNE) ? Z80_JR_NZ : Z80_JR_C, 3);
        emit8_16(Z80_LD_HL_NN, 0x0000);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_U8FETCH:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_L_MHL);
        emit8_8(Z80_LD_H_N, 0x00);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_I8FETCH:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_L_MHL);
        emit8(Z80_LD_A_L);
        emit8(Z80_RLA);
        emit8(Z80_SBC_A_A);
        emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_I8STORE:
        z80_pop(Z80_POP_HL); /* addr */
        emit8(Z80_POP_DE); /* val */
        emit8(Z80_LD_MHL_E);
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_E_MHL);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_D_MHL);
        emit8(Z80_EX_DE_HL);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        z80_pop(Z80_POP_HL); /* addr */
        emit8(Z80_POP_DE); /* val */
        emit8(Z80_LD_MHL_E);
        emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D);
        break;
    case OP_FFETCH:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_E_MHL); emit8(Z80_INC_HL);
        emit8(Z80_LD_D_MHL); emit8(Z80_INC_HL);
        emit8(Z80_LD_C_MHL); emit8(Z80_INC_HL);
        emit8(Z80_LD_B_MHL); emit8(Z80_INC_HL);
        emit8(Z80_LD_A_MHL); emit8(Z80_INC_HL);
        emit8(Z80_LD_H_MHL);
        emit8(Z80_LD_L_A);
        emit8(Z80_PUSH_HL);
        emit8(Z80_PUSH_BC);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_FSTORE:
        z80_pop(Z80_POP_HL);
        emit8(Z80_POP_DE);
        emit8(Z80_LD_MHL_E); emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D); emit8(Z80_INC_HL);
        emit8(Z80_POP_DE);
        emit8(Z80_LD_MHL_E); emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D); emit8(Z80_INC_HL);
        emit8(Z80_POP_DE);
        emit8(Z80_LD_MHL_E); emit8(Z80_INC_HL);
        emit8(Z80_LD_MHL_D);
        break;
    case OP_SEXT:
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_A_L);
        emit8(Z80_RLA);
        emit8(Z80_SBC_A_A);
        emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_W2A:
    case OP_A2W:
        /* 16-bit word and 16-bit address are identical on Z80: zero-cost no-op */
        break;
    case OP_W2F:
        z80_pop(Z80_POP_HL);
        z80_call_math48(RT_FP_ITOF);
        z80_push_f_ac();
        break;
    case OP_F2W:
        z80_pop_f_ac();
        z80_call_math48(RT_FP_FTOI);
        z80_push(Z80_PUSH_HL);
        break;
    }
}

static u16 z80_emit_br_fwd(BranchCond cond) {
    u16 patch_pos;
    if (cond == BR_JZ) {
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_A_H);
        emit8(Z80_OR_L);
        emit8(Z80_JP_Z);
    } else {
        emit8(Z80_JP);
    }
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static void z80_emit_br_back(BranchCond cond, u16 target_addr) {
    if (cond == BR_JZ) {
        z80_pop(Z80_POP_HL);
        emit8(Z80_LD_A_H);
        emit8(Z80_OR_L);
        emit8_16(Z80_JP_Z, target_addr);
    } else {
        emit8_16(Z80_JP, target_addr);
    }
}

static void z80_patch_fwd(u16 patch_offset) {
    z80_peep_barrier();
    patch16(patch_offset, cb_cur_addr());
}

const Backend be_z80 = {
    "z80",
    2, 2, 6,
    z80_cur_addr,
    z80_fn_begin,
    z80_fn_end,
    z80_call,
    z80_call_extern,
    z80_push_w,
    z80_push_a,
    z80_push_f,
    z80_push_data,
    z80_local_op,
    z80_stack_op,
    z80_op,
    z80_emit_br_fwd,
    z80_emit_br_back,
    z80_patch_fwd
};

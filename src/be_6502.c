/* be_6502.c - 6502 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: Zero Page $80..$FF indexed by X (grows downward via DEX/DEX)
 *   - Locals: Zero Page $20..$5F (2 bytes per slot: $20 + slot*2, preserved on HW stack)
 *   - Scratch pointers: Zero Page $00..$03 (used for indirect ($00),Y loads/stores)
 */
#include "ir4c.h"

#define ZP_SCRATCH 0x00
#define ZP_LOCALS  0x20
#define ZP_STACK   0x80

/* --- 6502 Raw Opcode Helpers --- */
static void m6502_clc(void)        { emit8(0x18); }
static void m6502_sec(void)        { emit8(0x38); }
static void m6502_pha(void)        { emit8(0x48); }
static void m6502_pla(void)        { emit8(0x68); }
static void m6502_dex(void)        { emit8(0xCA); }
static void m6502_inx(void)        { emit8(0xE8); }
static void m6502_tya(void)        { emit8(0x98); }
static void m6502_rts(void)        { emit8(0x60); }

static void m6502_lda_imm(u8 v)    { emit8(0xA9); emit8(v); }
static void m6502_ldy_imm(u8 v)    { emit8(0xA0); emit8(v); }
static void m6502_lda_zp(u8 zp)    { emit8(0xA5); emit8(zp); }
static void m6502_sta_zp(u8 zp)    { emit8(0x85); emit8(zp); }
static void m6502_lda_zpx(u8 zp)   { emit8(0xB5); emit8(zp); }
static void m6502_sta_zpx(u8 zp)   { emit8(0x95); emit8(zp); }
static void m6502_ldy_zpx(u8 zp)   { emit8(0xB4); emit8(zp); }
static void m6502_adc_zpx(u8 zp)   { emit8(0x75); emit8(zp); }
static void m6502_sbc_zpx(u8 zp)   { emit8(0xF5); emit8(zp); }
static void m6502_and_zpx(u8 zp)   { emit8(0x35); emit8(zp); }
static void m6502_ora_zpx(u8 zp)   { emit8(0x15); emit8(zp); }
static void m6502_eor_zpx(u8 zp)   { emit8(0x55); emit8(zp); }
static void m6502_eor_imm(u8 v)    { emit8(0x49); emit8(v); }
static void m6502_asl_zpx(u8 zp)   { emit8(0x16); emit8(zp); }
static void m6502_rol_zpx(u8 zp)   { emit8(0x36); emit8(zp); }
static void m6502_lsr_zpx(u8 zp)   { emit8(0x56); emit8(zp); }
static void m6502_ror_zpx(u8 zp)   { emit8(0x76); emit8(zp); }

static void m6502_lda_indy(u8 zp)  { emit8(0xB1); emit8(zp); }
static void m6502_sta_indy(u8 zp)  { emit8(0x91); emit8(zp); }

static void m6502_jsr(u16 addr)    { emit8(0x20); emit16(addr); }
static void m6502_jmp(u16 addr)    { emit8(0x4C); emit16(addr); }

/* --- Backend Interface Implementation --- */
static u16 m6502_cur_addr(void) {
    return cb_cur_addr();
}

static void m6502_fn_begin(const char *name, u8 param_cnt,
                           const LocalVar *locals, u8 total_locals) {
    u8 i;
    (void)name;
    (void)locals;

    /* Preserve caller's ZP local slots on hardware stack for reentrancy */
    for (i = 0; i < total_locals; i++) {
        u8 zp = (u8)(ZP_LOCALS + i * 2);
        m6502_lda_zp(zp);
        m6502_pha();
        m6502_lda_zp((u8)(zp + 1));
        m6502_pha();
    }

    /* Pop incoming parameters from ZP stack ($80,X) into ZP locals ($20+slot*2) */
    for (i = param_cnt; i > 0; i--) {
        u8 zp = (u8)(ZP_LOCALS + (i - 1) * 2);
        m6502_lda_zpx(ZP_STACK);
        m6502_sta_zp(zp);
        m6502_lda_zpx(ZP_STACK + 1);
        m6502_sta_zp((u8)(zp + 1));
        m6502_inx();
        m6502_inx();
    }

    /* Zero-initialize scratch locals after '|' */
    if (total_locals > param_cnt) {
        m6502_lda_imm(0x00);
        for (i = param_cnt; i < total_locals; i++) {
            u8 zp = (u8)(ZP_LOCALS + i * 2);
            m6502_sta_zp(zp);
            m6502_sta_zp((u8)(zp + 1));
        }
    }
}

static void m6502_fn_end(u8 param_cnt, u8 total_locals, u8 out_count) {
    u8 i;
    (void)param_cnt;
    (void)out_count;

    /* Restore caller's ZP local slots in reverse order */
    for (i = total_locals; i > 0; i--) {
        u8 zp = (u8)(ZP_LOCALS + (i - 1) * 2);
        m6502_pla();
        m6502_sta_zp((u8)(zp + 1));
        m6502_pla();
        m6502_sta_zp(zp);
    }

    m6502_rts();
}

static void m6502_call(u16 target_addr) {
    m6502_jsr(target_addr);
}

static void m6502_push_w(i32 val) {
    u16 v = (u16)val;
    m6502_dex();
    m6502_dex();
    m6502_lda_imm((u8)(v & 0xFF));
    m6502_sta_zpx(ZP_STACK);
    m6502_lda_imm((u8)((v >> 8) & 0xFF));
    m6502_sta_zpx(ZP_STACK + 1);
}

static void m6502_push_a(u32 val) {
    m6502_push_w((i32)val);
}

static void m6502_push_data(u16 offset) {
    u16 lo_pos, hi_pos;
    m6502_dex();
    m6502_dex();
    emit8(0xA9); lo_pos = cb_pc(); emit8(0x00); /* LDA #<addr */
    m6502_sta_zpx(ZP_STACK);
    emit8(0xA9); hi_pos = cb_pc(); emit8(0x00); /* LDA #>addr */
    m6502_sta_zpx(ZP_STACK + 1);
    cb_add_data_reloc_split(lo_pos, hi_pos, offset);
}

static void m6502_local_get(u8 slot, u8 type) {
    u8 zp = (u8)(ZP_LOCALS + slot * 2);
    (void)type;
    m6502_dex();
    m6502_dex();
    m6502_lda_zp(zp);
    m6502_sta_zpx(ZP_STACK);
    m6502_lda_zp((u8)(zp + 1));
    m6502_sta_zpx(ZP_STACK + 1);
}

static void m6502_local_set(u8 slot, u8 type) {
    u8 zp = (u8)(ZP_LOCALS + slot * 2);
    (void)type;
    m6502_lda_zpx(ZP_STACK);
    m6502_sta_zp(zp);
    m6502_lda_zpx(ZP_STACK + 1);
    m6502_sta_zp((u8)(zp + 1));
    m6502_inx();
    m6502_inx();
}

static void m6502_stack_op(u8 op) {
    if (op == OP_DUP) {
        m6502_dex();
        m6502_dex();
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sta_zpx(ZP_STACK);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_DROP) {
        m6502_inx();
        m6502_inx();
    } else if (op == OP_SWAP) {
        m6502_ldy_zpx(ZP_STACK);
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sta_zpx(ZP_STACK);
        m6502_tya();
        m6502_sta_zpx(ZP_STACK + 2);

        m6502_ldy_zpx(ZP_STACK + 1);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_sta_zpx(ZP_STACK + 1);
        m6502_tya();
        m6502_sta_zpx(ZP_STACK + 3);
    } else if (op == OP_OVER) {
        m6502_dex();
        m6502_dex();
        m6502_lda_zpx(ZP_STACK + 4);
        m6502_sta_zpx(ZP_STACK);
        m6502_lda_zpx(ZP_STACK + 5);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_ROT) {
        m6502_ldy_zpx(ZP_STACK + 4);
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sta_zpx(ZP_STACK + 4);
        m6502_lda_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_tya();
        m6502_sta_zpx(ZP_STACK);

        m6502_ldy_zpx(ZP_STACK + 5);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_sta_zpx(ZP_STACK + 5);
        m6502_lda_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_tya();
        m6502_sta_zpx(ZP_STACK + 1);
    }
}

static void m6502_alu_op(u8 op) {
    if (op == OP_INVERT) {
        m6502_lda_zpx(ZP_STACK);
        m6502_eor_imm(0xFF);
        m6502_sta_zpx(ZP_STACK);
        m6502_lda_zpx(ZP_STACK + 1);
        m6502_eor_imm(0xFF);
        m6502_sta_zpx(ZP_STACK + 1);
        return;
    }

    if (op == OP_ADD || op == OP_AADD) {
        m6502_clc();
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_adc_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_adc_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_inx();
        m6502_inx();
    } else if (op == OP_SUB || op == OP_ASUB || op == OP_ADIFF) {
        m6502_sec();
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sbc_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_sbc_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_inx();
        m6502_inx();
    } else if (op == OP_AND) {
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_and_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_and_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_inx();
        m6502_inx();
    } else if (op == OP_OR) {
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_ora_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_ora_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_inx();
        m6502_inx();
    } else if (op == OP_XOR) {
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_eor_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 2);
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_eor_zpx(ZP_STACK + 1);
        m6502_sta_zpx(ZP_STACK + 3);
        m6502_inx();
        m6502_inx();
    } else if (op == OP_LSHIFT || op == OP_RSHIFT || op == OP_ARSHIFT) {
        m6502_ldy_zpx(ZP_STACK);
        m6502_inx();
        m6502_inx();
        emit8(0xC0); emit8(0x00); /* CPY #0 */
        if (op == OP_LSHIFT) {
            emit8(0xF0); emit8(0x07); /* BEQ +7 */
            m6502_asl_zpx(ZP_STACK);
            m6502_rol_zpx(ZP_STACK + 1);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF9); /* BNE -7 */
        } else if (op == OP_RSHIFT) {
            emit8(0xF0); emit8(0x07); /* BEQ +7 */
            m6502_lsr_zpx(ZP_STACK + 1);
            m6502_ror_zpx(ZP_STACK);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF9); /* BNE -7 */
        } else {
            emit8(0xF0); emit8(0x0A); /* BEQ +10 */
            m6502_lda_zpx(ZP_STACK + 1);
            emit8(0x0A);              /* ASL A */
            m6502_ror_zpx(ZP_STACK + 1);
            m6502_ror_zpx(ZP_STACK);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF6); /* BNE -10 */
        }
    } else {
        ir4_die("unimplemented 6502 ALU op (mul/div require runtime helper)");
    }
}

static void m6502_cmp_op(u8 op) {
    u8 lhs_lo = ZP_STACK;
    u8 rhs_lo = (u8)(ZP_STACK - 2);

    /* Adjust X up front so INX does not clobber N/Z flags before branches */
    m6502_inx();
    m6502_inx();

    if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
        lhs_lo = (u8)(ZP_STACK - 2);
        rhs_lo = ZP_STACK;
    }

    if (op == OP_EQ || op == OP_NE || op == OP_AEQ || op == OP_ANE) {
        m6502_lda_zpx(ZP_STACK);
        m6502_eor_zpx((u8)(ZP_STACK - 2));
        m6502_sta_zp(ZP_SCRATCH);
        m6502_lda_zpx(ZP_STACK + 1);
        m6502_eor_zpx((u8)(ZP_STACK - 1));
        emit8(0x05); emit8(ZP_SCRATCH); /* ORA $00 */
        if (op == OP_EQ || op == OP_AEQ) {
            emit8(0xF0); emit8(0x04); /* BEQ true */
        } else {
            emit8(0xD0); emit8(0x04); /* BNE true */
        }
        m6502_lda_imm(0x00);
        emit8(0xF0); emit8(0x02);     /* BEQ store */
        m6502_lda_imm(0xFF);
        m6502_sta_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_ULT || op == OP_UGT || op == OP_ALT || op == OP_AGT ||
               op == OP_UGE || op == OP_ULE) {
        m6502_sec();
        m6502_lda_zpx(lhs_lo);
        m6502_sbc_zpx(rhs_lo);
        m6502_lda_zpx((u8)(lhs_lo + 1));
        m6502_sbc_zpx((u8)(rhs_lo + 1));
        if (op == OP_UGE || op == OP_ULE) {
            emit8(0xB0); emit8(0x04); /* BCS true */
        } else {
            emit8(0x90); emit8(0x04); /* BCC true */
        }
        m6502_lda_imm(0x00);
        emit8(0xF0); emit8(0x02);     /* BEQ store */
        m6502_lda_imm(0xFF);
        m6502_sta_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_LT || op == OP_GT || op == OP_GE || op == OP_LE) {
        m6502_sec();
        m6502_lda_zpx(lhs_lo);
        m6502_sbc_zpx(rhs_lo);
        m6502_lda_zpx((u8)(lhs_lo + 1));
        m6502_sbc_zpx((u8)(rhs_lo + 1));
        emit8(0x50); emit8(0x02);     /* BVC +2 */
        m6502_eor_imm(0x80);
        if (op == OP_GE || op == OP_LE) {
            emit8(0x10); emit8(0x04); /* BPL true */
        } else {
            emit8(0x30); emit8(0x04); /* BMI true */
        }
        m6502_lda_imm(0x00);
        emit8(0xF0); emit8(0x02);     /* BEQ store */
        m6502_lda_imm(0xFF);
        m6502_sta_zpx(ZP_STACK);
        m6502_sta_zpx(ZP_STACK + 1);
    } else {
        ir4_die("unimplemented 6502 comparison op");
    }
}

static void m6502_mem_op(u8 op) {
    m6502_lda_zpx(ZP_STACK);
    m6502_sta_zp(ZP_SCRATCH);
    m6502_lda_zpx(ZP_STACK + 1);
    m6502_sta_zp(ZP_SCRATCH + 1);
    m6502_ldy_imm(0x00);

    if (op == OP_CFETCH) {
        m6502_lda_indy(ZP_SCRATCH);
        m6502_sta_zpx(ZP_STACK);
        m6502_lda_imm(0x00);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_CSTORE) {
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sta_indy(ZP_SCRATCH);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
    } else if (op == OP_FETCH || op == OP_AFETCH) {
        m6502_lda_indy(ZP_SCRATCH);
        m6502_sta_zpx(ZP_STACK);
        emit8(0xC8); /* INY */
        m6502_lda_indy(ZP_SCRATCH);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_STORE || op == OP_ASTORE) {
        m6502_lda_zpx(ZP_STACK + 2);
        m6502_sta_indy(ZP_SCRATCH);
        emit8(0xC8); /* INY */
        m6502_lda_zpx(ZP_STACK + 3);
        m6502_sta_indy(ZP_SCRATCH);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
    } else {
        ir4_die("unimplemented 6502 float memory op");
    }
}

static void m6502_conv_op(u8 op) {
    if (op == OP_SEXT) {
        m6502_lda_imm(0x00);
        m6502_ldy_zpx(ZP_STACK);
        emit8(0x10); emit8(0x02); /* BPL +2 */
        m6502_lda_imm(0xFF);
        m6502_sta_zpx(ZP_STACK + 1);
    } else if (op == OP_W2A || op == OP_A2W) {
        /* Zero-cost no-op on 6502 */
    } else {
        ir4_die("unimplemented 6502 float conversion op");
    }
}

static u16 m6502_emit_jz_fwd(void) {
    u16 patch_pos;
    m6502_inx();
    m6502_inx();
    m6502_lda_zpx((u8)(ZP_STACK - 2));
    m6502_ora_zpx((u8)(ZP_STACK - 1));
    emit8(0xD0); emit8(0x03); /* BNE +3 */
    emit8(0x4C);              /* JMP abs */
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static u16 m6502_emit_jmp_fwd(void) {
    u16 patch_pos;
    emit8(0x4C);
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static void m6502_emit_jmp_back(u16 target_addr) {
    m6502_jmp(target_addr);
}

static void m6502_emit_jz_back(u16 target_addr) {
    m6502_inx();
    m6502_inx();
    m6502_lda_zpx((u8)(ZP_STACK - 2));
    m6502_ora_zpx((u8)(ZP_STACK - 1));
    emit8(0xD0); emit8(0x03); /* BNE +3 */
    m6502_jmp(target_addr);
}

static void m6502_patch_fwd(u16 patch_offset) {
    patch16(patch_offset, cb_cur_addr());
}

const Backend be_6502 = {
    "6502",
    2, 2, 5,
    m6502_cur_addr,
    m6502_fn_begin,
    m6502_fn_end,
    m6502_call,
    m6502_push_w,
    m6502_push_a,
    m6502_push_data,
    m6502_local_get,
    m6502_local_set,
    m6502_stack_op,
    m6502_alu_op,
    m6502_cmp_op,
    m6502_mem_op,
    m6502_conv_op,
    m6502_emit_jz_fwd,
    m6502_emit_jmp_fwd,
    m6502_emit_jmp_back,
    m6502_emit_jz_back,
    m6502_patch_fwd
};

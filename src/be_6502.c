/* be_6502.c - 6502 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: 256-byte BSS buffer in normal RAM indexed by X (wraps in 0..255 via DEX/DEX)
 *   - Locals: 64-byte BSS buffer in normal RAM (2 bytes per slot, preserved on HW stack)
 *   - Scratch pointers: 4 Zero Page bytes at zp_scratch (default $FB..$FE; override via --zp)
 */
#include "ir4c.h"

static u8  zp_scratch      = 0xFB;
static u16 stack_data_off  = 0;
static u16 locals_data_off = 0;
static int data_inited     = 0;

void be_6502_set_zp(u8 zp) {
    zp_scratch = zp;
}

static void ensure_data_alloc(void) {
    if (!data_inited) {
        stack_data_off  = cb_alloc_data(256, 1);
        locals_data_off = cb_alloc_data(64, 1);
        data_inited = 1;
    }
}

u16 be_6502_stack_data_off(void) {
    ensure_data_alloc();
    return stack_data_off;
}

static void emit_stack_op(u8 opcode, u8 disp) {
    ensure_data_alloc();
    emit8(opcode);
    cb_add_data_reloc(cb_pc(), (u16)(stack_data_off + disp));
    emit16(0x0000);
}

static void emit_local_op(u8 opcode, u8 byte_off) {
    ensure_data_alloc();
    emit8(opcode);
    cb_add_data_reloc(cb_pc(), (u16)(locals_data_off + byte_off));
    emit16(0x0000);
}

/* --- 6502 Raw Opcode Helpers --- */
static void m6502_clc(void)        { emit8(0x18); }
static void m6502_sec(void)        { emit8(0x38); }
static void m6502_pha(void)        { emit8(0x48); }
static void m6502_pla(void)        { emit8(0x68); }
static void m6502_php(void)        { emit8(0x08); }
static void m6502_plp(void)        { emit8(0x28); }
static void m6502_dex(void)        { emit8(0xCA); }
static void m6502_inx(void)        { emit8(0xE8); }
static void m6502_tya(void)        { emit8(0x98); }
static void m6502_rts(void)        { emit8(0x60); }

static void m6502_lda_imm(u8 v)    { emit8(0xA9); emit8(v); }
static void m6502_ldy_imm(u8 v)    { emit8(0xA0); emit8(v); }
static void m6502_lda_zp(u8 zp)    { emit8(0xA5); emit8(zp); }
static void m6502_sta_zp(u8 zp)    { emit8(0x85); emit8(zp); }
static void m6502_eor_imm(u8 v)    { emit8(0x49); emit8(v); }

static void m6502_lda_local(u8 o)  { emit_local_op(0xAD, o); }
static void m6502_sta_local(u8 o)  { emit_local_op(0x8D, o); }

static void m6502_lda_stk(u8 d)    { emit_stack_op(0xBD, d); }
static void m6502_sta_stk(u8 d)    { emit_stack_op(0x9D, d); }
static void m6502_ldy_stk(u8 d)    { emit_stack_op(0xBC, d); }
static void m6502_adc_stk(u8 d)    { emit_stack_op(0x7D, d); }
static void m6502_sbc_stk(u8 d)    { emit_stack_op(0xFD, d); }
static void m6502_and_stk(u8 d)    { emit_stack_op(0x3D, d); }
static void m6502_ora_stk(u8 d)    { emit_stack_op(0x1D, d); }
static void m6502_eor_stk(u8 d)    { emit_stack_op(0x5D, d); }
static void m6502_asl_stk(u8 d)    { emit_stack_op(0x1E, d); }
static void m6502_lsr_stk(u8 d)    { emit_stack_op(0x5E, d); }
static void m6502_ror_stk(u8 d)    { emit_stack_op(0x7E, d); }

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

    /* Preserve caller's local slots on hardware stack for reentrancy */
    for (i = 0; i < total_locals; i++) {
        u8 off = (u8)(i * 2);
        m6502_lda_local(off);
        m6502_pha();
        m6502_lda_local((u8)(off + 1));
        m6502_pha();
    }

    /* Pop incoming parameters from data stack into locals */
    for (i = param_cnt; i > 0; i--) {
        u8 off = (u8)((i - 1) * 2);
        m6502_lda_stk(0);
        m6502_sta_local(off);
        m6502_lda_stk(1);
        m6502_sta_local((u8)(off + 1));
        m6502_inx();
        m6502_inx();
    }

    /* Zero-initialize scratch locals after '|' */
    if (total_locals > param_cnt) {
        m6502_lda_imm(0x00);
        for (i = param_cnt; i < total_locals; i++) {
            u8 off = (u8)(i * 2);
            m6502_sta_local(off);
            m6502_sta_local((u8)(off + 1));
        }
    }
}

static void m6502_fn_end(const FuncSig *sig, const LocalVar *locals, u8 total_locals) {
    u8 i;
    (void)sig;
    (void)locals;

    /* Restore caller's local slots in reverse order */
    for (i = total_locals; i > 0; i--) {
        u8 off = (u8)((i - 1) * 2);
        m6502_pla();
        m6502_sta_local((u8)(off + 1));
        m6502_pla();
        m6502_sta_local(off);
    }

    m6502_rts();
}

static void m6502_call(u16 target_addr) {
    m6502_jsr(target_addr);
}

static void m6502_call_extern(const FuncSig *sig) {
    u8 i;
    u8 has_a = 0, has_x = 0, has_y = 0;
    if (!sig->has_addr) ir4_die("6502 extern requires a fixed address");
    if (sig->out_count > 1) ir4_die("6502 extern supports at most 1 return value");

    for (i = sig->in_count; i > 0; i--) {
        const char *r = sig->in_regs[i - 1];
        m6502_lda_stk(0);
        if (strcmp(r, "A") == 0) {
            m6502_sta_zp(zp_scratch);
            has_a = 1;
        } else if (strcmp(r, "X") == 0) {
            m6502_sta_zp((u8)(zp_scratch + 1));
            has_x = 1;
        } else if (strcmp(r, "Y") == 0) {
            m6502_sta_zp((u8)(zp_scratch + 2));
            has_y = 1;
        } else {
            ir4_die("6502 extern only supports A, X, and Y registers");
        }
        m6502_inx(); m6502_inx();
    }

    emit8(0x86); emit8((u8)(zp_scratch + 3)); /* STX zp_scratch+3 */
    if (has_a) m6502_lda_zp(zp_scratch);
    if (has_x) { emit8(0xA6); emit8((u8)(zp_scratch + 1)); } /* LDX */
    if (has_y) { emit8(0xA4); emit8((u8)(zp_scratch + 2)); } /* LDY */

    m6502_jsr(sig->addr);

    if (sig->out_count == 1) {
        const char *r = sig->out_reg;
        if (strcmp(r, "X") == 0) {
            emit8(0x8A); /* TXA */
        } else if (strcmp(r, "Y") == 0) {
            m6502_tya();
        } else if (strcmp(r, "A") != 0) {
            ir4_die("6502 extern return only supports A, X, and Y registers");
        }
        emit8(0xA6); emit8((u8)(zp_scratch + 3)); /* LDX zp_scratch+3 */
        m6502_dex(); m6502_dex();
        m6502_sta_stk(0);
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
    } else {
        emit8(0xA6); emit8((u8)(zp_scratch + 3)); /* LDX zp_scratch+3 */
    }
}

static void m6502_push_w(i32 val) {
    u16 v = (u16)val;
    m6502_dex();
    m6502_dex();
    m6502_lda_imm((u8)(v & 0xFF));
    m6502_sta_stk(0);
    m6502_lda_imm((u8)((v >> 8) & 0xFF));
    m6502_sta_stk(1);
}

static void m6502_push_a(u32 val) {
    m6502_push_w((i32)val);
}

static void m6502_push_f(double val) {
    (void)val;
    ir4_die("unimplemented 6502 float literal");
}

static void m6502_push_data(u16 offset) {
    u16 lo_pos, hi_pos;
    m6502_dex();
    m6502_dex();
    emit8(0xA9); lo_pos = cb_pc(); emit8(0x00); /* LDA #<addr */
    m6502_sta_stk(0);
    emit8(0xA9); hi_pos = cb_pc(); emit8(0x00); /* LDA #>addr */
    m6502_sta_stk(1);
    cb_add_data_reloc_split(lo_pos, hi_pos, offset);
}

static void m6502_local_get(u8 slot, u8 type, u16 stack_bytes) {
    u8 off = (u8)(slot * 2);
    (void)type;
    (void)stack_bytes;
    m6502_dex();
    m6502_dex();
    m6502_lda_local(off);
    m6502_sta_stk(0);
    m6502_lda_local((u8)(off + 1));
    m6502_sta_stk(1);
}

static void m6502_local_set(u8 slot, u8 type, u16 stack_bytes) {
    u8 off = (u8)(slot * 2);
    (void)type;
    (void)stack_bytes;
    m6502_lda_stk(0);
    m6502_sta_local(off);
    m6502_lda_stk(1);
    m6502_sta_local((u8)(off + 1));
    m6502_inx();
    m6502_inx();
}

static void m6502_local_op(LocalOp op, u8 slot, StackType type, u16 stack_bytes) {
    if (op == LOCAL_GET) m6502_local_get(slot, (u8)type, stack_bytes);
    else                 m6502_local_set(slot, (u8)type, stack_bytes);
}

static void m6502_stack_op(StackOp op, StackType t1, StackType t2) {
    if (t1 == TY_F || t2 == TY_F) {
        ir4_die("unimplemented 6502 float stack op");
    }
    switch (op) {
    case OP_DUP:
        m6502_dex();
        m6502_dex();
        m6502_lda_stk(2);
        m6502_sta_stk(0);
        m6502_lda_stk(3);
        m6502_sta_stk(1);
        break;
    case OP_DROP:
        m6502_inx();
        m6502_inx();
        break;
    case OP_SWAP:
        m6502_ldy_stk(0);
        m6502_lda_stk(2);
        m6502_sta_stk(0);
        m6502_tya();
        m6502_sta_stk(2);

        m6502_ldy_stk(1);
        m6502_lda_stk(3);
        m6502_sta_stk(1);
        m6502_tya();
        m6502_sta_stk(3);
        break;
    case OP_OVER:
        m6502_dex();
        m6502_dex();
        m6502_lda_stk(4);
        m6502_sta_stk(0);
        m6502_lda_stk(5);
        m6502_sta_stk(1);
        break;
    default:
        ir4_die("unimplemented 6502 stack op");
    }
}

static void m6502_int_cmp(IrOp op) {
    u8 lhs_off = 2;
    u8 rhs_off = 0;

    switch (op) {
    case OP_GT:
    case OP_LE:
    case OP_UGT:
    case OP_ULE:
    case OP_AGT:
        lhs_off = 0;
        rhs_off = 2;
        break;
    default:
        break;
    }

    switch (op) {
    case OP_EQ:
    case OP_NE:
    case OP_AEQ:
    case OP_ANE:
        m6502_lda_stk(2);
        m6502_eor_stk(0);
        m6502_sta_zp(zp_scratch);
        m6502_lda_stk(3);
        m6502_eor_stk(1);
        emit8(0x05); emit8(zp_scratch); /* ORA zp_scratch */
        if (op == OP_EQ || op == OP_AEQ) {
            emit8(0xF0); emit8(0x04); /* BEQ true */
        } else {
            emit8(0xD0); emit8(0x04); /* BNE true */
        }
        m6502_lda_imm(0x00);
        emit8(0xF0); emit8(0x02);     /* BEQ store */
        m6502_lda_imm(0xFF);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
    case OP_UGE:
    case OP_ULE:
        m6502_sec();
        m6502_lda_stk(lhs_off);
        m6502_sbc_stk(rhs_off);
        m6502_lda_stk((u8)(lhs_off + 1));
        m6502_sbc_stk((u8)(rhs_off + 1));
        if (op == OP_UGE || op == OP_ULE) {
            emit8(0xB0); emit8(0x04); /* BCS true */
        } else {
            emit8(0x90); emit8(0x04); /* BCC true */
        }
        m6502_lda_imm(0x00);
        emit8(0xF0); emit8(0x02);     /* BEQ store */
        m6502_lda_imm(0xFF);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_LT:
    case OP_GT:
    case OP_GE:
    case OP_LE:
        m6502_sec();
        m6502_lda_stk(lhs_off);
        m6502_sbc_stk(rhs_off);
        m6502_lda_stk((u8)(lhs_off + 1));
        m6502_sbc_stk((u8)(rhs_off + 1));
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
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    default:
        ir4_die("unimplemented 6502 comparison op");
    }
}

static void m6502_mem_addr_setup(void) {
    m6502_lda_stk(0);
    m6502_sta_zp(zp_scratch);
    m6502_lda_stk(1);
    m6502_sta_zp((u8)(zp_scratch + 1));
    m6502_ldy_imm(0x00);
}

static void m6502_op(IrOp op) {
    switch (op) {
    case OP_INVERT:
        m6502_lda_stk(0);
        m6502_eor_imm(0xFF);
        m6502_sta_stk(0);
        m6502_lda_stk(1);
        m6502_eor_imm(0xFF);
        m6502_sta_stk(1);
        break;
    case OP_ADD:
    case OP_AADD:
        m6502_clc();
        m6502_lda_stk(2);
        m6502_adc_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_adc_stk(1);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        m6502_sec();
        m6502_lda_stk(2);
        m6502_sbc_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_sbc_stk(1);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_AND:
        m6502_lda_stk(2);
        m6502_and_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_and_stk(1);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_OR:
        m6502_lda_stk(2);
        m6502_ora_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_ora_stk(1);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_XOR:
        m6502_lda_stk(2);
        m6502_eor_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_eor_stk(1);
        m6502_sta_stk(3);
        m6502_inx();
        m6502_inx();
        break;
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        m6502_ldy_stk(0);
        m6502_inx();
        m6502_inx();
        emit8(0xC0); emit8(0x00); /* CPY #0 */
        if (op == OP_LSHIFT) {
            /* Avoid ROL abs,X ($3E) due to a PC-advance bug in sim65 V2.18 */
            emit8(0xF0); emit8(0x0D); /* BEQ +13 */
            m6502_asl_stk(0);
            m6502_lda_stk(1);
            emit8(0x2A);              /* ROL A */
            m6502_sta_stk(1);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF3); /* BNE -13 */
        } else if (op == OP_RSHIFT) {
            emit8(0xF0); emit8(0x09); /* BEQ +9 */
            m6502_lsr_stk(1);
            m6502_ror_stk(0);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF7); /* BNE -9 */
        } else {
            emit8(0xF0); emit8(0x0D); /* BEQ +13 */
            m6502_lda_stk(1);
            emit8(0x0A);              /* ASL A */
            m6502_ror_stk(1);
            m6502_ror_stk(0);
            emit8(0x88);              /* DEY */
            emit8(0xD0); emit8(0xF3); /* BNE -13 */
        }
        break;
    case OP_EQ: case OP_AEQ:
    case OP_NE: case OP_ANE:
    case OP_LT: case OP_GE: case OP_GT: case OP_LE:
    case OP_ULT: case OP_ALT: case OP_UGE: case OP_UGT: case OP_AGT: case OP_ULE:
        m6502_int_cmp(op);
        break;
    case OP_U8FETCH:
        m6502_mem_addr_setup();
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
        break;
    case OP_I8FETCH:
        m6502_mem_addr_setup();
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        emit8_8(0x10, 0x01); /* BPL +1 */
        emit8(0x88);         /* DEY */
        emit8(0x98);         /* TYA */
        m6502_sta_stk(1);
        break;
    case OP_I8STORE:
        m6502_mem_addr_setup();
        m6502_lda_stk(2);
        m6502_sta_indy(zp_scratch);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        m6502_mem_addr_setup();
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        emit8(0xC8); /* INY */
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(1);
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        m6502_mem_addr_setup();
        m6502_lda_stk(2);
        m6502_sta_indy(zp_scratch);
        emit8(0xC8); /* INY */
        m6502_lda_stk(3);
        m6502_sta_indy(zp_scratch);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
        break;
    case OP_SEXT:
        m6502_lda_imm(0x00);
        m6502_ldy_stk(0);
        emit8(0x10); emit8(0x02); /* BPL +2 */
        m6502_lda_imm(0xFF);
        m6502_sta_stk(1);
        break;
    case OP_W2A:
    case OP_A2W:
        /* Zero-cost no-op on 6502 */
        break;
    case OP_MUL:
    case OP_DIV:
    case OP_MOD:
    case OP_UDIV:
    case OP_UMOD:
        ir4_die("unimplemented 6502 ALU op (mul/div require runtime helper)");
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
    case OP_FNEG:
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
    case OP_FFETCH:
    case OP_FSTORE:
    case OP_W2F:
    case OP_F2W:
        ir4_die("unimplemented 6502 float op");
        break;
    }
}

static u16 m6502_emit_br_fwd(BranchCond cond) {
    u16 patch_pos;
    if (cond == BR_JZ) {
        m6502_lda_stk(0);
        m6502_ora_stk(1);
        m6502_php();
        m6502_inx();
        m6502_inx();
        m6502_plp();
        emit8(0xD0); emit8(0x03); /* BNE +3 */
    }
    emit8(0x4C);              /* JMP abs */
    patch_pos = cb_pc();
    emit16(0x0000);
    return patch_pos;
}

static void m6502_emit_br_back(BranchCond cond, u16 target_addr) {
    if (cond == BR_JZ) {
        m6502_lda_stk(0);
        m6502_ora_stk(1);
        m6502_php();
        m6502_inx();
        m6502_inx();
        m6502_plp();
        emit8(0xD0); emit8(0x03); /* BNE +3 */
    }
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
    m6502_call_extern,
    m6502_push_w,
    m6502_push_a,
    m6502_push_f,
    m6502_push_data,
    m6502_local_op,
    m6502_stack_op,
    m6502_op,
    m6502_emit_br_fwd,
    m6502_emit_br_back,
    m6502_patch_fwd
};

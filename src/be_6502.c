/* be_6502.c - 6502 Direct Binary Machine-Code Backend for IR4th (Strict C89)
 * Calling & Register Convention:
 *   - Data stack: 256-byte BSS buffer in normal RAM indexed by X (wraps in 0..255 via DEX/DEX)
 *   - Locals: 256-byte BSS software frame stack indexed by 1-byte BSS lfp (grows downward)
 *   - Scratch pointers: 4 Zero Page bytes at zp_scratch (default $FB..$FE; override via --zp)
 */
#include "codebuf.h"
#include "be_6502_wozfp.h"
#include <math.h>

static u8  zp_scratch      = 0xFB;
static u16 stack_data_off  = 0;
static u16 locals_data_off = 0;
static u16 lfp_data_off    = 0;
static int data_inited     = 0;
static u16 mul16_addr      = 0;
static u16 udivmod16_addr  = 0;
static u16 sdivmod16_addr  = 0;
static u16 wozfp_addr      = 0;

void be_6502_set_zp(u8 zp) {
    zp_scratch = zp;
}

static void ensure_data_alloc(void) {
    if (!data_inited) {
        stack_data_off  = cb_alloc_data(256, 1);
        locals_data_off = cb_alloc_data(256, 1);
        lfp_data_off    = cb_alloc_data(1, 1);
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

static void m6502_lda_local(u8 o)  { emit_local_op(0xB9, o); } /* LDA abs,Y */
static void m6502_sta_local(u8 o)  { emit_local_op(0x99, o); } /* STA abs,Y */
static void m6502_ldy_lfp(void)    { ensure_data_alloc(); emit8(0xAC); cb_add_data_reloc(cb_pc(), lfp_data_off); emit16(0x0000); } /* LDY lfp */

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

static u8 m6502_local_off[MAX_LOCALS];
static u8 m6502_frame_bytes = 0;

static void m6502_fn_begin(const char *name, u8 param_cnt,
                           const LocalVar *locals, u8 total_locals) {
    u8 i;
    (void)name;

    m6502_frame_bytes = 0;
    for (i = 0; i < total_locals; i++) {
        u16 sz = (locals[i].size > 0) ? locals[i].size
                                      : (u16)((locals[i].type == TY_F) ? 4 : 2);
        if ((u16)m6502_frame_bytes + sz > 255) {
            ir4_die("6502 local frame exceeds 255 bytes");
        }
        m6502_local_off[i] = m6502_frame_bytes;
        m6502_frame_bytes += (u8)sz;
    }

    if (m6502_frame_bytes > 0) {
        ensure_data_alloc();
        /* Save caller's lfp and allocate m6502_frame_bytes downward */
        emit8(0xAD); cb_add_data_reloc(cb_pc(), lfp_data_off); emit16(0x0000); /* LDA lfp */
        m6502_pha();
        m6502_sec();
        emit8(0xE9); emit8(m6502_frame_bytes); /* SBC #m6502_frame_bytes */
        emit8(0x8D); cb_add_data_reloc(cb_pc(), lfp_data_off); emit16(0x0000); /* STA lfp */
        emit8(0xA8); /* TAY (Y = new lfp) */
    }

    /* Pop incoming parameters from data stack into locals */
    for (i = param_cnt; i > 0; i--) {
        u8 idx = (u8)(i - 1);
        u8 off = m6502_local_off[idx];
        u8 sz = (locals[idx].type == TY_F) ? 4 : 2;
        u8 b;
        for (b = 0; b < sz; b++) {
            m6502_lda_stk(b);
            m6502_sta_local((u8)(off + b));
        }
        for (b = 0; b < sz; b++) m6502_inx();
    }

    /* Zero-initialize scalar scratch locals after '|' */
    if (total_locals > param_cnt) {
        m6502_lda_imm(0x00);
        for (i = param_cnt; i < total_locals; i++) {
            u8 off = m6502_local_off[i];
            u8 sz = (locals[i].type == TY_F) ? 4 : 2;
            u8 b;
            if (locals[i].size > 0) continue;
            for (b = 0; b < sz; b++) {
                m6502_sta_local((u8)(off + b));
            }
        }
    }
}

static void m6502_fn_end(const FuncSig *sig, const LocalVar *locals, u8 total_locals) {
    (void)sig;
    (void)locals;
    (void)total_locals;

    if (m6502_frame_bytes > 0) {
        ensure_data_alloc();
        m6502_pla();
        emit8(0x8D); cb_add_data_reloc(cb_pc(), lfp_data_off); emit16(0x0000); /* STA lfp */
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

static void encode_wozfp(double val, u8 out[4]) {
    int exp2;
    double m;
    i32 mant;
    if (val == 0.0) {
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
        return;
    }
    m = frexp(val, &exp2); /* val = m * 2^exp2, 0.5 <= |m| < 1.0 */
    mant = (i32)(m * 8388608.0 + (m >= 0.0 ? 0.5 : -0.5));
    exp2 -= 1;
    /* Normalize 24-bit 2's complement mantissa so bits 23 and 22 differ */
    if (mant != 0) {
        while (((mant >> 23) & 1) == ((mant >> 22) & 1)) {
            mant <<= 1;
            exp2 -= 1;
        }
    }
    if (exp2 < -128 || exp2 > 127) {
        ir4_die("6502 wozfp float literal out of range");
    }
    out[0] = (u8)(exp2 + 0x80);
    out[1] = (u8)((mant >> 16) & 0xFF);
    out[2] = (u8)((mant >> 8) & 0xFF);
    out[3] = (u8)(mant & 0xFF);
}

static void m6502_push_f(double val) {
    u8 bytes[4];
    u8 b;
    encode_wozfp(val, bytes);
    for (b = 0; b < 4; b++) m6502_dex();
    for (b = 0; b < 4; b++) {
        m6502_lda_imm(bytes[b]);
        m6502_sta_stk(b);
    }
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
    u8 off = m6502_local_off[slot];
    u8 sz = (type == TY_F) ? 4 : 2;
    u8 b;
    (void)stack_bytes;
    for (b = 0; b < sz; b++) m6502_dex();
    m6502_ldy_lfp();
    for (b = 0; b < sz; b++) {
        m6502_lda_local((u8)(off + b));
        m6502_sta_stk(b);
    }
}

static void m6502_local_set(u8 slot, u8 type, u16 stack_bytes) {
    u8 off = m6502_local_off[slot];
    u8 sz = (type == TY_F) ? 4 : 2;
    u8 b;
    (void)stack_bytes;
    m6502_ldy_lfp();
    for (b = 0; b < sz; b++) {
        m6502_lda_stk(b);
        m6502_sta_local((u8)(off + b));
    }
    for (b = 0; b < sz; b++) m6502_inx();
}

static void m6502_local_addr(u8 slot) {
    u16 lo_pos, hi_pos;
    ensure_data_alloc();
    m6502_dex(); m6502_dex();
    m6502_clc();
    emit8(0xA9); lo_pos = cb_pc(); emit8(0x00); /* LDA #<local */
    emit8(0x6D); cb_add_data_reloc(cb_pc(), lfp_data_off); emit16(0x0000); /* ADC lfp */
    m6502_sta_stk(0);
    emit8(0xA9); hi_pos = cb_pc(); emit8(0x00); /* LDA #>local */
    emit8(0x69); emit8(0x00); /* ADC #0 */
    m6502_sta_stk(1);
    cb_add_data_reloc_split(lo_pos, hi_pos, (u16)(locals_data_off + m6502_local_off[slot]));
}

static void m6502_local_op(LocalOp op, u8 slot, StackType type, u16 stack_bytes) {
    if (op == LOCAL_GET)       m6502_local_get(slot, (u8)type, stack_bytes);
    else if (op == LOCAL_ADDR) m6502_local_addr(slot);
    else                       m6502_local_set(slot, (u8)type, stack_bytes);
}

static void m6502_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 sz1 = (t1 == TY_F) ? 4 : 2;
    u8 sz2 = (t2 == TY_F) ? 4 : 2;
    u8 b;
    switch (op) {
    case OP_DUP:
        for (b = 0; b < sz1; b++) m6502_dex();
        for (b = 0; b < sz1; b++) {
            m6502_lda_stk((u8)(sz1 + b));
            m6502_sta_stk(b);
        }
        break;
    case OP_DROP:
        for (b = 0; b < sz1; b++) m6502_inx();
        break;
    case OP_SWAP:
        if (sz1 == sz2) {
            for (b = 0; b < sz1; b++) {
                m6502_ldy_stk(b);
                m6502_lda_stk((u8)(sz1 + b));
                m6502_sta_stk(b);
                m6502_tya();
                m6502_sta_stk((u8)(sz1 + b));
            }
        } else {
            /* Save TOS (sz1 <= 4 bytes) in zp_scratch, shift NOS down, store TOS */
            for (b = 0; b < sz1; b++) {
                m6502_lda_stk(b);
                m6502_sta_zp((u8)(zp_scratch + b));
            }
            for (b = 0; b < sz2; b++) {
                m6502_lda_stk((u8)(sz1 + b));
                m6502_sta_stk(b);
            }
            for (b = 0; b < sz1; b++) {
                m6502_lda_zp((u8)(zp_scratch + b));
                m6502_sta_stk((u8)(sz2 + b));
            }
        }
        break;
    case OP_OVER:
        for (b = 0; b < sz2; b++) m6502_dex();
        for (b = 0; b < sz2; b++) {
            m6502_lda_stk((u8)(sz2 + sz1 + b));
            m6502_sta_stk(b);
        }
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

static void m6502_mem_addr_setup_off(u8 y_off) {
    m6502_lda_stk(0);
    m6502_sta_zp(zp_scratch);
    m6502_lda_stk(1);
    m6502_sta_zp((u8)(zp_scratch + 1));
    m6502_ldy_imm(y_off);
}

static void emit_neg_stk(u8 disp) {
    m6502_sec();
    m6502_lda_imm(0x00);
    m6502_sbc_stk(disp);
    m6502_sta_stk(disp);
    m6502_lda_imm(0x00);
    m6502_sbc_stk((u8)(disp + 1));
    m6502_sta_stk((u8)(disp + 1));
}

static void emit_neg_zp(u8 zp) {
    m6502_sec();
    m6502_lda_imm(0x00);
    emit8(0xE5); emit8(zp);
    m6502_sta_zp(zp);
    m6502_lda_imm(0x00);
    emit8(0xE5); emit8((u8)(zp + 1));
    m6502_sta_zp((u8)(zp + 1));
}

static void ensure_mul16(void) {
    u16 skip_patch, loop_pc, bcc_patch;
    if (mul16_addr) return;
    emit8(0x4C); skip_patch = cb_pc(); emit16(0x0000);
    mul16_addr = cb_cur_addr();

    /* prod (zp0/zp1) = 0, Y = 16 */
    m6502_lda_imm(0x00);
    m6502_sta_zp(zp_scratch);
    m6502_sta_zp((u8)(zp_scratch + 1));
    m6502_ldy_imm(16);

    loop_pc = cb_pc();
    m6502_lsr_stk(1);
    m6502_ror_stk(0);
    emit8(0x90); bcc_patch = cb_pc(); emit8(0x00); /* BCC skip_add */
    m6502_clc();
    m6502_lda_zp(zp_scratch);
    m6502_adc_stk(2);
    m6502_sta_zp(zp_scratch);
    m6502_lda_zp((u8)(zp_scratch + 1));
    m6502_adc_stk(3);
    m6502_sta_zp((u8)(zp_scratch + 1));
    patch8(bcc_patch, (u8)(cb_pc() - (bcc_patch + 1)));

    m6502_asl_stk(2);
    m6502_lda_stk(3);
    emit8(0x2A); /* ROL A (avoids $3E sim65 bug) */
    m6502_sta_stk(3);
    emit8(0x88); /* DEY */
    emit8(0xD0); emit8((u8)(loop_pc - (cb_pc() + 1)));

    m6502_lda_zp(zp_scratch);
    m6502_sta_stk(2);
    m6502_lda_zp((u8)(zp_scratch + 1));
    m6502_sta_stk(3);
    m6502_inx();
    m6502_inx();
    m6502_rts();

    patch16(skip_patch, cb_cur_addr());
}

static void ensure_udivmod16(void) {
    u16 skip_patch, loop_pc, bcc_patch;
    if (udivmod16_addr) return;
    emit8(0x4C); skip_patch = cb_pc(); emit16(0x0000);
    udivmod16_addr = cb_cur_addr();

    /* rem (zp0/zp1) = 0, Y = 16; quotient in stack+2/3,X */
    m6502_lda_imm(0x00);
    m6502_sta_zp(zp_scratch);
    m6502_sta_zp((u8)(zp_scratch + 1));
    m6502_ldy_imm(16);

    loop_pc = cb_pc();
    m6502_asl_stk(2);
    m6502_lda_stk(3);
    emit8(0x2A); /* ROL A */
    m6502_sta_stk(3);
    emit8(0x26); emit8(zp_scratch);             /* ROL zp0 */
    emit8(0x26); emit8((u8)(zp_scratch + 1));   /* ROL zp1 */

    m6502_sec();
    m6502_lda_zp(zp_scratch);
    m6502_sbc_stk(0);
    m6502_sta_zp((u8)(zp_scratch + 2));
    m6502_lda_zp((u8)(zp_scratch + 1));
    m6502_sbc_stk(1);
    emit8(0x90); bcc_patch = cb_pc(); emit8(0x00); /* BCC skip_sub */
    m6502_sta_zp((u8)(zp_scratch + 1));
    m6502_lda_zp((u8)(zp_scratch + 2));
    m6502_sta_zp(zp_scratch);
    emit_stack_op(0xFE, 2);                     /* INC stack+2,X */
    patch8(bcc_patch, (u8)(cb_pc() - (bcc_patch + 1)));

    emit8(0x88); /* DEY */
    emit8(0xD0); emit8((u8)(loop_pc - (cb_pc() + 1)));
    m6502_rts();

    patch16(skip_patch, cb_cur_addr());
}

static void ensure_sdivmod16(void) {
    u16 skip_patch, p1, p2, p3, p4;
    if (sdivmod16_addr) return;
    ensure_udivmod16();
    emit8(0x4C); skip_patch = cb_pc(); emit16(0x0000);
    sdivmod16_addr = cb_cur_addr();

    /* Save dividend sign (zp3) and quotient sign (HW stack) */
    m6502_lda_stk(3);
    m6502_sta_zp((u8)(zp_scratch + 3));
    m6502_eor_stk(1);
    m6502_pha();

    /* If dividend < 0, negate stack+2/3,X */
    m6502_lda_stk(3);
    emit8(0x10); p1 = cb_pc(); emit8(0x00);
    emit_neg_stk(2);
    patch8(p1, (u8)(cb_pc() - (p1 + 1)));

    /* If divisor < 0, negate stack+0/1,X */
    m6502_lda_stk(1);
    emit8(0x10); p2 = cb_pc(); emit8(0x00);
    emit_neg_stk(0);
    patch8(p2, (u8)(cb_pc() - (p2 + 1)));

    m6502_lda_zp((u8)(zp_scratch + 3));
    m6502_pha();
    m6502_jsr(udivmod16_addr);

    /* Apply dividend sign to remainder (zp0/zp1) */
    m6502_pla();
    emit8(0x10); p3 = cb_pc(); emit8(0x00);
    emit_neg_zp(zp_scratch);
    patch8(p3, (u8)(cb_pc() - (p3 + 1)));

    /* Apply quotient sign to quotient (stack+2/3,X) */
    m6502_pla();
    emit8(0x10); p4 = cb_pc(); emit8(0x00);
    emit_neg_stk(2);
    patch8(p4, (u8)(cb_pc() - (p4 + 1)));

    m6502_rts();
    patch16(skip_patch, cb_cur_addr());
}

static void ensure_wozfp(void) {
    u16 jmp_skip;
    u16 base_pc;
    u16 i;
    if (wozfp_addr != 0) return;

    emit8(0x4C); jmp_skip = cb_pc(); emit16(0x0000); /* JMP skip */
    base_pc = cb_pc();
    wozfp_addr = cb_cur_addr();
    for (i = 0; i < (u16)sizeof(be_6502_wozfp); i++) {
        emit8(be_6502_wozfp[i]);
    }
    for (i = 0; i < (u16)BE_6502_WOZFP_RELOC_CNT; i++) {
        u16 r = be_6502_wozfp_relocs[i];
        u16 orig = (u16)be_6502_wozfp[r] | ((u16)be_6502_wozfp[r + 1] << 8);
        patch16((u16)(base_pc + r), (u16)(orig + wozfp_addr));
    }
    patch16(jmp_skip, cb_cur_addr());
}

static void wozfp_pop_to(u8 zp_base) {
    u8 b;
    for (b = 0; b < 4; b++) {
        m6502_lda_stk(b);
        m6502_sta_zp((u8)(zp_base + b));
    }
    for (b = 0; b < 4; b++) m6502_inx();
}

static void wozfp_push_from(u8 zp_base) {
    u8 b;
    for (b = 0; b < 4; b++) m6502_dex();
    for (b = 0; b < 4; b++) {
        m6502_lda_zp((u8)(zp_base + b));
        m6502_sta_stk(b);
    }
}

static void wozfp_call_keep_x(u16 entry_off) {
    emit8(0x86); emit8(zp_scratch); /* STX zp_scratch */
    m6502_jsr((u16)(wozfp_addr + entry_off));
    emit8(0xA6); emit8(zp_scratch); /* LDX zp_scratch (preserves A) */
}

static void emit_data_abs(u8 opcode, u16 data_offset) {
    emit8(opcode);
    cb_add_data_reloc(cb_pc(), data_offset);
    emit16(0x0000);
}

static int m6502_data_mem(IrOp op, u16 data_offset) {
    switch (op) {
    case OP_U8FETCH:
        m6502_dex(); m6502_dex();
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
        return 1;
    case OP_I8FETCH:
        m6502_dex(); m6502_dex();
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        emit8_8(0x10, 0x04); /* BPL +4 */
        m6502_lda_imm(0xFF);
        emit8_8(0x30, 0x02); /* BMI +2 */
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
        return 1;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        m6502_dex(); m6502_dex();
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        emit_data_abs(0xAD, (u16)(data_offset + 1)); /* LDA abs+1 */
        m6502_sta_stk(1);
        return 1;
    case OP_I8STORE:
        m6502_lda_stk(0);
        emit_data_abs(0x8D, data_offset); /* STA abs */
        m6502_inx(); m6502_inx();
        return 1;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        m6502_lda_stk(0);
        emit_data_abs(0x8D, data_offset); /* STA abs */
        m6502_lda_stk(1);
        emit_data_abs(0x8D, (u16)(data_offset + 1)); /* STA abs+1 */
        m6502_inx(); m6502_inx();
        return 1;
    default:
        return 0;
    }
}

static int m6502_mem_imm(IrOp op, i32 off) {
    i32 sz;
    int swapped = (op >= OP_SI8STORE && op <= OP_SFSTORE);
    u8 val_off;
    switch (op) {
    case OP_U8FETCH: case OP_I8FETCH: case OP_I8STORE: case OP_SI8STORE: sz = 1; break;
    case OP_FETCH: case OP_AFETCH: case OP_I16FETCH: case OP_U16FETCH:
    case OP_STORE: case OP_ASTORE: case OP_I16STORE:
    case OP_SSTORE: case OP_SASTORE: case OP_SI16STORE:                 sz = 2; break;
    case OP_FFETCH: case OP_FSTORE: case OP_SFSTORE:                    sz = 4; break;
    default: return 0;
    }
    if (off < 0 || off > 256 - sz) return 0;

    if (swapped) {
        u8 aoff = (u8)((sz == 4) ? 4 : 2);
        val_off = 0;
        m6502_lda_stk(aoff);
        m6502_sta_zp(zp_scratch);
        m6502_lda_stk((u8)(aoff + 1));
        m6502_sta_zp((u8)(zp_scratch + 1));
        m6502_ldy_imm((u8)off);
    } else {
        val_off = 2;
        m6502_mem_addr_setup_off((u8)off);
    }
    switch (op) {
    case OP_U8FETCH:
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
        break;
    case OP_I8FETCH:
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        emit8_8(0x10, 0x04); /* BPL +4 */
        m6502_lda_imm(0xFF);
        emit8_8(0x30, 0x02); /* BMI +2 */
        m6502_lda_imm(0x00);
        m6502_sta_stk(1);
        break;
    case OP_I8STORE:
    case OP_SI8STORE:
        m6502_lda_stk(val_off);
        m6502_sta_indy(zp_scratch);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(0);
        emit8(0xC8); /* INY */
        m6502_lda_indy(zp_scratch);
        m6502_sta_stk(1);
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SI16STORE:
        m6502_lda_stk(val_off);
        m6502_sta_indy(zp_scratch);
        emit8(0xC8); /* INY */
        m6502_lda_stk((u8)(val_off + 1));
        m6502_sta_indy(zp_scratch);
        m6502_inx(); m6502_inx();
        m6502_inx(); m6502_inx();
        break;
    case OP_FFETCH: {
        u8 b;
        m6502_dex();
        m6502_dex();
        for (b = 0; b < 4; b++) {
            m6502_lda_indy(zp_scratch);
            m6502_sta_stk(b);
            if (b < 3) emit8(0xC8); /* INY */
        }
        break;
    }
    case OP_FSTORE:
    case OP_SFSTORE: {
        u8 b;
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(val_off + b));
            m6502_sta_indy(zp_scratch);
            if (b < 3) emit8(0xC8); /* INY */
        }
        for (b = 0; b < 6; b++) m6502_inx();
        break;
    }
    default:
        break;
    }
    return 1;
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
    case OP_I8FETCH:
    case OP_I8STORE:
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
    case OP_SI8STORE:
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
        m6502_mem_imm(op, 0);
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
        ensure_mul16();
        m6502_jsr(mul16_addr);
        break;
    case OP_UDIV:
        ensure_udivmod16();
        m6502_jsr(udivmod16_addr);
        m6502_inx();
        m6502_inx();
        break;
    case OP_UMOD:
        ensure_udivmod16();
        m6502_jsr(udivmod16_addr);
        m6502_inx();
        m6502_inx();
        m6502_lda_zp(zp_scratch);
        m6502_sta_stk(0);
        m6502_lda_zp((u8)(zp_scratch + 1));
        m6502_sta_stk(1);
        break;
    case OP_DIV:
        ensure_sdivmod16();
        m6502_jsr(sdivmod16_addr);
        m6502_inx();
        m6502_inx();
        break;
    case OP_MOD:
        ensure_sdivmod16();
        m6502_jsr(sdivmod16_addr);
        m6502_inx();
        m6502_inx();
        m6502_lda_zp(zp_scratch);
        m6502_sta_stk(0);
        m6502_lda_zp((u8)(zp_scratch + 1));
        m6502_sta_stk(1);
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        u16 entry = (op == OP_FADD) ? RT_FP_ADD :
                    (op == OP_FSUB) ? RT_FP_SUB :
                    (op == OP_FMUL) ? RT_FP_MUL : RT_FP_DIV;
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1); /* rhs -> X1/M1 */
        wozfp_pop_to(WOZFP_ZP_X2); /* lhs -> X2/M2 */
        wozfp_call_keep_x(entry);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    }
    case OP_FNEG:
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1);
        wozfp_call_keep_x(RT_FP_NEG);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1); /* rhs -> X1/M1 */
        wozfp_pop_to(WOZFP_ZP_X2); /* lhs -> X2/M2 */
        wozfp_call_keep_x(RT_FP_CMP);
        /* A = $FF if lhs<rhs, 0 if lhs==rhs, 1 if lhs>rhs */
        m6502_dex();
        m6502_dex();
        if (op == OP_FEQ) {
            emit8(0xC9); emit8(0x00); /* CMP #0 */
            emit8(0xF0); emit8(0x04); /* BEQ true */
            m6502_lda_imm(0x00);
            emit8(0xF0); emit8(0x02); /* BEQ store */
            m6502_lda_imm(0xFF);
        } else if (op == OP_FNE) {
            emit8(0xC9); emit8(0x00); /* CMP #0 */
            emit8(0xD0); emit8(0x04); /* BNE true */
            m6502_lda_imm(0x00);
            emit8(0xF0); emit8(0x02); /* BEQ store */
            m6502_lda_imm(0xFF);
        } else if (op == OP_FLT) {
            emit8(0xC9); emit8(0xFF); /* CMP #$FF */
            emit8(0xF0); emit8(0x04); /* BEQ true */
            m6502_lda_imm(0x00);
            emit8(0xF0); emit8(0x02); /* BEQ store */
            m6502_lda_imm(0xFF);
        } else { /* OP_FGT */
            emit8(0xC9); emit8(0x01); /* CMP #1 */
            emit8(0xF0); emit8(0x04); /* BEQ true */
            m6502_lda_imm(0x00);
            emit8(0xF0); emit8(0x02); /* BEQ store */
            m6502_lda_imm(0xFF);
        }
        m6502_sta_stk(0);
        m6502_sta_stk(1);
        break;
    case OP_FFETCH:
    case OP_FSTORE:
        m6502_mem_imm(op, 0);
        break;
    case OP_W2F:
        ensure_wozfp();
        /* Pop 16-bit signed (lo, hi) -> M1+1 (lo), M1 (hi) */
        m6502_lda_stk(0);
        m6502_sta_zp((u8)(WOZFP_ZP_M1 + 1));
        m6502_lda_stk(1);
        m6502_sta_zp(WOZFP_ZP_M1);
        m6502_inx();
        m6502_inx();
        wozfp_call_keep_x(RT_FP_ITOF);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    case OP_F2W: {
        u16 skip_neg1;
        u16 skip_neg2;
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1);
        /* Truncate toward zero: if M1 < 0, negate before FIX and negate result */
        m6502_lda_zp(WOZFP_ZP_M1);
        m6502_pha();
        emit8(0x10); skip_neg1 = cb_pc(); emit8(0x00); /* BPL skip_neg1 */
        wozfp_call_keep_x(RT_FP_NEG);
        patch8(skip_neg1, (u8)(cb_pc() - (skip_neg1 + 1)));
        wozfp_call_keep_x(RT_FP_FTOI);
        m6502_dex();
        m6502_dex();
        m6502_lda_zp((u8)(WOZFP_ZP_M1 + 1));
        m6502_sta_stk(0);
        m6502_lda_zp(WOZFP_ZP_M1);
        m6502_sta_stk(1);
        m6502_pla();
        emit8(0x10); skip_neg2 = cb_pc(); emit8(0x00); /* BPL skip_neg2 */
        emit_neg_stk(0);
        patch8(skip_neg2, (u8)(cb_pc() - (skip_neg2 + 1)));
        break;
    }
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

static int m6502_add_imm(i32 imm) {
    u16 d = (u16)(i16)imm;
    if (d == 0) return 1;
    m6502_clc();
    m6502_lda_stk(0);
    emit8_8(0x69, (u8)(d & 0xFF)); /* ADC #imm_lo */
    m6502_sta_stk(0);
    m6502_lda_stk(1);
    emit8_8(0x69, (u8)(d >> 8));   /* ADC #imm_hi */
    m6502_sta_stk(1);
    return 1;
}

static void m6502_patch_call(u16 call_pc, u16 call_addr, u16 target_addr) {
    (void)call_addr;
    patch16((u16)(call_pc + 1), target_addr);
}

const Backend be_6502 = {
    "6502",
    2, 2, 4,
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
    m6502_add_imm,
    NULL,
    m6502_mem_imm,
    m6502_data_mem,
    m6502_emit_br_fwd,
    m6502_emit_br_back,
    m6502_patch_fwd,
    m6502_patch_call
};

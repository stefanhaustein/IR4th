/* be_ez80.c - eZ80 ADL (24-bit) Direct Binary Backend for IR4th (Strict C89)
 * Calling & Register Convention (ADL = 1):
 *   - Data stack: hardware SP (3 bytes per :i/:a slot, 6 bytes per :f/i32 slot)
 *   - Return stack: dedicated 24-bit BSS stack pointed to by BSS rsp_off
 *   - Locals: static BSS for non-recursive functions; SP-relative for recursive
 */
#include "codebuf.h"
#include "vec.h"

static size_t ez_rsp_off = 0;
static size_t ez_rstack_off = 0;
static size_t ez_tmp_off = 0;
static int ez_rt_inited = 0;

static size_t ez_mul24_addr = 0;
static size_t ez_udivmod24_addr = 0;
static size_t ez_sdivmod24_addr = 0;
static size_t ez_shl24_addr = 0;
static size_t ez_shr24_addr = 0;
static size_t ez_ashr24_addr = 0;

static void ez_ensure_rt_data(void) {
    if (!ez_rt_inited) {
        ez_rstack_off = cb_alloc_data(256 * 3, 1);
        ez_rsp_off = cb_alloc_data(3, 1);
        ez_tmp_off = cb_alloc_data(6, 1);
        ez_rt_inited = 1;
    }
}

size_t be_ez80_rsp_off(void) {
    ez_ensure_rt_data();
    return ez_rsp_off;
}

size_t be_ez80_rstack_top_off(void) {
    ez_ensure_rt_data();
    return ez_rstack_off + 256 * 3;
}

static size_t ez_cur_addr(void) { return cb_cur_addr(); }

static void emit8_24(u8 op, u32 val) {
    emit8(op);
    emit24(val);
}

static void ez_ld_hl_data(size_t data_off) {
    emit8(0x21); /* LD HL, imm24 */
    cb_add_data_reloc24(cb_pc(), data_off);
    emit24(0);
}

static void ez_ld_hl_ind_data(size_t data_off) {
    emit8(0x2A); /* LD HL, (addr24) */
    cb_add_data_reloc24(cb_pc(), data_off);
    emit24(0);
}

static void ez_ld_data_hl(size_t data_off) {
    emit8(0x22); /* LD (addr24), HL */
    cb_add_data_reloc24(cb_pc(), data_off);
    emit24(0);
}

static size_t ez_stack_bytes = 0;

static void ez_push_hl(void) {
    emit8(0xE5);
    ez_stack_bytes += 3;
}

static void ez_pop_hl(void) {
    emit8(0xE1);
    ez_stack_bytes -= 3;
}

static void ez_pop_de(void) {
    emit8(0xD1);
    ez_stack_bytes -= 3;
}

VEC_OF(SizeVec, size_t);
VEC_OF(U8Vec, u8);
static SizeVec ez_local_off;
static U8Vec ez_is_static;
static size_t ez_frame_bytes = 0;
static size_t ez_static_base = 0;

static void ez_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    size_t off;
    int is4 = (type == TY_F || type == TY_I32);
    (void)stack_bytes;

    if (ez_is_static.data[slot]) {
        size_t addr_off = ez_static_base + ez_local_off.data[slot];
        if (op == LOCAL_ADDR) {
            ez_ld_hl_data(addr_off);
            ez_push_hl();
        } else if (op == LOCAL_GET) {
            if (is4) {
                ez_ld_hl_ind_data(addr_off + 3);
                ez_push_hl(); /* upper byte + padding */
                ez_ld_hl_ind_data(addr_off);
                ez_push_hl();
            } else {
                ez_ld_hl_ind_data(addr_off);
                ez_push_hl();
            }
        } else {
            if (is4) {
                ez_pop_hl();
                ez_ld_data_hl(addr_off);
                ez_pop_hl();
                ez_ld_data_hl(addr_off + 3);
            } else {
                ez_pop_hl();
                ez_ld_data_hl(addr_off);
            }
        }
        return;
    }

    off = ez_local_off.data[slot] + ez_stack_bytes;
    if (op == LOCAL_ADDR) {
        emit8_24(0x21, (u32)off); /* LD HL, off */
        emit8(0x39);              /* ADD HL, SP */
        ez_push_hl();
    } else if (op == LOCAL_GET) {
        if (is4) {
            emit8_24(0x21, (u32)(off + 3));
            emit8(0x39);
            emit8_8(0xED, 0x27); /* LD HL, (HL) */
            ez_push_hl();
            emit8_24(0x21, (u32)(off + 3));
            emit8(0x39);
            emit8_8(0xED, 0x27);
            ez_push_hl();
        } else {
            emit8_24(0x21, (u32)off);
            emit8(0x39);
            emit8_8(0xED, 0x27); /* LD HL, (HL) */
            ez_push_hl();
        }
    } else {
        if (is4) {
            ez_pop_de();
            emit8_24(0x21, (u32)(off - 3));
            emit8(0x39);
            emit8_8(0xED, 0x1F); /* LD (HL), DE */
            ez_pop_de();
            emit8_24(0x21, (u32)off);
            emit8(0x39);
            emit8_8(0xED, 0x1F);
        } else {
            ez_pop_de();
            emit8_24(0x21, (u32)off);
            emit8(0x39);
            emit8_8(0xED, 0x1F); /* LD (HL), DE */
        }
    }
}

static void ez_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                        size_t total_locals, Bool is_recursive) {
    size_t i;
    (void)name;
    ez_ensure_rt_data();
    ez_stack_bytes = 0;

    /* Save 3-byte return address from SP onto BSS rstack */
    emit8(0xD1); /* POP DE */
    ez_ld_hl_ind_data(ez_rsp_off);
    emit8(0x2B); /* DEC HL */
    emit8(0x2B);
    emit8(0x2B);
    ez_ld_data_hl(ez_rsp_off);
    emit8_8(0xED, 0x1F); /* LD (HL), DE */

    VEC_ENSURE(ez_local_off, total_locals);
    VEC_ENSURE(ez_is_static, total_locals);
    ez_frame_bytes = 0;

    for (i = 0; i < total_locals; i++) {
        size_t sz = locals[i].size ? locals[i].size
                                   : ((locals[i].type == TY_F || locals[i].type == TY_I32) ? 6 : 3);
        ez_local_off.data[i] = ez_frame_bytes;
        ez_is_static.data[i] = (u8)(!is_recursive);
        ez_frame_bytes += sz;
    }

    if (!is_recursive) {
        ez_static_base = ez_frame_bytes ? cb_alloc_data(ez_frame_bytes, 1) : 0;
        for (i = param_cnt; i > 0; i--) {
            ez_local_op(LOCAL_SET, i - 1, locals[i - 1].type, 0);
        }
    } else {
        if (ez_frame_bytes > 0) {
            /* Allocate frame on SP */
            emit8_24(0x21, (u32)(0x1000000UL - (ez_frame_bytes & 0xFFFFFFUL)));
            emit8(0x39); /* ADD HL, SP */
            emit8(0xF9); /* LD SP, HL */
        }
        /* Pop params from above frame into frame slots */
        for (i = param_cnt; i > 0; i--) {
            size_t p = i - 1;
            size_t sz = (locals[p].type == TY_F || locals[p].type == TY_I32) ? 6 : 3;
            size_t src_off = ez_frame_bytes;
            size_t dst_off = ez_local_off.data[p];
            if (sz == 6) {
                emit8_24(0x21, (u32)src_off);
                emit8(0x39);
                emit8_8(0xED, 0x17); /* LD DE, (HL) */
                emit8_24(0x21, (u32)dst_off);
                emit8(0x39);
                emit8_8(0xED, 0x1F); /* LD (HL), DE */
                emit8_24(0x21, (u32)(src_off + 3));
                emit8(0x39);
                emit8_8(0xED, 0x17);
                emit8_24(0x21, (u32)(dst_off + 3));
                emit8(0x39);
                emit8_8(0xED, 0x1F);
            } else {
                emit8_24(0x21, (u32)src_off);
                emit8(0x39);
                emit8_8(0xED, 0x17);
                emit8_24(0x21, (u32)dst_off);
                emit8(0x39);
                emit8_8(0xED, 0x1F);
            }
            /* Collapse parameter slot by moving frame up? Easier: just use SP+off for frame */
            if (sz == 6) {
                emit8(0xE1);
                emit8(0xE1);
            } else {
                emit8(0xE1);
            }
        }
    }
}

static void ez_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    (void)sig;
    (void)locals;
    if (total_locals > 0 && !ez_is_static.data[0] && ez_frame_bytes > 0) {
        /* Deallocate recursive frame */
        if (sig->out_types.len == 1) {
            int out4 = (sig->out_types.data[0] == TY_F || sig->out_types.data[0] == TY_I32);
            if (out4) {
                emit8(0xE1);
                emit8(0xD1);
            } else {
                emit8(0xD1);
            }
            emit8_24(0x21, (u32)ez_frame_bytes);
            emit8(0x39);
            emit8(0xF9);
            if (out4) {
                emit8(0xD5);
                emit8(0xE5);
            } else {
                emit8(0xD5);
            }
        } else {
            emit8_24(0x21, (u32)ez_frame_bytes);
            emit8(0x39);
            emit8(0xF9);
        }
    }

    /* Pop return address from BSS rstack and JP (HL) */
    ez_ld_hl_ind_data(ez_rsp_off);
    emit8_8(0xED, 0x17); /* LD DE, (HL) */
    emit8(0x23);         /* INC HL */
    emit8(0x23);
    emit8(0x23);
    ez_ld_data_hl(ez_rsp_off);
    emit8(0xEB); /* EX DE, HL */
    emit8(0xE9); /* JP (HL) */
}

static void ez_call(size_t target_addr) { emit8_24(0xCD, (u32)target_addr); }

static void ez_call_extern(const FuncSig *sig) {
    if (!sig->has_addr)
        ir4_die("eZ80 extern requires fixed address");
    emit8_24(0xCD, (u32)sig->addr);
}

static void ez_push_w(i32 val) {
    emit8_24(0x21, (u32)val & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_a(u32 val) {
    emit8_24(0x21, val & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_f(double val) {
    float f = (float)val;
    u32 bits;
    memcpy(&bits, &f, 4);
    emit8_24(0x21, (bits >> 24) & 0xFFUL);
    ez_push_hl();
    emit8_24(0x21, bits & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_i32(i32 val) {
    u32 u = (u32)val;
    emit8_24(0x21, (u >> 24) & 0xFFUL);
    ez_push_hl();
    emit8_24(0x21, u & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_data(size_t data_offset) {
    ez_ld_hl_data(data_offset);
    ez_push_hl();
}

static void ez_stack_op(StackOp op, StackType t1, StackType t2) {
    int is4_1 = (t1 == TY_F || t1 == TY_I32);
    int is4_2 = (t2 == TY_F || t2 == TY_I32);
    switch (op) {
    case OP_DUP:
        if (!is4_1) {
            emit8(0xE1);
            emit8(0xE5);
            ez_push_hl();
        } else {
            emit8(0xE1);
            emit8(0xD1);
            emit8(0xD5);
            emit8(0xE5);
            emit8(0xD5);
            emit8(0xE5);
            ez_stack_bytes += 6;
        }
        break;
    case OP_DROP:
        if (!is4_1) {
            ez_pop_hl();
        } else {
            ez_pop_hl();
            ez_pop_hl();
        }
        break;
    case OP_SWAP:
        if (!is4_1 && !is4_2) {
            emit8(0xE1);
            emit8(0xD1);
            emit8(0xE5);
            emit8(0xD5);
        } else {
            ir4_die("eZ80 mixed 6-byte swap not yet implemented");
        }
        break;
    }
}

static void ez_ensure_mul24(void) {
    u16 loop_pc;
    if (ez_mul24_addr != 0)
        return;
    emit8(0xC3);
    loop_pc = (u16)cb_pc();
    emit24(0);
    ez_mul24_addr = cb_cur_addr();
    /* HL = DE * BC (24-bit) */
    emit8_24(0x21, 0); /* LD HL, 0 */
    emit8_8(0x3E, 24); /* LD A, 24 */
    {
        size_t l = cb_cur_addr();
        u16 skip_add;
        emit8(0x29); /* ADD HL, HL */
        emit8(0xEB); /* EX DE, HL */
        emit8(0x29); /* ADD HL, HL */
        emit8(0xEB); /* EX DE, HL */
        emit8(0xD2); /* JP NC, skip */
        skip_add = (u16)cb_pc();
        emit24(0);
        emit8(0x09); /* ADD HL, BC */
        patch24(skip_add, (u32)cb_cur_addr());
        emit8(0x3D); /* DEC A */
        emit8_24(0xC2, (u32)l); /* JP NZ, l */
        emit8(0xC9); /* RET */
    }
    patch24(loop_pc, (u32)cb_cur_addr());
}

static void ez_ensure_udivmod24(void) {
    u16 skip;
    if (ez_udivmod24_addr != 0)
        return;
    emit8(0xC3);
    skip = (u16)cb_pc();
    emit24(0);
    ez_udivmod24_addr = cb_cur_addr();
    /* DE = dividend, BC = divisor -> DE = quotient, HL = remainder */
    emit8_24(0x21, 0); /* LD HL, 0 */
    emit8_8(0x3E, 24); /* LD A, 24 */
    {
        size_t l = cb_cur_addr();
        u16 no_sub;
        emit8(0xEB); /* EX DE, HL */
        emit8(0x29); /* ADD HL, HL */
        emit8(0xEB); /* EX DE, HL */
        emit8(0xED);
        emit8(0x6A); /* ADC HL, HL */
        emit8(0xB7); /* OR A */
        emit8(0xED);
        emit8(0x42); /* SBC HL, BC */
        emit8(0xD2); /* JP NC, + */
        no_sub = (u16)cb_pc();
        emit24(0);
        emit8(0x09); /* ADD HL, BC */
        {
            u16 done = (u16)cb_pc();
            emit8(0xC3);
            done = (u16)cb_pc();
            emit24(0);
            patch24(no_sub, (u32)cb_cur_addr());
            emit8(0x13); /* INC DE */
            patch24(done, (u32)cb_cur_addr());
        }
        emit8(0x3D); /* DEC A */
        emit8_24(0xC2, (u32)l);
        emit8(0xC9);
    }
    patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_sdivmod24(void) {
    u16 skip, pos_a, pos_b, pos_q;
    ez_ensure_udivmod24();
    if (ez_sdivmod24_addr != 0)
        return;
    emit8(0xC3);
    skip = (u16)cb_pc();
    emit24(0);
    ez_sdivmod24_addr = cb_cur_addr();
    /* HL = dividend, DE = divisor -> DE = quotient, HL = remainder */
    ez_ensure_rt_data();
    /* HL=dividend, DE=divisor */
    ez_ld_data_hl(ez_tmp_off);
    emit8(0xEB);
    ez_ld_data_hl(ez_tmp_off + 3);
    /* Sign of dividend = tmp+2 bit 7 */
    emit8(0x3A);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 2);
    emit24(0);
    emit8(0xB7); /* OR A */
    emit8(0xF2); /* JP P, pos_a */
    pos_a = (u16)cb_pc();
    emit24(0);
    ez_ld_hl_ind_data(ez_tmp_off);
    emit8(0xEB);
    emit8_24(0x21, 0);
    emit8(0xB7);
    emit8(0xED);
    emit8(0x52); /* SBC HL, DE */
    ez_ld_data_hl(ez_tmp_off);
    patch24(pos_a, (u32)cb_cur_addr());

    emit8(0x3A);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 5);
    emit24(0);
    emit8(0xB7);
    emit8(0xF2);
    pos_b = (u16)cb_pc();
    emit24(0);
    ez_ld_hl_ind_data(ez_tmp_off + 3);
    emit8(0xEB);
    emit8_24(0x21, 0);
    emit8(0xB7);
    emit8(0xED);
    emit8(0x52);
    ez_ld_data_hl(ez_tmp_off + 3);
    patch24(pos_b, (u32)cb_cur_addr());

    ez_ld_hl_ind_data(ez_tmp_off);
    emit8(0xEB); /* DE = |dividend| */
    ez_ld_hl_ind_data(ez_tmp_off + 3);
    emit8(0xE5); /* PUSH HL */
    emit8(0xC1); /* BC = |divisor| */
    emit8_24(0xCD, (u32)ez_udivmod24_addr);
    /* Check if signs differed */
    emit8(0x3A);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 2);
    emit24(0);
    emit8(0x47); /* LD B, A */
    emit8(0x3A);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 5);
    emit24(0);
    emit8(0xA8); /* XOR B */
    emit8(0xF2);
    pos_q = (u16)cb_pc();
    emit24(0);
    emit8(0xE5); /* save rem */
    emit8_24(0x21, 0);
    emit8(0xB7);
    emit8(0xED);
    emit8(0x52);
    emit8(0xEB); /* DE = -quot */
    emit8(0xE1); /* restore rem */
    patch24(pos_q, (u32)cb_cur_addr());
    emit8(0xC9);
    patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_shifts(void) {
    u16 skip, done1, done2, done3;
    if (ez_shl24_addr != 0)
        return;
    ez_ensure_rt_data();
    emit8(0xC3);
    skip = (u16)cb_pc();
    emit24(0);

    /* shl24: HL << A */
    ez_shl24_addr = cb_cur_addr();
    emit8(0xB7);
    emit8(0xCA);
    done1 = (u16)cb_pc();
    emit24(0);
    {
        size_t l = cb_cur_addr();
        emit8(0x29); /* ADD HL, HL */
        emit8(0x3D);
        emit8_24(0xC2, (u32)l);
    }
    patch24(done1, (u32)cb_cur_addr());
    emit8(0xC9);

    /* shr24: HL >> A (unsigned) */
    ez_shr24_addr = cb_cur_addr();
    emit8(0xB7);
    emit8(0xCA);
    done2 = (u16)cb_pc();
    emit24(0);
    ez_ld_data_hl(ez_tmp_off);
    emit8(0x47); /* LD B, A */
    {
        size_t l = cb_cur_addr();
        ez_ld_hl_data(ez_tmp_off + 2);
        emit8_8(0xCB, 0x3E); /* SRL (HL) */
        emit8(0x2B);
        emit8_8(0xCB, 0x1E); /* RR (HL) */
        emit8(0x2B);
        emit8_8(0xCB, 0x1E); /* RR (HL) */
        emit8(0x10);
        emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
    }
    ez_ld_hl_ind_data(ez_tmp_off);
    patch24(done2, (u32)cb_cur_addr());
    emit8(0xC9);

    /* ashr24: HL >> A (signed) */
    ez_ashr24_addr = cb_cur_addr();
    emit8(0xB7);
    emit8(0xCA);
    done3 = (u16)cb_pc();
    emit24(0);
    ez_ld_data_hl(ez_tmp_off);
    emit8(0x47);
    {
        size_t l = cb_cur_addr();
        ez_ld_hl_data(ez_tmp_off + 2);
        emit8_8(0xCB, 0x2E); /* SRA (HL) */
        emit8(0x2B);
        emit8_8(0xCB, 0x1E); /* RR (HL) */
        emit8(0x2B);
        emit8_8(0xCB, 0x1E); /* RR (HL) */
        emit8(0x10);
        emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
    }
    ez_ld_hl_ind_data(ez_tmp_off);
    patch24(done3, (u32)cb_cur_addr());
    emit8(0xC9);

    patch24(skip, (u32)cb_cur_addr());
}

static void ez_op(IrOp op) {
    switch (op) {
    case OP_ADD:
    case OP_AADD:
        ez_pop_de();
        ez_pop_hl();
        emit8(0x19); /* ADD HL, DE */
        ez_push_hl();
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        ez_pop_de();
        ez_pop_hl();
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52); /* SBC HL, DE */
        ez_push_hl();
        break;
    case OP_MUL:
        ez_ensure_mul24();
        emit8(0xC1);
        emit8(0xD1);
        ez_stack_bytes -= 6;
        emit8_24(0xCD, (u32)ez_mul24_addr);
        ez_push_hl();
        break;
    case OP_UDIV:
    case OP_UMOD:
        ez_ensure_udivmod24();
        emit8(0xC1); /* BC = divisor */
        emit8(0xD1); /* DE = dividend */
        ez_stack_bytes -= 6;
        emit8_24(0xCD, (u32)ez_udivmod24_addr);
        if (op == OP_UDIV)
            emit8(0xEB);
        ez_push_hl();
        break;
    case OP_DIV:
    case OP_MOD:
        ez_ensure_sdivmod24();
        ez_pop_de();
        ez_pop_hl();
        emit8_24(0xCD, (u32)ez_sdivmod24_addr);
        if (op == OP_DIV)
            emit8(0xEB);
        ez_push_hl();
        break;
    case OP_AND:
    case OP_OR:
    case OP_XOR: {
        u8 b_op = (op == OP_AND) ? 0xA0 : (op == OP_OR) ? 0xB0 : 0xA8; /* AND/OR/XOR B */
        ez_ensure_rt_data();
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off + 3);
        {
            u8 i;
            for (i = 0; i < 3; i++) {
                emit8(0x3A);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + i);
                emit24(0);
                emit8(0x47); /* LD B, A */
                emit8(0x3A);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3 + i);
                emit24(0);
                emit8(b_op);
                emit8(0x32);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3 + i);
                emit24(0);
            }
        }
        ez_ld_hl_ind_data(ez_tmp_off + 3);
        ez_push_hl();
        break;
    }
    case OP_INVERT:
        ez_pop_hl();
        emit8(0xEB);
        emit8_24(0x21, 0xFFFFFFUL);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52);
        ez_push_hl();
        break;
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        ez_ensure_shifts();
        ez_pop_de();
        ez_pop_hl();
        emit8(0x7B); /* LD A, E */
        emit8_8(0xE6, 31);
        emit8_24(0xCD, (u32)(op == OP_LSHIFT ? ez_shl24_addr
                                              : (op == OP_RSHIFT ? ez_shr24_addr : ez_ashr24_addr)));
        ez_push_hl();
        break;
    case OP_EQ:
    case OP_NE:
    case OP_AEQ:
    case OP_ANE:
    case OP_ULT:
    case OP_UGT:
    case OP_ULE:
    case OP_UGE:
    case OP_ALT:
    case OP_AGT:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
        u16 skip_t;
        u8 jcc;
        if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
            ez_pop_hl();
            ez_pop_de();
        } else {
            ez_pop_de();
            ez_pop_hl();
        }
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52); /* SBC HL, DE */
        if (op == OP_EQ || op == OP_AEQ)
            jcc = 0xCA;
        else if (op == OP_NE || op == OP_ANE)
            jcc = 0xC2;
        else if (op == OP_ULT || op == OP_UGT || op == OP_ALT || op == OP_AGT)
            jcc = 0xDA;
        else if (op == OP_ULE || op == OP_UGE)
            jcc = 0xD2;
        else {
            /* Signed: (S ^ P/V) == 1 for LT/GT, 0 for GE/LE */
            u16 ov_pc, done_pc, t_pc;
            u8 j_noov = (op == OP_LT || op == OP_GT) ? 0xFA : 0xF2;
            u8 j_ov = (op == OP_LT || op == OP_GT) ? 0xF2 : 0xFA;
            emit8(0xEA); /* JP PE, ov */
            ov_pc = (u16)cb_pc();
            emit24(0);
            emit8_24(0x21, 0xFFFFFFUL);
            emit8(j_noov);
            t_pc = (u16)cb_pc();
            emit24(0);
            emit8_24(0x21, 0);
            emit8(0xC3);
            done_pc = (u16)cb_pc();
            emit24(0);
            patch24(ov_pc, (u32)cb_cur_addr());
            emit8_24(0x21, 0xFFFFFFUL);
            emit8(j_ov);
            skip_t = (u16)cb_pc();
            emit24(0);
            emit8_24(0x21, 0);
            patch24(t_pc, (u32)cb_cur_addr());
            patch24(done_pc, (u32)cb_cur_addr());
            patch24(skip_t, (u32)cb_cur_addr());
            ez_push_hl();
            break;
        }
        emit8_24(0x21, 0xFFFFFFUL);
        emit8(jcc);
        skip_t = (u16)cb_pc();
        emit24(0);
        emit8_24(0x21, 0);
        patch24(skip_t, (u32)cb_cur_addr());
        ez_push_hl();
        break;
    }
    case OP_U8FETCH:
        ez_pop_hl();
        emit8(0x7E); /* LD A, (HL) */
        emit8_24(0x21, 0);
        emit8(0x6F); /* LD L, A */
        ez_push_hl();
        break;
    case OP_I8FETCH: {
        u16 pos;
        ez_pop_hl();
        emit8(0x7E);
        emit8(0xB7);
        emit8_24(0x21, 0);
        emit8(0xF2);
        pos = (u16)cb_pc();
        emit24(0);
        emit8_24(0x21, 0xFFFFFFUL);
        patch24(pos, (u32)cb_cur_addr());
        emit8(0x6F);
        ez_push_hl();
        break;
    }
    case OP_I8STORE:
        ez_pop_hl();
        ez_pop_de();
        emit8(0x73); /* LD (HL), E */
        break;
    case OP_SI8STORE:
        ez_pop_de();
        ez_pop_hl();
        emit8(0x73);
        break;
    case OP_FETCH:
    case OP_AFETCH:
        ez_pop_hl();
        emit8_8(0xED, 0x27); /* LD HL, (HL) */
        ez_push_hl();
        break;
    case OP_STORE:
    case OP_ASTORE:
        ez_pop_hl();
        ez_pop_de();
        emit8_8(0xED, 0x1F); /* LD (HL), DE */
        break;
    case OP_SSTORE:
    case OP_SASTORE:
        ez_pop_de();
        ez_pop_hl();
        emit8_8(0xED, 0x1F);
        break;
    case OP_U16FETCH:
        ez_pop_hl();
        emit8(0x5E); /* LD E, (HL) */
        emit8(0x23);
        emit8(0x56); /* LD D, (HL) */
        emit8_24(0x21, 0);
        emit8(0x6B); /* LD L, E */
        emit8(0x62); /* LD H, D */
        ez_push_hl();
        break;
    case OP_I16FETCH: {
        u16 pos;
        ez_pop_hl();
        emit8(0x5E); /* LD E, (HL) */
        emit8(0x23);
        emit8(0x56); /* LD D, (HL) */
        emit8(0x7A); /* LD A, D */
        emit8(0xB7); /* OR A */
        emit8_24(0x21, 0);
        emit8(0xF2); /* JP P, pos */
        pos = (u16)cb_pc();
        emit24(0);
        emit8_24(0x21, 0xFFFFFFUL);
        patch24(pos, (u32)cb_cur_addr());
        emit8(0x6B); /* LD L, E */
        emit8(0x62); /* LD H, D */
        ez_push_hl();
        break;
    }
    case OP_I16STORE:
        ez_pop_hl();
        ez_pop_de();
        emit8(0x73);
        emit8(0x23);
        emit8(0x72);
        break;
    case OP_SEXT: {
        u16 pos;
        ez_pop_hl();
        emit8(0x7D); /* LD A, L */
        emit8(0xB7); /* OR A */
        emit8_24(0x21, 0);
        emit8(0xF2); /* JP P, pos */
        pos = (u16)cb_pc();
        emit24(0);
        emit8_24(0x21, 0xFFFFFFUL);
        patch24(pos, (u32)cb_cur_addr());
        emit8(0x6F); /* LD L, A */
        ez_push_hl();
        break;
    }
    case OP_SI16STORE:
        ez_pop_de();
        ez_pop_hl();
        emit8(0x73);
        emit8(0x23);
        emit8(0x72);
        break;
    case OP_BCOPY: {
        u16 skip;
        emit8(0xC1); /* BC = len */
        emit8(0xE1); /* HL = src */
        emit8(0xD1); /* DE = dst */
        ez_stack_bytes -= 9;
        emit8(0xE5);
        emit8_24(0x21, 0);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x42); /* SBC HL, BC */
        emit8(0xE1);
        emit8(0xCA);
        skip = (u16)cb_pc();
        emit24(0);
        emit8_8(0xED, 0xB0); /* LDIR */
        patch24(skip, (u32)cb_cur_addr());
        break;
    }
    case OP_W2A:
    case OP_A2W:
        break;
    case OP_W2I32:
        ez_pop_de();
        emit8(0xD5); /* PUSH DE */
        emit8(0xE1); /* POP HL */
        emit8(0x29); /* ADD HL, HL -> C = sign */
        emit8(0x9F); /* SBC A, A -> 0xFF or 0x00 */
        emit8_24(0x21, 0);
        emit8(0x6F); /* LD L, A */
        ez_push_hl();
        emit8(0xD5); /* PUSH DE */
        ez_stack_bytes += 3;
        break;
    case OP_U2I32:
        ez_pop_de();
        emit8_24(0x21, 0);
        ez_push_hl();
        emit8(0xD5);
        ez_stack_bytes += 3;
        break;
    case OP_I322W:
        ez_pop_hl();
        ez_pop_de();
        ez_push_hl();
        break;
    case OP_I32FETCH:
        ez_pop_hl();
        emit8_8(0xED, 0x17); /* LD DE, (HL) */
        emit8(0x23);
        emit8(0x23);
        emit8(0x23);
        emit8(0x7E); /* LD A, (HL) */
        emit8_24(0x21, 0);
        emit8(0x6F);
        ez_push_hl();
        emit8(0xD5);
        ez_stack_bytes += 3;
        break;
    case OP_I32STORE:
        ez_pop_hl(); /* addr */
        ez_pop_de(); /* lo 24 */
        emit8(0xC1); /* hi 8 in C */
        ez_stack_bytes -= 3;
        emit8_8(0xED, 0x1F); /* LD (HL), DE */
        emit8(0x23);
        emit8(0x23);
        emit8(0x23);
        emit8(0x71); /* LD (HL), C */
        break;
    case OP_ADD32:
        ez_ensure_rt_data();
        ez_pop_de(); /* b_lo */
        ez_pop_hl(); /* b_hi */
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl(); /* a_lo */
        emit8(0xC1); /* a_hi in C */
        ez_stack_bytes -= 3;
        emit8(0x19); /* ADD HL, DE */
        emit8(0xEB); /* DE = sum_lo, C preserved */
        ez_ld_hl_data(ez_tmp_off);
        emit8(0x79); /* LD A, C */
        emit8(0x8E); /* ADC A, (HL) */
        emit8_24(0x21, 0);
        emit8(0x6F);
        ez_push_hl();
        emit8(0xD5);
        ez_stack_bytes += 3;
        break;
    case OP_SUB32:
        ez_ensure_rt_data();
        ez_pop_de(); /* b_lo */
        ez_pop_hl(); /* b_hi */
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl(); /* a_lo */
        emit8(0xC1); /* a_hi in C */
        ez_stack_bytes -= 3;
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52); /* SBC HL, DE */
        emit8(0xEB); /* DE = diff_lo, C preserved */
        ez_ld_hl_data(ez_tmp_off);
        emit8(0x79); /* LD A, C */
        emit8(0x9E); /* SBC A, (HL) */
        emit8_24(0x21, 0);
        emit8(0x6F);
        ez_push_hl();
        emit8(0xD5);
        ez_stack_bytes += 3;
        break;
    case OP_INV32:
        ez_pop_de(); /* lo */
        ez_pop_hl(); /* hi */
        emit8(0x7D); /* LD A, L */
        emit8(0x2F); /* CPL */
        emit8_24(0x21, 0);
        emit8(0x6F);
        ez_push_hl();
        emit8_24(0x21, 0xFFFFFFUL);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52); /* SBC HL, DE */
        ez_push_hl();
        break;
    case OP_AND32:
    case OP_OR32:
    case OP_XOR32: {
        u8 alu_op = (op == OP_AND32) ? 0xA6 : ((op == OP_OR32) ? 0xB6 : 0xAE);
        u8 back;
        ez_ensure_rt_data();
        /* Pop b (lo 3 bytes, hi 1 byte) to ez_tmp_off..ez_tmp_off+3 */
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl();
        emit8(0x7D); /* LD A, L */
        emit8(0x32);
        cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3);
        emit24(0);
        /* Pop a_lo to ez_tmp_off+4? Use SP directly: a_lo at [SP+0..2], a_hi at [SP+3] */
        ez_ld_hl_data(ez_tmp_off); /* HL = &b[0] */
        emit8_24(0xED, 0x07); /* nope, use DE = SP: LD HL, 0; ADD HL, SP; EX DE, HL */
        cb_rewind(4);
        emit8_24(0x21, 0);
        emit8(0x39); /* ADD HL, SP -> HL = &a[0] */
        emit8(0xEB); /* DE = &a[0] */
        ez_ld_hl_data(ez_tmp_off); /* HL = &b[0] */
        emit8_8(0x06, 4); /* LD B, 4 */
        back = (u8)cb_cur_addr(); /* wait, use relative */
        (void)back;
        {
            size_t l = cb_cur_addr();
            emit8(0x1A); /* LD A, (DE) */
            emit8(alu_op); /* AND/OR/XOR (HL) */
            emit8(0x12); /* LD (DE), A */
            emit8(0x13); /* INC DE */
            emit8(0x23); /* INC HL */
            emit8(0x10); /* DJNZ l */
            emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
        }
        break;
    }
    case OP_EQ32:
    case OP_NE32:
    case OP_ULT32:
    case OP_UGT32:
    case OP_ULE32:
    case OP_UGE32:
    case OP_LT32:
    case OP_GT32:
    case OP_LE32:
    case OP_GE32: {
        u16 cmp_lo_pc, ret_t_pc, ret_f_pc, done_pc;
        int swap = (op == OP_GT32 || op == OP_LE32 || op == OP_UGT32 || op == OP_ULE32);
        ez_ensure_rt_data();
        if (swap) {
            /* Pop rhs into HL/E, lhs into tmp -> actually pop b then a, swap via tmp */
            ez_pop_hl(); /* b_lo */
            ez_ld_data_hl(ez_tmp_off);
            ez_pop_hl(); /* b_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            ez_pop_hl(); /* a_lo -> DE */
            emit8(0xEB);
            ez_pop_hl(); /* a_hi -> C */
            emit8(0x4D); /* LD C, L */
            ez_ld_hl_data(ez_tmp_off); /* wait: we need lhs=b, rhs=a */
            /* Swap: b_lo -> HL, b_hi -> E, a_lo -> tmp, a_hi -> tmp+3 */
            emit8(0xEB); /* HL = a_lo */
            emit8(0xC5); /* PUSH BC (a_hi) */
            emit8(0xE5); /* PUSH HL (a_lo) */
            ez_ld_hl_ind_data(ez_tmp_off); /* HL = b_lo */
            emit8(0xEB); /* DE = b_lo */
            ez_ld_hl_ind_data(ez_tmp_off + 3); /* L = b_hi */
            emit8(0x45); /* B = b_hi? C = b_hi? LD B, L */
            emit8(0x4D); /* LD C, L */
            emit8(0xE1); /* HL = a_lo */
            ez_ld_data_hl(ez_tmp_off);
            emit8(0xE1); /* HL = a_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            emit8(0xEB); /* HL = b_lo */
            emit8(0x59); /* LD E, C (b_hi) */
        } else {
            ez_pop_hl(); /* b_lo */
            ez_ld_data_hl(ez_tmp_off);
            ez_pop_hl(); /* b_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            ez_pop_hl(); /* a_lo */
            ez_pop_de(); /* a_hi in E */
        }
        /* Now HL = lhs_lo, E = lhs_hi, tmp = rhs_lo, tmp+3 = rhs_hi */
        emit8(0xE5); /* PUSH HL (save lhs_lo) */
        ez_ld_hl_data(ez_tmp_off + 3);
        emit8(0x7B); /* LD A, E (lhs_hi) */
        emit8(0xBE); /* CP (HL) (lhs_hi - rhs_hi) */
        emit8(0xE1); /* POP HL (lhs_lo, flags preserved) */
        emit8(0xCA); /* JP Z, cmp_lo */
        cmp_lo_pc = (u16)cb_pc();
        emit24(0);
        /* High bytes differ */
        if (op == OP_EQ32) {
            emit8(0xC3);
            ret_f_pc = (u16)cb_pc();
            emit24(0);
        } else if (op == OP_NE32) {
            emit8(0xC3);
            ret_t_pc = (u16)cb_pc();
            emit24(0);
        } else if (op == OP_ULT32 || op == OP_UGT32 || op == OP_ULE32 || op == OP_UGE32) {
            u8 jcc = (op == OP_ULT32 || op == OP_UGT32) ? 0xDA : 0xD2; /* JC or JNC */
            emit8(jcc);
            ret_t_pc = (u16)cb_pc();
            emit24(0);
            emit8(0xC3);
            ret_f_pc = (u16)cb_pc();
            emit24(0);
        } else {
            /* Signed 8-bit compare on high bytes: (S ^ P/V) == 1 for LT/GT */
            u16 ov_pc;
            u8 j_noov = (op == OP_LT32 || op == OP_GT32) ? 0xFA : 0xF2;
            u8 j_ov = (op == OP_LT32 || op == OP_GT32) ? 0xF2 : 0xFA;
            emit8(0xEA); /* JP PE, ov */
            ov_pc = (u16)cb_pc();
            emit24(0);
            emit8(j_noov);
            ret_t_pc = (u16)cb_pc();
            emit24(0);
            emit8(0xC3);
            ret_f_pc = (u16)cb_pc();
            emit24(0);
            patch24(ov_pc, (u32)cb_cur_addr());
            emit8(j_ov);
            /* patch both ret_t_pc? No, jump to ret_t/ret_f below */
            {
                u16 t2 = (u16)cb_pc();
                emit24(0);
                emit8(0xC3);
                {
                    u16 f2 = (u16)cb_pc();
                    emit24(0);
                    patch24(cmp_lo_pc, (u32)cb_cur_addr());
                    /* Compare unsigned 24-bit HL vs (tmp) */
                    emit8(0xEB); /* DE = lhs_lo */
                    ez_ld_hl_ind_data(ez_tmp_off); /* HL = rhs_lo */
                    emit8(0xEB); /* HL = lhs_lo, DE = rhs_lo */
                    emit8(0xB7);
                    emit8(0xED);
                    emit8(0x52); /* SBC HL, DE */
                    if (op == OP_EQ32) {
                        emit8(0xCA);
                        ret_t_pc = (u16)cb_pc();
                        emit24(0);
                        emit8(0xC3);
                        ret_f_pc = (u16)cb_pc();
                        emit24(0);
                    } else if (op == OP_NE32) {
                        emit8(0xC2);
                        ret_t_pc = (u16)cb_pc();
                        emit24(0);
                        emit8(0xC3);
                        ret_f_pc = (u16)cb_pc();
                        emit24(0);
                    } else {
                        u8 jcc = (op == OP_LT32 || op == OP_GT32) ? 0xDA : 0xD2;
                        emit8(jcc);
                        {
                            u16 t3 = (u16)cb_pc();
                            emit24(0);
                            patch24(ret_f_pc, (u32)cb_cur_addr());
                            patch24(f2, (u32)cb_cur_addr());
                            emit8_24(0x21, 0);
                            emit8(0xC3);
                            done_pc = (u16)cb_pc();
                            emit24(0);
                            patch24(ret_t_pc, (u32)cb_cur_addr());
                            patch24(t2, (u32)cb_cur_addr());
                            patch24(t3, (u32)cb_cur_addr());
                            emit8_24(0x21, 0xFFFFFFUL);
                            patch24(done_pc, (u32)cb_cur_addr());
                        }
                    }
                }
            }
            if (op == OP_EQ32 || op == OP_NE32) {
                /* unreachable for signed */
            }
            ez_push_hl();
            break;
        }
        patch24(cmp_lo_pc, (u32)cb_cur_addr());
        emit8(0xEB);
        ez_ld_hl_ind_data(ez_tmp_off);
        emit8(0xEB);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52);
        {
            u8 jcc = (op == OP_EQ32)
                         ? 0xCA
                         : ((op == OP_NE32)
                                ? 0xC2
                                : ((op == OP_ULT32 || op == OP_UGT32) ? 0xDA : 0xD2));
            u16 t3;
            emit8(jcc);
            t3 = (u16)cb_pc();
            emit24(0);
            if (op != OP_NE32)
                patch24(ret_f_pc, (u32)cb_cur_addr());
            emit8_24(0x21, 0);
            emit8(0xC3);
            done_pc = (u16)cb_pc();
            emit24(0);
            if (op != OP_EQ32)
                patch24(ret_t_pc, (u32)cb_cur_addr());
            patch24(t3, (u32)cb_cur_addr());
            emit8_24(0x21, 0xFFFFFFUL);
            patch24(done_pc, (u32)cb_cur_addr());
        }
        ez_push_hl();
        break;
    }
    default:
        ir4_die("unimplemented eZ80 op");
    }
}

static int ez_add_imm(i32 imm) {
    (void)imm;
    return 0;
}

static int ez_cmp_imm(IrOp op, i32 imm) {
    (void)op;
    (void)imm;
    return 0;
}

static int ez_shift_imm(IrOp op, u8 count) {
    (void)op;
    (void)count;
    return 0;
}

static int ez_mem_imm(IrOp op, i32 off) {
    (void)op;
    (void)off;
    return 0;
}

static int ez_data_mem(IrOp op, size_t data_offset) {
    (void)op;
    (void)data_offset;
    return 0;
}

static int ez_local_mem(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)slot;
    (void)off;
    (void)op;
    (void)stack_bytes;
    return 0;
}

static size_t ez_emit_br_fwd(BranchCond cond) {
    size_t pos;
    if (cond == BR_JZ) {
        ez_pop_hl();
        emit8(0xEB);
        emit8_24(0x21, 0);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52); /* SBC HL, DE */
        emit8(0xCA); /* JP Z, target */
        pos = cb_pc();
        emit24(0);
        return pos;
    }
    emit8(0xC3); /* JP target */
    pos = cb_pc();
    emit24(0);
    return pos;
}

static void ez_emit_br_back(BranchCond cond, size_t target_addr) {
    if (cond == BR_JZ) {
        ez_pop_hl();
        emit8(0xEB);
        emit8_24(0x21, 0);
        emit8(0xB7);
        emit8(0xED);
        emit8(0x52);
        emit8_24(0xCA, (u32)target_addr);
    } else {
        emit8_24(0xC3, (u32)target_addr);
    }
}

static void ez_patch_fwd(size_t patch_offset) { patch24(patch_offset, (u32)cb_cur_addr()); }

static void ez_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    patch24(call_pc + 1, (u32)target_addr);
}

static size_t ez_push_fn(size_t target_addr) {
    size_t pos;
    emit8(0x21);
    pos = cb_pc();
    emit24((u32)target_addr);
    ez_push_hl();
    return pos;
}

static void ez_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    patch24(patch_pc, (u32)target_addr);
}

static void ez_icall(void) {
    u16 ret_pc;
    ez_pop_hl();
    emit8_24(0x11, 0); /* LD DE, ret_addr */
    ret_pc = (u16)(cb_pc() - 3);
    emit8(0xD5); /* PUSH DE */
    emit8(0xE9); /* JP (HL) */
    patch24(ret_pc, (u32)cb_cur_addr());
}

static size_t ez_stack_data_off(void) { return 0; }

const Backend be_ez80 = {"ez80",
                         3,
                         3,
                         4,
                         ez_cur_addr,
                         ez_fn_begin,
                         ez_fn_end,
                         ez_call,
                         ez_call_extern,
                         ez_push_w,
                         ez_push_a,
                         ez_push_f,
                         ez_push_i32,
                         ez_push_data,
                         ez_local_op,
                         ez_stack_op,
                         ez_op,
                         ez_add_imm,
                         ez_cmp_imm,
                         ez_shift_imm,
                         ez_mem_imm,
                         ez_data_mem,
                         ez_local_mem,
                         ez_emit_br_fwd,
                         ez_emit_br_back,
                         ez_patch_fwd,
                         ez_patch_call,
                         ez_push_fn,
                         ez_patch_fn,
                         ez_icall,
                         ez_stack_data_off};

/* be_arm64.c - Simplified ARM64 (AArch64) Binary Backend for IR4th (Strict C89)
 * Uniform 64-bit cells (W: and A: are both 8 bytes; no 8-bit stack items).
 * Stack & Frame Convention:
 *   - Evaluation stack: Hardware SP using 16-byte aligned slots (1 cell per slot)
 *   - Frame pointer: X29 (locals at [X29, #-8], [X29, #-16], ...)
 */
#include "codebuf.h"

/* --- Native Hardware-SP Push/Pop (16-byte aligned slots) --- */
static void push_x0(void) { emit32(0xF81F0FE0UL); } /* STR X0, [SP, #-16]! */
static void push_x1(void) { emit32(0xF81F0FE1UL); } /* STR X1, [SP, #-16]! */
static void pop_x0(void)  { emit32(0xF84107E0UL); } /* LDR X0, [SP], #16   */
static void pop_x1(void)  { emit32(0xF84107E1UL); } /* LDR X1, [SP], #16   */
static void push_d0(void) { emit32(0xFC1F0FE0UL); } /* STR D0, [SP, #-16]! */
static void pop_d0(void)  { emit32(0xFC4107E0UL); } /* LDR D0, [SP], #16   */
static void pop_d1(void)  { emit32(0xFC4107E1UL); } /* LDR D1, [SP], #16   */

static void mov_x0_u32(u32 lo, u32 hi) {
    emit32(0xD2800000UL | ((lo & 0xFFFFUL) << 5));
    if ((lo >> 16) != 0 || hi != 0) {
        emit32(0xF2A00000UL | (((lo >> 16) & 0xFFFFUL) << 5));
    }
    if (hi != 0) {
        emit32(0xF2C00000UL | ((hi & 0xFFFFUL) << 5));
        emit32(0xF2E00000UL | (((hi >> 16) & 0xFFFFUL) << 5));
    }
}

static void mov_x0_imm(i32 val) {
    mov_x0_u32((u32)val, (val < 0) ? 0xFFFFFFFFUL : 0x00000000UL);
}

/* --- Backend Interface --- */
static u16 a64_cur_addr(void) { return cb_cur_addr(); }

static void a64_fn_begin(const char *name, u8 param_cnt,
                         const LocalVar *locals, u8 total_locals) {
    u8 i;
    u32 frame_bytes = (u32)((total_locals + 1) & ~1) * 8;
    (void)name;
    (void)locals;

    /* Note: Incoming parameters were pushed onto caller SP before BL.
     * Save FP/LR, set FP = SP, and copy incoming args from [X29, #16 + k*16] */
    emit32(0xA9BF7BFDUL); /* STP X29, X30, [SP, #-16]! */
    emit32(0x910003FDUL); /* MOV X29, SP */

    if (frame_bytes > 0) {
        emit32(0xD10003FFUL | ((frame_bytes & 0xFFFUL) << 10)); /* SUB SP, SP, #frame */
    }

    for (i = 0; i < param_cnt; i++) {
        u32 caller_off = (u32)(16 + (param_cnt - 1 - i) * 16);
        u32 local_off  = (u32)(i + 1) * 8;
        emit32(0xF94003A0UL | ((caller_off >> 3) << 10));         /* LDR X0, [X29, #caller_off] */
        emit32(0xF80003A0UL | (((0UL - local_off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-local_off] */
    }

    if (total_locals > param_cnt) {
        emit32(0xD2800000UL); /* MOV X0, #0 */
        for (i = param_cnt; i < total_locals; i++) {
            u32 local_off = (u32)(i + 1) * 8;
            emit32(0xF80003A0UL | (((0UL - local_off) & 0x1FFUL) << 12));
        }
    }
}

static void a64_fn_end(const FuncSig *sig, const LocalVar *locals, u8 total_locals) {
    u8 k;
    u8 param_cnt = sig->in_count;
    u8 out_count = sig->out_count;
    (void)locals;
    (void)total_locals;

    /* Save return values from evaluation SP into scratch X9..X12 */
    for (k = out_count; k > 0; k--) {
        emit32(0xF84107E0UL | (u32)(8 + k)); /* LDR X(8+k), [SP], #16 */
    }

    emit32(0x910003BFUL); /* MOV SP, X29 */
    emit32(0xA8C17BFDUL); /* LDP X29, X30, [SP], #16 */

    /* Pop incoming parameters pushed by caller */
    if (param_cnt > 0) {
        u32 arg_bytes = (u32)param_cnt * 16;
        emit32(0x910003FFUL | ((arg_bytes & 0xFFFUL) << 10)); /* ADD SP, SP, #arg_bytes */
    }

    /* Push return values onto caller's evaluation stack */
    for (k = 1; k <= out_count; k++) {
        emit32(0xF81F0FE0UL | (u32)(8 + k)); /* STR X(8+k), [SP, #-16]! */
    }

    emit32(0xD65F03C0UL); /* RET */
}

static void a64_call(u16 target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    emit32(0x94000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
}

static void a64_call_extern(const FuncSig *sig) {
    u8 int_cnt = 0;
    u8 flt_cnt = 0;
    u8 i;

    if (sig->has_addr) {
        ir4_die("ARM64 extern does not take a fixed address");
    }

    for (i = 0; i < sig->in_count; i++) {
        if (sig->in_types[i] == TY_F) flt_cnt++;
        else                          int_cnt++;
    }
    if (int_cnt > 8 || flt_cnt > 8) {
        ir4_die("ARM64 extern supports at most 8 int and 8 float args");
    }

    for (i = sig->in_count; i > 0; i--) {
        if (sig->in_types[i - 1] == TY_F) {
            flt_cnt--;
            emit32(0xFC4107E0UL | (u32)flt_cnt); /* LDR D<k>, [SP], #16 */
        } else {
            int_cnt--;
            emit32(0xF84107E0UL | (u32)int_cnt); /* LDR X<k>, [SP], #16 */
        }
    }

    cb_add_extern_reloc(cb_pc(), sig->name);
    emit32(0xD2800009UL); /* MOVZ X9, #0 */
    emit32(0xF2A00009UL); /* MOVK X9, #0, LSL #16 */
    emit32(0xF2C00009UL); /* MOVK X9, #0, LSL #32 */
    emit32(0xF2E00009UL); /* MOVK X9, #0, LSL #48 */
    emit32(0xD63F0120UL); /* BLR X9 */

    if (sig->out_count == 1) {
        if (sig->out_types[0] == TY_F) push_d0();
        else                           push_x0();
    }
}

static void a64_push_w(i32 val) { mov_x0_imm(val); push_x0(); }
static void a64_push_a(u32 val) { mov_x0_u32(val, 0); push_x0(); }
static void a64_push_f(double val) {
    u8 bytes[8];
    u32 lo, hi;
    memcpy(bytes, &val, 8);
    lo = (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
    hi = (u32)bytes[4] | ((u32)bytes[5] << 8) | ((u32)bytes[6] << 16) | ((u32)bytes[7] << 24);
    mov_x0_u32(lo, hi);
    push_x0();
}

static void a64_push_data(u16 offset) {
    if (offset <= 4095) {
        emit32(0x91000380UL | (((u32)offset & 0xFFFUL) << 10)); /* ADD X0, X28, #offset */
    } else {
        mov_x0_u32((u32)offset, 0);
        emit32(0x8B000380UL); /* ADD X0, X28, X0 */
    }
    push_x0();
}

static void a64_local_op(LocalOp op, u8 slot, StackType type, u16 stack_bytes) {
    u32 off = (u32)(slot + 1) * 8;
    (void)type;
    (void)stack_bytes;
    if (op == LOCAL_GET) {
        emit32(0xF84003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X0, [X29, #-off] */
        push_x0();
    } else {
        pop_x0();
        emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
    }
}

static void a64_stack_op(StackOp op, StackType t1, StackType t2) {
    (void)t1; (void)t2;
    switch (op) {
    case OP_DUP:
        emit32(0xF94003E0UL); /* LDR X0, [SP] */
        push_x0();
        break;
    case OP_DROP:
        emit32(0x910043FFUL); /* ADD SP, SP, #16 */
        break;
    case OP_SWAP:
        pop_x0(); pop_x1(); push_x0(); push_x1();
        break;
    case OP_OVER:
        emit32(0xF9400BE0UL); /* LDR X0, [SP, #16] */
        push_x0();
        break;
    default:
        ir4_die("unimplemented ARM64 stack op");
    }
}

static void a64_int_alu(IrOp op) {
    pop_x1(); /* rhs */
    pop_x0(); /* lhs */

    switch (op) {
    case OP_ADD:
    case OP_AADD:
        emit32(0x8B010000UL); /* ADD X0, X0, X1 */
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        emit32(0xCB010000UL); /* SUB X0, X0, X1 */
        break;
    case OP_MUL:
        emit32(0x9B017C00UL); /* MUL X0, X0, X1 */
        break;
    case OP_DIV:
        emit32(0x9AC10C00UL); /* SDIV X0, X0, X1 */
        break;
    case OP_UDIV:
        emit32(0x9AC10800UL); /* UDIV X0, X0, X1 */
        break;
    case OP_MOD:
        emit32(0x9AC10C02UL);
        emit32(0x9B018040UL);
        break;
    case OP_UMOD:
        emit32(0x9AC10802UL);
        emit32(0x9B018040UL);
        break;
    case OP_AND:
        emit32(0x8A010000UL); /* AND X0, X0, X1 */
        break;
    case OP_OR:
        emit32(0xAA010000UL); /* ORR X0, X0, X1 */
        break;
    case OP_XOR:
        emit32(0xCA010000UL); /* EOR X0, X0, X1 */
        break;
    case OP_LSHIFT:
        emit32(0x9AC12000UL); /* LSLV X0, X0, X1 */
        break;
    case OP_RSHIFT:
        emit32(0x9AC12400UL); /* LSRV X0, X0, X1 */
        break;
    case OP_ARSHIFT:
        emit32(0x9AC12800UL); /* ASRV X0, X0, X1 */
        break;
    default:
        ir4_die("unimplemented ARM64 ALU op");
    }

    push_x0();
}

static void a64_int_cmp(IrOp op) {
    u32 cond = 0;
    pop_x1();
    pop_x0();
    emit32(0xEB01001FUL); /* CMP X0, X1 */

    switch (op) {
    case OP_EQ:
    case OP_AEQ:  cond = 0x0; break;
    case OP_NE:
    case OP_ANE:  cond = 0x1; break;
    case OP_LT:   cond = 0xB; break;
    case OP_GE:   cond = 0xA; break;
    case OP_GT:   cond = 0xC; break;
    case OP_LE:   cond = 0xD; break;
    case OP_ULT:
    case OP_ALT:  cond = 0x3; break;
    case OP_UGE:  cond = 0x2; break;
    case OP_UGT:
    case OP_AGT:  cond = 0x8; break;
    case OP_ULE:  cond = 0x9; break;
    default:
        ir4_die("unimplemented ARM64 comparison op");
    }

    emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    push_x0();
}

static void a64_op(IrOp op) {
    switch (op) {
    case OP_ADD: case OP_AADD:
    case OP_SUB: case OP_ASUB: case OP_ADIFF:
    case OP_MUL: case OP_DIV: case OP_UDIV: case OP_MOD: case OP_UMOD:
    case OP_AND: case OP_OR: case OP_XOR:
    case OP_LSHIFT: case OP_RSHIFT: case OP_ARSHIFT:
        a64_int_alu(op);
        break;
    case OP_INVERT:
        pop_x0();
        emit32(0xAA2003E0UL); /* MVN X0, X0 */
        push_x0();
        break;
    case OP_FNEG:
        pop_d0();
        emit32(0x1E614000UL); /* FNEG D0, D0 */
        push_d0();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
        pop_d1(); /* rhs */
        pop_d0(); /* lhs */
        if (op == OP_FADD)      emit32(0x1E612800UL); /* FADD D0, D0, D1 */
        else if (op == OP_FSUB) emit32(0x1E613800UL); /* FSUB D0, D0, D1 */
        else if (op == OP_FMUL) emit32(0x1E610800UL); /* FMUL D0, D0, D1 */
        else                    emit32(0x1E611800UL); /* FDIV D0, D0, D1 */
        push_d0();
        break;
    case OP_EQ: case OP_AEQ:
    case OP_NE: case OP_ANE:
    case OP_LT: case OP_GE: case OP_GT: case OP_LE:
    case OP_ULT: case OP_ALT: case OP_UGE: case OP_UGT: case OP_AGT: case OP_ULE:
        a64_int_cmp(op);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT: {
        u32 cond = (op == OP_FEQ) ? 0x0 : (op == OP_FNE) ? 0x1 : (op == OP_FLT) ? 0x4 : 0xC;
        pop_d1();
        pop_d0();
        emit32(0x1E612000UL); /* FCMP D0, D1 */
        emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
        push_x0();
        break;
    }
    case OP_U8FETCH:
        pop_x0();
        emit32(0x39400000UL); /* LDRB W0, [X0] (implicitly zero-extends to 64-bit X0) */
        push_x0();
        break;
    case OP_I8FETCH:
        pop_x0();
        emit32(0x39800000UL); /* LDRSB X0, [X0] */
        push_x0();
        break;
    case OP_I8STORE:
        pop_x0(); pop_x1();
        emit32(0x39000001UL); /* STRB W1, [X0] */
        break;
    case OP_U16FETCH:
        pop_x0();
        emit32(0x79400000UL); /* LDRH W0, [X0] */
        push_x0();
        break;
    case OP_I16FETCH:
        pop_x0();
        emit32(0x79800000UL); /* LDRSH X0, [X0] */
        push_x0();
        break;
    case OP_I16STORE:
        pop_x0(); pop_x1();
        emit32(0x79000001UL); /* STRH W1, [X0] */
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_FFETCH:
        pop_x0();
        emit32(0xF9400000UL); /* LDR X0, [X0] */
        push_x0();
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_FSTORE:
        pop_x0(); pop_x1();
        emit32(0xF9000001UL); /* STR X1, [X0] */
        break;
    case OP_SEXT:
        pop_x0();
        emit32(0x93401C00UL); /* SXTB X0, X0 */
        push_x0();
        break;
    case OP_W2A:
    case OP_A2W:
        /* Uniform 64-bit cells: zero-cost no-op */
        break;
    case OP_W2F:
        pop_x0();
        emit32(0x9E620000UL); /* SCVTF D0, X0 */
        push_d0();
        break;
    case OP_F2W:
        pop_d0();
        emit32(0x9E780000UL); /* FCVTZS X0, D0 */
        push_x0();
        break;
    }
}

static u16 a64_emit_br_fwd(BranchCond cond) {
    u16 pos;
    if (cond == BR_JZ) {
        pop_x0();
        pos = cb_pc();
        emit32(0xB4000000UL); /* CBZ X0, rel19 */
    } else {
        pos = cb_pc();
        emit32(0x14000000UL); /* B rel26 */
    }
    return pos;
}

static void a64_emit_br_back(BranchCond cond, u16 target_addr) {
    i32 diff;
    if (cond == BR_JZ) {
        u32 imm19;
        pop_x0();
        diff = (i32)target_addr - (i32)cb_cur_addr();
        imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        emit32(0xB4000000UL | (imm19 << 5)); /* CBZ X0, rel19 */
    } else {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        emit32(0x14000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
    }
}

static void a64_patch_fwd(u16 patch_offset) {
    i32 diff = (i32)cb_pc() - (i32)patch_offset;
    u8 top = cb_data()[patch_offset + 3];
    if (top == 0xB4) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        patch32(patch_offset, 0xB4000000UL | (imm19 << 5));
    } else {
        u32 imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
        patch32(patch_offset, 0x14000000UL | imm26);
    }
}

const Backend be_arm64 = {
    "arm64",
    8, 8, 8,
    a64_cur_addr,
    a64_fn_begin,
    a64_fn_end,
    a64_call,
    a64_call_extern,
    a64_push_w,
    a64_push_a,
    a64_push_f,
    a64_push_data,
    a64_local_op,
    a64_stack_op,
    a64_op,
    a64_emit_br_fwd,
    a64_emit_br_back,
    a64_patch_fwd
};

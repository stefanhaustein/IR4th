/* be_arm64.c - Simplified ARM64 (AArch64) Binary Backend for IR4th (Strict C89)
 * Uniform 64-bit cells (W: and A: are both 8 bytes; no 8-bit stack items).
 * Stack & Frame Convention:
 *   - Evaluation stack: Hardware SP using 16-byte aligned slots (1 cell per slot)
 *   - Frame pointer: X29 (locals at [X29, #-8], [X29, #-16], ...)
 */
#include "ir4c.h"

static void emit32(u32 insn) {
    emit16((u16)(insn & 0xFFFFU));
    emit16((u16)((insn >> 16) & 0xFFFFU));
}

static void patch32(u16 offset, u32 insn) {
    patch16(offset, (u16)(insn & 0xFFFFU));
    patch16((u16)(offset + 2), (u16)((insn >> 16) & 0xFFFFU));
}

/* --- Native Hardware-SP Push/Pop (16-byte aligned slots) --- */
static void push_x0(void) { emit32(0xF81F0FE0UL); } /* STR X0, [SP, #-16]! */
static void push_x1(void) { emit32(0xF81F0FE1UL); } /* STR X1, [SP, #-16]! */
static void push_x2(void) { emit32(0xF81F0FE2UL); } /* STR X2, [SP, #-16]! */
static void pop_x0(void)  { emit32(0xF84107E0UL); } /* LDR X0, [SP], #16   */
static void pop_x1(void)  { emit32(0xF84107E1UL); } /* LDR X1, [SP], #16   */
static void pop_x2(void)  { emit32(0xF84107E2UL); } /* LDR X2, [SP], #16   */

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

static void a64_fn_end(u8 param_cnt, u8 total_locals, u8 out_count) {
    u8 k;
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

static void a64_push_w(i32 val) { mov_x0_imm(val); push_x0(); }
static void a64_push_a(u32 val) { mov_x0_u32(val, 0); push_x0(); }

static void a64_push_data(u16 offset) {
    if (offset <= 4095) {
        emit32(0x91000380UL | (((u32)offset & 0xFFFUL) << 10)); /* ADD X0, X28, #offset */
    } else {
        mov_x0_u32((u32)offset, 0);
        emit32(0x8B000380UL); /* ADD X0, X28, X0 */
    }
    push_x0();
}

static void a64_local_get(u8 slot, u8 type) {
    u32 off = (u32)(slot + 1) * 8;
    (void)type;
    emit32(0xF84003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X0, [X29, #-off] */
    push_x0();
}

static void a64_local_set(u8 slot, u8 type) {
    u32 off = (u32)(slot + 1) * 8;
    (void)type;
    pop_x0();
    emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
}

static void a64_stack_op(u8 op) {
    if (op == OP_DUP) {
        emit32(0xF94003E0UL); /* LDR X0, [SP] */
        push_x0();
    } else if (op == OP_DROP) {
        emit32(0x910043FFUL); /* ADD SP, SP, #16 */
    } else if (op == OP_SWAP) {
        pop_x0(); pop_x1(); push_x0(); push_x1();
    } else if (op == OP_OVER) {
        emit32(0xF9400BE0UL); /* LDR X0, [SP, #16] */
        push_x0();
    } else if (op == OP_ROT) {
        pop_x2(); pop_x1(); pop_x0();
        push_x1(); push_x2(); push_x0();
    }
}

static void a64_alu_op(u8 op) {
    if (op == OP_INVERT) {
        pop_x0();
        emit32(0xAA2003E0UL); /* MVN X0, X0 */
        push_x0();
        return;
    }

    pop_x1(); /* rhs */
    pop_x0(); /* lhs */

    if (op == OP_ADD || op == OP_AADD)                 emit32(0x8B010000UL); /* ADD X0, X0, X1 */
    else if (op == OP_SUB || op == OP_ASUB || op == OP_ADIFF) emit32(0xCB010000UL); /* SUB X0, X0, X1 */
    else if (op == OP_MUL)                             emit32(0x9B017C00UL); /* MUL X0, X0, X1 */
    else if (op == OP_DIV)                             emit32(0x9AC10C00UL); /* SDIV X0, X0, X1 */
    else if (op == OP_UDIV)                            emit32(0x9AC10800UL); /* UDIV X0, X0, X1 */
    else if (op == OP_MOD)   { emit32(0x9AC10C02UL); emit32(0x9B018040UL); }
    else if (op == OP_UMOD)  { emit32(0x9AC10802UL); emit32(0x9B018040UL); }
    else if (op == OP_AND)                             emit32(0x8A010000UL); /* AND X0, X0, X1 */
    else if (op == OP_OR)                              emit32(0xAA010000UL); /* ORR X0, X0, X1 */
    else if (op == OP_XOR)                             emit32(0xCA010000UL); /* EOR X0, X0, X1 */
    else if (op == OP_LSHIFT)                          emit32(0x9AC12000UL); /* LSLV X0, X0, X1 */
    else if (op == OP_RSHIFT)                          emit32(0x9AC12400UL); /* LSRV X0, X0, X1 */
    else if (op == OP_ARSHIFT)                         emit32(0x9AC12800UL); /* ASRV X0, X0, X1 */
    else ir4_die("unimplemented ARM64 ALU op");

    push_x0();
}

static void a64_cmp_op(u8 op) {
    u32 cond = 0;
    pop_x1();
    pop_x0();
    emit32(0xEB01001FUL); /* CMP X0, X1 */

    if (op == OP_EQ || op == OP_AEQ)       cond = 0x0;
    else if (op == OP_NE || op == OP_ANE)  cond = 0x1;
    else if (op == OP_LT)                  cond = 0xB;
    else if (op == OP_GE)                  cond = 0xA;
    else if (op == OP_GT)                  cond = 0xC;
    else if (op == OP_LE)                  cond = 0xD;
    else if (op == OP_ULT || op == OP_ALT) cond = 0x3;
    else if (op == OP_UGE)                 cond = 0x2;
    else if (op == OP_UGT || op == OP_AGT) cond = 0x8;
    else if (op == OP_ULE)                 cond = 0x9;
    else ir4_die("unimplemented ARM64 comparison op");

    emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    push_x0();
}

static void a64_mem_op(u8 op) {
    if (op == OP_CFETCH) {
        pop_x0();
        emit32(0x39400000UL); /* LDRB W0, [X0] (implicitly zero-extends to 64-bit X0) */
        push_x0();
    } else if (op == OP_CSTORE) {
        pop_x0(); pop_x1();
        emit32(0x39000001UL); /* STRB W1, [X0] */
    } else if (op == OP_FETCH || op == OP_AFETCH) {
        pop_x0();
        emit32(0xF9400000UL); /* LDR X0, [X0] */
        push_x0();
    } else if (op == OP_STORE || op == OP_ASTORE) {
        pop_x0(); pop_x1();
        emit32(0xF9000001UL); /* STR X1, [X0] */
    } else {
        ir4_die("unimplemented ARM64 float memory op");
    }
}

static void a64_conv_op(u8 op) {
    if (op == OP_SEXT) {
        pop_x0();
        emit32(0x93401C00UL); /* SXTB X0, X0 */
        push_x0();
    } else if (op == OP_W2A || op == OP_A2W) {
        /* Uniform 64-bit cells: zero-cost no-op */
    } else {
        ir4_die("unimplemented ARM64 float conversion op");
    }
}

static u16 a64_emit_jz_fwd(void) {
    u16 pos;
    pop_x0();
    pos = cb_pc();
    emit32(0xB4000000UL); /* CBZ X0, rel19 */
    return pos;
}

static u16 a64_emit_jmp_fwd(void) {
    u16 pos = cb_pc();
    emit32(0x14000000UL); /* B rel26 */
    return pos;
}

static void a64_emit_jmp_back(u16 target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    emit32(0x14000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
}

static void a64_emit_jz_back(u16 target_addr) {
    i32 diff;
    u32 imm19;
    pop_x0();
    diff = (i32)target_addr - (i32)cb_cur_addr();
    imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
    emit32(0xB4000000UL | (imm19 << 5)); /* CBZ X0, rel19 */
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
    a64_push_w,
    a64_push_a,
    a64_push_data,
    a64_local_get,
    a64_local_set,
    a64_stack_op,
    a64_alu_op,
    a64_cmp_op,
    a64_mem_op,
    a64_conv_op,
    a64_emit_jz_fwd,
    a64_emit_jmp_fwd,
    a64_emit_jmp_back,
    a64_emit_jz_back,
    a64_patch_fwd
};

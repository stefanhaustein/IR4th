/* ir4.h - Core IR4 types, limits, and Backend/Platform interfaces (Strict C89)
 *
 * Shared contract between the IR4th frontend, code buffer, backends, and
 * platform/output drivers. Each backend implements the Backend vtable; each
 * output mode (--cpm, --sim65, --virt, --obj, --run) implements Platform.
 */
#ifndef IR4_H
#define IR4_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

typedef unsigned char  u8;
typedef signed char    i8;
typedef unsigned short u16;
typedef signed short   i16;
#if UINT_MAX == 0xFFFFFFFFUL
typedef unsigned int   u32;
typedef signed int     i32;
#else
typedef unsigned long  u32;
typedef signed long    i32;
#endif

#define MAX_TOK    64
#define MAX_LOCALS 64
#define MAX_CODE   65535
#define MAX_DATA   49152
#define MAX_RELOCS 16384

/* Stack types (v0.3) */
typedef enum {
    TY_NONE = 0,
    TY_W,
    TY_A,
    TY_F,
    TY_I32
} StackType;

/* Stack operations */
typedef enum {
    OP_DUP = 0,
    OP_DROP,
    OP_SWAP,
    OP_OVER
} StackOp;

/* Local variable operations */
typedef enum {
    LOCAL_GET = 0,
    LOCAL_SET,
    LOCAL_ADDR
} LocalOp;

/* Branch conditions */
typedef enum {
    BR_JMP = 0,
    BR_JZ
} BranchCond;

/* Unified IR operations (ALU, comparison, memory, conversion) */
typedef enum {
    OP_ADD = 0,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_UDIV,
    OP_UMOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_INVERT,
    OP_LSHIFT,
    OP_ARSHIFT,
    OP_RSHIFT,
    OP_AADD,
    OP_ASUB,
    OP_ADIFF,
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FNEG,
    OP_ADD32,
    OP_SUB32,
    OP_MUL32,
    OP_DIV32,
    OP_MOD32,
    OP_UDIV32,
    OP_UMOD32,
    OP_AND32,
    OP_OR32,
    OP_XOR32,
    OP_INV32,
    OP_SHL32,
    OP_ASHR32,
    OP_RSHR32,

    OP_EQ,
    OP_NE,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_ULT,
    OP_UGT,
    OP_ULE,
    OP_UGE,
    OP_AEQ,
    OP_ANE,
    OP_ALT,
    OP_AGT,
    OP_FEQ,
    OP_FNE,
    OP_FLT,
    OP_FGT,
    OP_EQ32,
    OP_NE32,
    OP_LT32,
    OP_GT32,
    OP_LE32,
    OP_GE32,
    OP_ULT32,
    OP_UGT32,
    OP_ULE32,
    OP_UGE32,

    OP_I8FETCH,
    OP_U8FETCH,
    OP_I8STORE,
    OP_I16FETCH,
    OP_U16FETCH,
    OP_I16STORE,
    OP_FETCH,
    OP_STORE,
    OP_AFETCH,
    OP_ASTORE,
    OP_FFETCH,
    OP_FSTORE,
    OP_I32FETCH,
    OP_I32STORE,

    OP_SI8STORE,
    OP_SI16STORE,
    OP_SSTORE,
    OP_SASTORE,
    OP_SFSTORE,
    OP_BCOPY,

    OP_SEXT,
    OP_W2A,
    OP_A2W,
    OP_W2F,
    OP_F2W,
    OP_W2I32,
    OP_U2I32,
    OP_I322W
} IrOp;

typedef struct {
    char name[MAX_TOK];
    u8   type;
    u8   slot;
    u16  size;
} LocalVar;

typedef struct {
    char name[MAX_TOK];
    u16  addr;
    u8   is_extern;
    u8   is_defined;
    u8   has_addr;
    u8   is_recursive;
    u8   in_types[8];
    char in_regs[8][MAX_TOK];
    u8   in_count;
    u8   out_types[4];
    char out_reg[MAX_TOK];
    u8   out_count;
} FuncSig;

/* Backend code-generation interface implemented by each target (be_*.c). */
typedef struct {
    const char *name;
    u8 word_bytes;
    u8 addr_bytes;
    u8 float_bytes;

    u16  (*cur_addr)(void);
    void (*fn_begin)(const char *name, u8 param_cnt, const LocalVar *locals, u8 total_locals, u8 is_recursive);
    void (*fn_end)(const FuncSig *sig, const LocalVar *locals, u8 total_locals);
    void (*call)(u16 target_addr);
    void (*call_extern)(const FuncSig *sig);

    void (*push_w)(i32 val);
    void (*push_a)(u32 val);
    void (*push_f)(double val);
    void (*push_i32)(i32 val);
    void (*push_data)(u16 data_offset);
    void (*local_op)(LocalOp op, u8 slot, StackType type, u16 stack_bytes);

    void (*stack_op)(StackOp op, StackType t1, StackType t2);
    void (*op)(IrOp op);
    int  (*add_imm)(i32 imm);
    int  (*cmp_imm)(IrOp op, i32 imm);
    int  (*shift_imm)(IrOp op, u8 count);
    int  (*mem_imm)(IrOp op, i32 off);
    int  (*data_mem)(IrOp op, u16 data_offset);
    int  (*local_mem)(u8 slot, i32 off, IrOp op, u16 stack_bytes);

    u16  (*emit_br_fwd)(BranchCond cond);
    void (*emit_br_back)(BranchCond cond, u16 target_addr);
    void (*patch_fwd)(u16 patch_offset);
    void (*patch_call)(u16 call_pc, u16 call_addr, u16 target_addr);
    u16  (*stack_data_off)(void);
} Backend;

/* Error reporting (implemented in frontend.c) */
void ir4_die(const char *msg);

/* Platform / output-format hooks (startup stub, epilogue, file writer, runner). */
typedef struct {
    const char *flag;
    const Backend *be;
    u16 default_base;
    const char *req_platform;
    int needs_main;
    void (*begin)(u16 base);
    void (*end)(int main_idx);
    int (*write)(FILE *fp, u16 base);
    void (*run)(u8 main_idx);
} Platform;

extern const Backend be_z80;
extern const Backend be_6502;
extern const Backend be_arm64;
extern const Backend be_rv32;

extern const Platform plat_z80_cpm;
extern const Platform plat_6502_sim;
extern const Platform plat_6502_c64;
extern const Platform plat_arm64_jit;
extern const Platform plat_rv32_virt;
extern const Platform plat_rv32_obj;

void be_6502_set_zp(u8 zp);
u16 be_6502_stack_data_off(void);
void be_rv32_set_obj(int obj);

#endif /* IR4_H */

#ifndef IR4C_H
#define IR4C_H

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

#define MAX_TOK    32
#define MAX_SYMS   48
#define MAX_LOCALS 16
#define MAX_STACK  32
#define MAX_BLOCKS 16
#define MAX_CODE   16384
#define MAX_DATA   4096
#define MAX_RELOCS 128

/* Stack types (v0.3) */
enum {
    TY_NONE = 0,
    TY_W,
    TY_A,
    TY_F
};

/* Stack operations */
enum {
    OP_DUP = 0,
    OP_DROP,
    OP_SWAP,
    OP_OVER
};

/* ALU & bitwise operations */
enum {
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
    OP_FNEG
};

/* Comparison operations */
enum {
    OP_EQ = 0,
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
    OP_FGT
};

/* Memory operations */
enum {
    OP_I8FETCH = 0,
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
    OP_FSTORE
};

/* Type conversion operations */
enum {
    OP_SEXT = 0,
    OP_W2A,
    OP_A2W,
    OP_W2F,
    OP_F2W
};

typedef struct {
    char name[MAX_TOK];
    u8   type;
    u8   slot;
} LocalVar;

typedef struct {
    char name[MAX_TOK];
    u16  addr;
    u8   in_types[8];
    u8   in_count;
    u8   out_types[4];
    u8   out_count;
} FuncSig;

typedef struct {
    const char *name;
    u8 word_bytes;
    u8 addr_bytes;
    u8 float_bytes;

    u16  (*cur_addr)(void);
    void (*fn_begin)(const char *name, u8 param_cnt, const LocalVar *locals, u8 total_locals);
    void (*fn_end)(const FuncSig *sig, const LocalVar *locals, u8 total_locals);
    void (*call)(u16 target_addr);

    void (*push_w)(i32 val);
    void (*push_a)(u32 val);
    void (*push_f)(double val);
    void (*push_data)(u16 data_offset);
    void (*local_get)(u8 slot, u8 type, u16 stack_bytes);
    void (*local_set)(u8 slot, u8 type, u16 stack_bytes);

    void (*stack_op)(u8 op, u8 t1, u8 t2);
    void (*alu_op)(u8 op);
    void (*cmp_op)(u8 op);
    void (*mem_op)(u8 op);
    void (*conv_op)(u8 op);

    u16  (*emit_jz_fwd)(void);
    u16  (*emit_jmp_fwd)(void);
    void (*emit_jmp_back)(u16 target_addr);
    void (*emit_jz_back)(u16 target_addr);
    void (*patch_fwd)(u16 patch_offset);
} Backend;

/* Shared code buffer API (implemented in codebuf.c) */
void      cb_reset(u16 base);
u16       cb_pc(void);
u16       cb_cur_addr(void);
const u8 *cb_data(void);
void      emit8(u8 b);
void      emit16(u16 w);
void      emit8_8(u8 a, u8 b);
void      emit8_16(u8 a, u16 w);
void      emit8_8_8(u8 a, u8 b, u8 c);
void      cb_rewind(u16 n);
void      patch16(u16 patch_offset, u16 target_addr);
u16       cb_alloc_data(u16 size, u8 align);
u16       cb_data_size(void);
void      cb_add_data_reloc(u16 patch_offset, u16 data_offset);
void      cb_add_data_reloc_split(u16 lo_offset, u16 hi_offset, u16 data_offset);
void      cb_patch_data_relocs(u16 data_base);

/* Frontend entry points (implemented in frontend.c) */
void            ir4_die(const char *msg);
void            ir4_compile_stream(FILE *in, const char *path, const Backend *target_be);
int             ir4_find_func(const char *name);
u16             ir4_func_addr(u8 idx);
const FuncSig  *ir4_func_sig(u8 idx);

/* Available backends & platform stubs */
extern const Backend be_z80;
extern const Backend be_6502;
extern const Backend be_arm64;

void be_z80_cpm_begin(void);
void be_z80_cpm_end(u8 main_idx);
void be_arm64_run_jit(u8 main_idx);

#endif /* IR4C_H */

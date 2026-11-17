#include "codebuf.h"
#include "frontend.h"

#define MAX_INCLUDE_DEPTH 16
#define MAX_INCLUDES      64
#define MAX_PATH         256
#define MAX_IR          8192
#define MAX_SYMS          255
#define MAX_STACK         32
#define MAX_BLOCKS        16

typedef enum {
    BLOCK_IF = 1,
    BLOCK_BEGIN = 2
} BlockKind;

typedef struct {
    BlockKind kind;
    u16 patch_else;
    u16 patch_end;
    u16 loop_addr;
    u8  saved_sp;
    u8  saved_stack[MAX_STACK];
    u8  else_sp;
    u8  else_stack[MAX_STACK];
    u8  has_else;
    u8  saved_unreach;
    u8  if_unreach;
} BlockInfo;

typedef enum {
    IR_FN_END,
    IR_PUSH_W,
    IR_PUSH_A,
    IR_PUSH_F,
    IR_PUSH_DATA,
    IR_PUSH_HEAP_START,
    IR_LOCAL_OP,
    IR_STACK_OP,
    IR_ALU_OP,
    IR_CALL,
    IR_CALL_EXTERN,
    IR_BR_FWD,
    IR_BR_BACK
} IrKind;

typedef struct {
    IrKind kind;
    union {
        i32    w;
        u32    a;
        double f;
        u16    offset;
        struct { LocalOp op; u8 slot; StackType type; u16 stack_bytes; } loc;
        struct { StackOp op; StackType t1; StackType t2; } stk;
        IrOp   op;
        u8     func_idx;
        struct { BranchCond cond; u16 target; } br;
    } u;
} IrInstr;

typedef struct {
    u16      ir_start;
    u16      ir_len;
    u8       param_cnt;
    u8       local_count;
    LocalVar locals[MAX_LOCALS];
    u8       reachable;
    u8       is_code;
} WordIR;

static IrInstr ir_buf[MAX_IR];
static u16     ir_count = 0;
static u16     fold_fence = 0;
static WordIR  words[MAX_SYMS];
static u8      raw_code_buf[MAX_CODE];
static u16     raw_code_len = 0;

typedef struct {
    FILE *fp;
    char  path[MAX_PATH];
    u16   line_num;
} SrcFile;

static SrcFile        src_stack[MAX_INCLUDE_DEPTH];
static u8             src_depth = 0;
static char           seen_files[MAX_INCLUDES][MAX_PATH];
static u8             seen_count = 0;
static const Backend *be;
static const char    *platform_name = NULL;

static FuncSig   funcs[MAX_SYMS];
static u8        func_count = 0;

typedef struct {
    char name[MAX_TOK];
    u16  offset;
    u8   type; /* TY_NONE for :bytes / data buffers; TY_W/TY_A/TY_F for typed globals */
} BufferSym;

static BufferSym buffers[MAX_SYMS];
static u8        buffer_count = 0;

static LocalVar  locals[MAX_LOCALS];
static u8        local_count = 0;
static u8        cur_outs[4];
static u8        cur_out_count = 0;

static u8        tstack[MAX_STACK];
static u8        tsp = 0;

static BlockInfo blocks[MAX_BLOCKS];
static u8        bsp = 0;

void ir4_die(const char *msg) {
    if (src_depth > 0) {
        const SrcFile *cur = &src_stack[src_depth - 1];
        fprintf(stderr, "ir4c error (%s:%u): %s\n", cur->path, cur->line_num, msg);
    } else {
        fprintf(stderr, "ir4c error: %s\n", msg);
    }
    exit(1);
}

static const char *type_name(u8 t) {
    if (t == TY_W) return ":i";
    if (t == TY_A) return ":a";
    if (t == TY_F) return ":f";
    return "?";
}

static u8 parse_type_tok(const char *tok) {
    if (strcmp(tok, ":i") == 0) return TY_W;
    if (strcmp(tok, ":a") == 0) return TY_A;
    if (strcmp(tok, ":f") == 0) return TY_F;
    return TY_NONE;
}

static int skip_ws_and_comments(void) {
    for (;;) {
        SrcFile *cur;
        int c;
        if (src_depth == 0) return EOF;
        cur = &src_stack[src_depth - 1];
        c = fgetc(cur->fp);
        if (c == EOF) {
            if (src_depth > 1) {
                fclose(cur->fp);
                src_depth--;
                continue;
            }
            return EOF;
        }
        if (c == '\n') { cur->line_num++; continue; }
        if (isspace((unsigned char)c)) continue;
        if (c == '/') {
            int c2 = fgetc(cur->fp);
            if (c2 == '/') {
                while ((c = fgetc(cur->fp)) != EOF && c != '\n') ;
                if (c == '\n') cur->line_num++;
                continue;
            }
            if (c2 != EOF) ungetc(c2, cur->fp);
        }
        return c;
    }
}

static int next_tok(char *buf) {
    int c = skip_ws_and_comments();
    u8 len = 0;

    if (c == EOF) return 0;

    {
        SrcFile *cur = &src_stack[src_depth - 1];
        while (c != EOF && !isspace((unsigned char)c)) {
            if (len >= MAX_TOK - 1) ir4_die("token too long");
            buf[len++] = (char)c;
            c = fgetc(cur->fp);
        }
        if (c == '\n') cur->line_num++;
    }
    buf[len] = '\0';
    return 1;
}

static void resolve_path(const char *base_path, const char *rel, char *out, size_t out_sz) {
    const char *slash;
    size_t dir_len = 0;
    size_t rel_len;

    while (rel[0] == '.' && rel[1] == '/') {
        rel += 2;
    }

    if (rel[0] != '/' && base_path != NULL) {
        slash = strrchr(base_path, '/');
        if (slash != NULL) {
            dir_len = (size_t)(slash - base_path + 1);
        }
    }

    rel_len = strlen(rel);
    if (dir_len + rel_len >= out_sz) {
        ir4_die("required file path too long");
    }
    if (dir_len > 0) {
        memcpy(out, base_path, dir_len);
    }
    memcpy(out + dir_len, rel, rel_len + 1);
}

static FILE *try_open_require(const char *path, int *already_seen) {
    u8 i;
    *already_seen = 0;
    for (i = 0; i < seen_count; i++) {
        if (strcmp(seen_files[i], path) == 0) {
            *already_seen = 1;
            return NULL;
        }
    }
    return fopen(path, "r");
}

static void push_require(const char *name) {
    char cands[3][MAX_PATH];
    int n_cands = 0;
    int c;
    char path[MAX_PATH];
    FILE *fp = NULL;
    int seen = 0;

    if (name[0] != '/') {
        if (platform_name != NULL && platform_name[0] != '\0') {
            if (strlen(platform_name) + 1 + strlen(name) >= MAX_PATH) {
                ir4_die("required file path too long");
            }
            sprintf(cands[n_cands++], "%s/%s", platform_name, name);
        }
        if (strlen(be->name) + 1 + strlen(name) >= MAX_PATH) {
            ir4_die("required file path too long");
        }
        sprintf(cands[n_cands++], "%s/%s", be->name, name);
    }
    if (strlen(name) >= MAX_PATH) {
        ir4_die("required file path too long");
    }
    strcpy(cands[n_cands++], name);

    for (c = 0; c < n_cands; c++) {
        resolve_path(src_stack[src_depth - 1].path, cands[c], path, sizeof(path));
        fp = try_open_require(path, &seen);
        if (seen) return;
        if (fp) break;
    }

    if (!fp && name[0] != '/') {
        for (c = 0; c < n_cands; c++) {
            if (4 + strlen(cands[c]) >= sizeof(path)) continue;
            sprintf(path, "lib/%s", cands[c]);
            fp = try_open_require(path, &seen);
            if (seen) return;
            if (fp) break;
        }
    }

    if (!fp) {
        const SrcFile *cur = &src_stack[src_depth - 1];
        fprintf(stderr, "ir4c error (%s:%u): cannot open required file '%s'\n",
                cur->path, cur->line_num, name);
        exit(1);
    }
    if (seen_count >= MAX_INCLUDES) {
        fclose(fp);
        ir4_die("too many required files");
    }
    if (src_depth >= MAX_INCLUDE_DEPTH) {
        fclose(fp);
        ir4_die("require nesting too deep");
    }

    memcpy(seen_files[seen_count++], path, strlen(path) + 1);
    src_stack[src_depth].fp = fp;
    memcpy(src_stack[src_depth].path, path, strlen(path) + 1);
    src_stack[src_depth].line_num = 1;
    src_depth++;
}

static int parse_int(const char *s, i32 *val) {
    char *endp;
    int base = 0;
    const char *p = s;

    if (*p == '$') {
        base = 16;
        p++;
        if (*p == '\0') return 0;
    } else {
        if (*p == '-' || *p == '+') p++;
        if (!isdigit((unsigned char)*p)) return 0;
        p = s;
    }

    *val = (i32)strtol(p, &endp, base);
    return (*endp == '\0');
}

static int parse_float(const char *s, double *val) {
    char *endp;
    const char *p = s;

    if (*p == '-' || *p == '+') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return 0;
    if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    if (*p == '.' && !isdigit((unsigned char)p[1])) return 0;
    if (strchr(p, '.') == NULL && strchr(p, 'e') == NULL && strchr(p, 'E') == NULL) {
        return 0;
    }

    *val = strtod(s, &endp);
    return (*endp == '\0');
}

static u16 tstack_bytes(void) {
    u16 bytes = 0;
    u8 i;
    for (i = 0; i < tsp; i++) {
        if (tstack[i] == TY_F)      bytes += be->float_bytes;
        else if (tstack[i] == TY_A) bytes += be->addr_bytes;
        else                        bytes += be->word_bytes;
    }
    return bytes;
}

static void tpush(u8 t) {
    if (tsp >= MAX_STACK) ir4_die("type stack overflow");
    tstack[tsp++] = t;
}

static u8 tpop(void) {
    if (tsp == 0) ir4_die("type stack underflow");
    return tstack[--tsp];
}

static void tpop_expect(u8 expected) {
    u8 got = tpop();
    if (got != expected) {
        const SrcFile *cur = &src_stack[src_depth - 1];
        fprintf(stderr, "ir4c error (%s:%u): expected %s, got %s\n",
                cur->path, cur->line_num, type_name(expected), type_name(got));
        exit(1);
    }
}

static void check_return_stack(void) {
    u8 i;
    if (tsp != cur_out_count) ir4_die("return stack depth mismatch");
    for (i = 0; i < cur_out_count; i++) {
        if (tstack[i] != cur_outs[i]) ir4_die("return stack type mismatch");
    }
}

static int find_local(const char *name) {
    u8 i;
    for (i = 0; i < local_count; i++) {
        if (strcmp(locals[i].name, name) == 0) return (int)i;
    }
    return -1;
}

int ir4_find_func(const char *name) {
    u8 i;
    for (i = 0; i < func_count; i++) {
        if (strcmp(funcs[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int find_buffer(const char *name) {
    u8 i;
    for (i = 0; i < buffer_count; i++) {
        if (strcmp(buffers[i].name, name) == 0) return (int)i;
    }
    return -1;
}

u16 ir4_func_addr(u8 idx) {
    if (idx >= func_count) ir4_die("function index out of bounds");
    return funcs[idx].addr;
}

const FuncSig *ir4_func_sig(u8 idx) {
    if (idx >= func_count) ir4_die("function index out of bounds");
    return &funcs[idx];
}

typedef struct {
    const char *name;
    IrOp op;
    u8 in1;
    u8 in2;
    u8 out;
} BuiltinDef;

static const BuiltinDef builtins[] = {
    /* Arithmetic & bitwise */
    { "+",       OP_ADD,      TY_W, TY_W,    TY_W },
    { "+",       OP_AADD,     TY_W, TY_A,    TY_A },
    { "+",       OP_FADD,     TY_F, TY_F,    TY_F },
    { "-",       OP_SUB,      TY_W, TY_W,    TY_W },
    { "-",       OP_ASUB,     TY_W, TY_A,    TY_A },
    { "-",       OP_ADIFF,    TY_A, TY_A,    TY_W },
    { "-",       OP_FSUB,     TY_F, TY_F,    TY_F },
    { "*",       OP_MUL,      TY_W, TY_W,    TY_W },
    { "*",       OP_FMUL,     TY_F, TY_F,    TY_F },
    { "/",       OP_DIV,      TY_W, TY_W,    TY_W },
    { "/",       OP_FDIV,     TY_F, TY_F,    TY_F },
    { "mod",     OP_MOD,      TY_W, TY_W,    TY_W },
    { "u/",      OP_UDIV,     TY_W, TY_W,    TY_W },
    { "umod",    OP_UMOD,     TY_W, TY_W,    TY_W },
    { "and",     OP_AND,      TY_W, TY_W,    TY_W },
    { "or",      OP_OR,       TY_W, TY_W,    TY_W },
    { "xor",     OP_XOR,      TY_W, TY_W,    TY_W },
    { "invert",  OP_INVERT,   TY_W, TY_NONE, TY_W },
    { "<<",      OP_LSHIFT,   TY_W, TY_W,    TY_W },
    { ">>",      OP_ARSHIFT,  TY_W, TY_W,    TY_W },
    { "u>>",     OP_RSHIFT,   TY_W, TY_W,    TY_W },
    { "fnegate", OP_FNEG,     TY_F, TY_NONE, TY_F },

    /* Comparisons */
    { "==",      OP_EQ,       TY_W, TY_W,    TY_W },
    { "==",      OP_AEQ,      TY_A, TY_A,    TY_W },
    { "==",      OP_FEQ,      TY_F, TY_F,    TY_W },
    { "!=",      OP_NE,       TY_W, TY_W,    TY_W },
    { "!=",      OP_ANE,      TY_A, TY_A,    TY_W },
    { "!=",      OP_FNE,      TY_F, TY_F,    TY_W },
    { "<",       OP_LT,       TY_W, TY_W,    TY_W },
    { "<",       OP_FLT,      TY_F, TY_F,    TY_W },
    { ">",       OP_GT,       TY_W, TY_W,    TY_W },
    { ">",       OP_FGT,      TY_F, TY_F,    TY_W },
    { "<=",      OP_LE,       TY_W, TY_W,    TY_W },
    { ">=",      OP_GE,       TY_W, TY_W,    TY_W },
    { "u<",      OP_ULT,      TY_W, TY_W,    TY_W },
    { "u<",      OP_ALT,      TY_A, TY_A,    TY_W },
    { "u>",      OP_UGT,      TY_W, TY_W,    TY_W },
    { "u>",      OP_AGT,      TY_A, TY_A,    TY_W },
    { "u<=",     OP_ULE,      TY_W, TY_W,    TY_W },
    { "u>=",     OP_UGE,      TY_W, TY_W,    TY_W },

    /* Memory load/store */
    { "@i8",     OP_I8FETCH,  TY_A, TY_NONE, TY_W },
    { "@u8",     OP_U8FETCH,  TY_A, TY_NONE, TY_W },
    { "!i8",     OP_I8STORE,  TY_A, TY_W,    TY_NONE },
    { "@i16",    OP_I16FETCH, TY_A, TY_NONE, TY_W },
    { "@u16",    OP_U16FETCH, TY_A, TY_NONE, TY_W },
    { "!i16",    OP_I16STORE, TY_A, TY_W,    TY_NONE },
    { "@i",      OP_FETCH,    TY_A, TY_NONE, TY_W },
    { "!i",      OP_STORE,    TY_A, TY_W,    TY_NONE },
    { "@a",      OP_AFETCH,   TY_A, TY_NONE, TY_A },
    { "!a",      OP_ASTORE,   TY_A, TY_A,    TY_NONE },
    { "@f",      OP_FFETCH,   TY_A, TY_NONE, TY_F },
    { "!f",      OP_FSTORE,   TY_A, TY_F,    TY_NONE },

    /* Type conversions */
    { "sext",    OP_SEXT,     TY_W, TY_NONE, TY_W },
    { ":a",      OP_W2A,      TY_W, TY_NONE, TY_A },
    { ":i",      OP_A2W,      TY_A, TY_NONE, TY_W },
    { ":f",      OP_W2F,      TY_W, TY_NONE, TY_F },
    { ":i",      OP_F2W,      TY_F, TY_NONE, TY_W }
};

static IrInstr *emit_ir(IrKind kind, WordIR *w) {
    IrInstr *ins;
    if (ir_count >= MAX_IR) {
        ir4_die("IR buffer overflow");
    }
    ins = &ir_buf[ir_count++];
    memset(ins, 0, sizeof(IrInstr));
    ins->kind = kind;
    w->ir_len++;
    return ins;
}

static int try_fold_const_w(IrOp op, i32 a, i32 b, i32 *out) {
    if (be->word_bytes == 2) {
        i16 sa = (i16)a;
        i16 sb = (i16)b;
        u16 ua = (u16)sa;
        u16 ub = (u16)sb;
        switch (op) {
        case OP_ADD: *out = (i32)(i16)(ua + ub); return 1;
        case OP_SUB: *out = (i32)(i16)(ua - ub); return 1;
        case OP_EQ:  *out = (sa == sb) ? -1 : 0; return 1;
        case OP_NE:  *out = (sa != sb) ? -1 : 0; return 1;
        case OP_LT:  *out = (sa <  sb) ? -1 : 0; return 1;
        case OP_GT:  *out = (sa >  sb) ? -1 : 0; return 1;
        case OP_LE:  *out = (sa <= sb) ? -1 : 0; return 1;
        case OP_GE:  *out = (sa >= sb) ? -1 : 0; return 1;
        case OP_ULT: *out = (ua <  ub) ? -1 : 0; return 1;
        case OP_UGT: *out = (ua >  ub) ? -1 : 0; return 1;
        case OP_ULE: *out = (ua <= ub) ? -1 : 0; return 1;
        case OP_UGE: *out = (ua >= ub) ? -1 : 0; return 1;
        default: return 0;
        }
    }
    if (be->word_bytes == 8) {
        if ((op == OP_ADD || op == OP_SUB) &&
            (a < -0x3FFFFFFF || a > 0x3FFFFFFF || b < -0x3FFFFFFF || b > 0x3FFFFFFF)) {
            return 0;
        }
        if ((op == OP_ULT || op == OP_UGT || op == OP_ULE || op == OP_UGE) &&
            (a < 0 || b < 0)) {
            return 0;
        }
    }
    switch (op) {
    case OP_ADD: *out = (i32)((u32)a + (u32)b); return 1;
    case OP_SUB: *out = (i32)((u32)a - (u32)b); return 1;
    case OP_EQ:  *out = (a == b) ? -1 : 0; return 1;
    case OP_NE:  *out = (a != b) ? -1 : 0; return 1;
    case OP_LT:  *out = (a <  b) ? -1 : 0; return 1;
    case OP_GT:  *out = (a >  b) ? -1 : 0; return 1;
    case OP_LE:  *out = (a <= b) ? -1 : 0; return 1;
    case OP_GE:  *out = (a >= b) ? -1 : 0; return 1;
    case OP_ULT: *out = ((u32)a <  (u32)b) ? -1 : 0; return 1;
    case OP_UGT: *out = ((u32)a >  (u32)b) ? -1 : 0; return 1;
    case OP_ULE: *out = ((u32)a <= (u32)b) ? -1 : 0; return 1;
    case OP_UGE: *out = ((u32)a >= (u32)b) ? -1 : 0; return 1;
    default: return 0;
    }
}

static IrOp swapped_store_op(IrOp op) {
    switch (op) {
    case OP_I8STORE:  return OP_SI8STORE;
    case OP_I16STORE: return OP_SI16STORE;
    case OP_STORE:    return OP_SSTORE;
    case OP_ASTORE:   return OP_SASTORE;
    case OP_FSTORE:   return OP_SFSTORE;
    default:          return op;
    }
}

static int handle_builtin(const char *tok, WordIR *w) {
    u8 a, b;
    size_t i;
    IrInstr *ins;

    /* Stack manipulation (polymorphic) */
    if (strcmp(tok, "dup") == 0) {
        a = tpop(); tpush(a); tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DUP; ins->u.stk.t1 = (StackType)a; ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "drop") == 0) {
        a = tpop();
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DROP; ins->u.stk.t1 = (StackType)a; ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "swap") == 0) {
        b = tpop(); a = tpop(); tpush(b); tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_SWAP; ins->u.stk.t1 = (StackType)b; ins->u.stk.t2 = (StackType)a;
        return 1;
    }
    if (strcmp(tok, "over") == 0) {
        b = tpop(); a = tpop(); tpush(a); tpush(b); tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_OVER; ins->u.stk.t1 = (StackType)b; ins->u.stk.t2 = (StackType)a;
        return 1;
    }

    {
        int matched_name = 0;
        for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            int need;
            if (strcmp(tok, builtins[i].name) != 0) continue;
            matched_name = 1;
            need = (builtins[i].in2 != TY_NONE) ? 2 : 1;
            if (tsp < need) continue;
            if (tstack[tsp - 1] != builtins[i].in1) continue;
            if (builtins[i].in2 != TY_NONE && tstack[tsp - 2] != builtins[i].in2) continue;

            tsp -= need;
            if (builtins[i].out != TY_NONE) tpush(builtins[i].out);
            if (builtins[i].op == OP_W2A && w->ir_len >= fold_fence + 1) {
                IrInstr *p1 = &ir_buf[w->ir_start + w->ir_len - 1];
                if (p1->kind == IR_PUSH_W) {
                    p1->kind = IR_PUSH_A;
                    p1->u.a = (u32)p1->u.w;
                    return 1;
                }
            }
            if (w->ir_len >= fold_fence + 1) {
                IrOp sop = swapped_store_op(builtins[i].op);
                if (sop != builtins[i].op) {
                    IrInstr *p1 = &ir_buf[w->ir_start + w->ir_len - 1];
                    if (p1->kind == IR_STACK_OP && p1->u.stk.op == OP_SWAP) {
                        p1->kind = IR_ALU_OP;
                        p1->u.op = sop;
                        return 1;
                    }
                }
            }
            if (w->ir_len >= fold_fence + 2) {
                IrInstr *p1 = &ir_buf[w->ir_start + w->ir_len - 2];
                IrInstr *p2 = &ir_buf[w->ir_start + w->ir_len - 1];
                i32 folded;
                if (p1->kind == IR_PUSH_W && p2->kind == IR_PUSH_W &&
                    try_fold_const_w(builtins[i].op, p1->u.w, p2->u.w, &folded)) {
                    p1->u.w = folded;
                    w->ir_len--;
                    ir_count--;
                    return 1;
                }
            }
            ins = emit_ir(IR_ALU_OP, w);
            ins->u.op = builtins[i].op;
            return 1;
        }
        if (matched_name) {
            char msg[128];
            sprintf(msg, "type mismatch or stack underflow for '%s'", tok);
            ir4_die(msg);
        }
    }

    return 0;
}

typedef struct {
    u8   src_slot;
    u8   dst_slot;
    u16  size;
    char name[MAX_TOK];
} BytesParamCopy;

static BytesParamCopy cur_bytes_copies[8];
static u8 cur_bytes_copy_count = 0;
static u8 cur_bytes_cnt_slot = 0;

static void flush_bytes_params(u8 *params_flushed) {
    u8 k;
    if (*params_flushed) return;
    *params_flushed = 1;
    if (cur_bytes_copy_count == 0) return;
    for (k = 0; k < cur_bytes_copy_count; k++) {
        if (local_count >= MAX_LOCALS) ir4_die("too many parameters/locals");
        memcpy(locals[local_count].name, cur_bytes_copies[k].name,
               strlen(cur_bytes_copies[k].name) + 1);
        locals[local_count].type = TY_A;
        locals[local_count].slot = local_count;
        locals[local_count].size = cur_bytes_copies[k].size;
        cur_bytes_copies[k].dst_slot = local_count;
        local_count++;
    }
    if (local_count >= MAX_LOCALS) ir4_die("too many parameters/locals");
    memcpy(locals[local_count].name, "__bpcnt", 8);
    locals[local_count].type = TY_W;
    locals[local_count].slot = local_count;
    locals[local_count].size = 0;
    cur_bytes_cnt_slot = local_count;
    local_count++;
}

static void parse_sig_body(FuncSig *sig, u8 *param_cnt_out, int allow_locals) {
    char tok[MAX_TOK];
    u8 param_cnt = 0;
    u8 in_locals = 0;
    u8 in_outs = 0;
    u8 params_flushed = 0;

    local_count = 0;
    cur_out_count = 0;
    cur_bytes_copy_count = 0;
    cur_bytes_cnt_slot = 0;

    /* Parse inputs | locals -> outputs ) (after '(' has been consumed) */
    while (next_tok(tok)) {
        u8 ty;
        char ty_tok[MAX_TOK];
        if (strcmp(tok, ")") == 0) {
            flush_bytes_params(&params_flushed);
            if (in_outs && sig->out_count == 0) ir4_die("expected return type after '->'");
            if (param_cnt_out) *param_cnt_out = param_cnt;
            return;
        }
        if (strcmp(tok, "|") == 0) {
            if (!allow_locals) ir4_die("locals '|' not allowed in extern signature");
            if (in_outs) ir4_die("'|' not allowed after '->' in signature");
            flush_bytes_params(&params_flushed);
            in_locals = 1;
            continue;
        }
        if (strcmp(tok, "->") == 0) {
            if (in_outs) ir4_die("duplicate '->' in signature");
            flush_bytes_params(&params_flushed);
            in_outs = 1;
            continue;
        }

        if (in_outs) {
            ty = parse_type_tok(tok);
            if (ty == TY_NONE) {
                if (!allow_locals && cur_out_count == 0 && sig->out_reg[0] == '\0') {
                    size_t len = strlen(tok);
                    memcpy(sig->out_reg, tok, len + 1);
                    if (!next_tok(tok)) ir4_die("expected type token (:i, :a, :f) after extern register");
                    ty = parse_type_tok(tok);
                }
                if (ty == TY_NONE) ir4_die("expected type token (:i, :a, :f) in signature");
            }
            if (cur_out_count >= 4) ir4_die("too many return types");
            sig->out_types[sig->out_count++] = ty;
            cur_outs[cur_out_count++] = ty;
        } else {
            if (local_count >= MAX_LOCALS) ir4_die("too many parameters/locals");
            if (parse_type_tok(tok) != TY_NONE) {
                ir4_die("expected identifier before type in signature");
            }
            memcpy(locals[local_count].name, tok, strlen(tok) + 1);
            if (!next_tok(ty_tok)) ir4_die("expected type token (:i, :a, :f) after identifier in signature");
            if (strcmp(ty_tok, ":bytes") == 0) {
                i32 sz = 0;
                if (!next_tok(ty_tok) || !parse_int(ty_tok, &sz) || sz <= 0 || sz > MAX_DATA) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                if (in_locals) {
                    locals[local_count].type = TY_A;
                    locals[local_count].slot = local_count;
                    locals[local_count].size = (u16)sz;
                    local_count++;
                } else {
                    if (!allow_locals) ir4_die("':bytes' not allowed in extern signature");
                    if (sig->in_count >= 8) ir4_die("too many parameters");
                    cur_bytes_copies[cur_bytes_copy_count].src_slot = local_count;
                    cur_bytes_copies[cur_bytes_copy_count].size = (u16)sz;
                    memcpy(cur_bytes_copies[cur_bytes_copy_count].name, tok, strlen(tok) + 1);
                    cur_bytes_copy_count++;
                    sprintf(locals[local_count].name, "__bp%u", (unsigned)param_cnt);
                    locals[local_count].type = TY_A;
                    locals[local_count].slot = local_count;
                    locals[local_count].size = 0;
                    sig->in_types[sig->in_count] = TY_A;
                    sig->in_count++;
                    param_cnt++;
                    local_count++;
                }
                continue;
            }
            ty = parse_type_tok(ty_tok);
            if (ty == TY_NONE) ir4_die("expected type token (:i, :a, :f) in signature");
            locals[local_count].type = ty;
            locals[local_count].slot = local_count;
            locals[local_count].size = 0;
            if (!in_locals) {
                if (sig->in_count >= 8) ir4_die("too many parameters");
                sig->in_types[sig->in_count] = ty;
                memcpy(sig->in_regs[sig->in_count], locals[local_count].name,
                       strlen(locals[local_count].name) + 1);
                sig->in_count++;
                param_cnt++;
            }
            local_count++;
        }
    }

    ir4_die("unexpected EOF in signature (missing ')')");
}

static void compile_body(WordIR *w);

static void emit_bytes_param_copies(WordIR *w) {
    u8 k;
    u16 wb = be->word_bytes;
    for (k = 0; k < cur_bytes_copy_count; k++) {
        IrInstr *ins;
        u16 loop_ir, jmp_done;
        u8 src = cur_bytes_copies[k].src_slot;
        u8 dst = cur_bytes_copies[k].dst_slot;
        u16 sz = cur_bytes_copies[k].size;

        ins = emit_ir(IR_PUSH_W, w); ins->u.w = (i32)sz;
        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_SET; ins->u.loc.slot = cur_bytes_cnt_slot;
        ins->u.loc.type = TY_W; ins->u.loc.stack_bytes = 0;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_GET; ins->u.loc.slot = src;
        ins->u.loc.type = TY_A; ins->u.loc.stack_bytes = 0;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_ADDR; ins->u.loc.slot = dst;
        ins->u.loc.type = TY_A; ins->u.loc.stack_bytes = wb;

        loop_ir = w->ir_len;
        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_GET; ins->u.loc.slot = cur_bytes_cnt_slot;
        ins->u.loc.type = TY_W; ins->u.loc.stack_bytes = (u16)(2 * wb);

        jmp_done = w->ir_len;
        ins = emit_ir(IR_BR_FWD, w);
        ins->u.br.cond = BR_JZ; ins->u.br.target = 0;

        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_OVER; ins->u.stk.t1 = TY_A; ins->u.stk.t2 = TY_A;
        ins = emit_ir(IR_ALU_OP, w); ins->u.op = OP_U8FETCH;
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_OVER; ins->u.stk.t1 = TY_W; ins->u.stk.t2 = TY_A;
        ins = emit_ir(IR_ALU_OP, w); ins->u.op = OP_I8STORE;

        ins = emit_ir(IR_PUSH_W, w); ins->u.w = 1;
        ins = emit_ir(IR_ALU_OP, w); ins->u.op = OP_AADD;
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_SWAP; ins->u.stk.t1 = TY_A; ins->u.stk.t2 = TY_A;
        ins = emit_ir(IR_PUSH_W, w); ins->u.w = 1;
        ins = emit_ir(IR_ALU_OP, w); ins->u.op = OP_AADD;
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_SWAP; ins->u.stk.t1 = TY_A; ins->u.stk.t2 = TY_A;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_GET; ins->u.loc.slot = cur_bytes_cnt_slot;
        ins->u.loc.type = TY_W; ins->u.loc.stack_bytes = (u16)(2 * wb);
        ins = emit_ir(IR_PUSH_W, w); ins->u.w = 1;
        ins = emit_ir(IR_ALU_OP, w); ins->u.op = OP_SUB;
        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_SET; ins->u.loc.slot = cur_bytes_cnt_slot;
        ins->u.loc.type = TY_W; ins->u.loc.stack_bytes = (u16)(2 * wb);

        ins = emit_ir(IR_BR_BACK, w);
        ins->u.br.cond = BR_JMP; ins->u.br.target = loop_ir;

        ir_buf[w->ir_start + jmp_done].u.br.target = w->ir_len;
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DROP; ins->u.stk.t1 = TY_A; ins->u.stk.t2 = TY_NONE;
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DROP; ins->u.stk.t1 = TY_A; ins->u.stk.t2 = TY_NONE;
        fold_fence = w->ir_len;
    }
}

static void compile_word(void) {
    char tok[MAX_TOK];
    char name[MAX_TOK];
    FuncSig *sig;
    WordIR *w;
    u8 fidx;
    int existing;
    u8 param_cnt = 0;

    if (!next_tok(name)) ir4_die("expected word name after 'fn'");
    if (!next_tok(tok) || strcmp(tok, "(") != 0) ir4_die("expected '(' after word name");

    existing = ir4_find_func(name);
    if (existing >= 0) {
        FuncSig fn_sig;
        if (funcs[existing].is_defined || funcs[existing].is_extern) {
            ir4_die("duplicate function definition");
        }
        fidx = (u8)existing;
        memset(&fn_sig, 0, sizeof(FuncSig));
        memcpy(fn_sig.name, name, strlen(name) + 1);
        tsp = 0;
        bsp = 0;
        parse_sig_body(&fn_sig, &param_cnt, 1);
        if (fn_sig.in_count != funcs[fidx].in_count ||
            fn_sig.out_count != funcs[fidx].out_count ||
            memcmp(fn_sig.in_types, funcs[fidx].in_types, fn_sig.in_count) != 0 ||
            memcmp(fn_sig.out_types, funcs[fidx].out_types, fn_sig.out_count) != 0) {
            ir4_die("function signature does not match 'fwd' declaration");
        }
        funcs[fidx] = fn_sig;
        funcs[fidx].is_defined = 1;
        sig = &funcs[fidx];
        w = &words[fidx];
        memset(w, 0, sizeof(WordIR));
    } else {
        if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
        fidx = func_count++;
        sig = &funcs[fidx];
        w = &words[fidx];
        memset(sig, 0, sizeof(FuncSig));
        memset(w, 0, sizeof(WordIR));
        memcpy(sig->name, name, strlen(name) + 1);
        sig->is_defined = 1;
        tsp = 0;
        bsp = 0;
        parse_sig_body(sig, &param_cnt, 1);
    }

    w->param_cnt = param_cnt;
    w->local_count = local_count;
    if (local_count > 0) {
        memcpy(w->locals, locals, local_count * sizeof(LocalVar));
    }
    emit_bytes_param_copies(w);
    compile_body(w);
}

static u8 init_funcs[MAX_SYMS];
static u8 init_count = 0;

static void raw_emit_u8(WordIR *w, u8 b) {
    if (raw_code_len >= MAX_CODE) ir4_die("code buffer overflow");
    raw_code_buf[raw_code_len++] = b;
    w->ir_len++;
}

static void compile_code(void) {
    char tok[MAX_TOK];
    char name[MAX_TOK];
    FuncSig *sig;
    WordIR *w;
    u8 fidx;
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;

    if (!next_tok(name)) ir4_die("expected word name after 'code'");
    if (ir4_find_func(name) >= 0) ir4_die("duplicate function definition");
    if (!next_tok(tok) || strcmp(tok, "(") != 0) ir4_die("expected '(' after 'code' name");
    if (func_count >= MAX_SYMS) ir4_die("too many word definitions");

    fidx = func_count++;
    sig = &funcs[fidx];
    w = &words[fidx];
    memset(sig, 0, sizeof(FuncSig));
    memset(w, 0, sizeof(WordIR));
    memcpy(sig->name, name, strlen(name) + 1);
    sig->is_defined = 1;
    parse_sig_body(sig, NULL, 0);

    w->is_code = 1;
    w->ir_start = raw_code_len;
    w->ir_len = 0;

    while (next_tok(tok)) {
        if (strcmp(tok, "end") == 0) {
            if (has_pending) ir4_die("missing width (:i8, :i16, :i32) in 'code'");
            return;
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending) ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255) ir4_die(":i8 value out of range");
            raw_emit_u8(w, (u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            u16 v;
            if (!has_pending) ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535) ir4_die(":i16 value out of range");
            v = (u16)(pending & 0xFFFF);
            raw_emit_u8(w, (u8)(v & 0xFF));
            raw_emit_u8(w, (u8)((v >> 8) & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i32") == 0) {
            u32 v;
            if (!has_pending) ir4_die("expected integer before ':i32'");
            v = (u32)pending;
            raw_emit_u8(w, (u8)(v & 0xFF));
            raw_emit_u8(w, (u8)((v >> 8) & 0xFF));
            raw_emit_u8(w, (u8)((v >> 16) & 0xFF));
            raw_emit_u8(w, (u8)((v >> 24) & 0xFF));
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending) ir4_die("missing width (:i8, :i16, :i32) before literal in 'code'");
            pending = num_val;
            has_pending = 1;
        } else {
            ir4_die("unexpected token in 'code'");
        }
    }
    ir4_die("unexpected EOF in 'code' (missing 'end')");
}

static void compile_init(void) {
    FuncSig *sig;
    WordIR *w;
    u8 fidx;

    if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
    fidx = func_count++;
    sig = &funcs[fidx];
    w = &words[fidx];
    memset(sig, 0, sizeof(FuncSig));
    memset(w, 0, sizeof(WordIR));
    sprintf(sig->name, "__init_%u", (unsigned)init_count);
    sig->is_defined = 1;
    init_funcs[init_count++] = fidx;

    tsp = 0;
    bsp = 0;
    cur_out_count = 0;
    local_count = 0;
    compile_body(w);
}

#define MAX_LABELS     64
#define MAX_LABEL_REFS 256

typedef struct {
    char name[MAX_TOK];
    u16  target_ir;
    u8   defined;
} LabelInfo;

typedef struct {
    u16 ir_idx;
    u16 label_idx;
} LabelRef;

static IrOp global_fetch_op(u8 ty) {
    if (ty == TY_A) return OP_AFETCH;
    if (ty == TY_F) return OP_FFETCH;
    return OP_FETCH;
}

static IrOp global_store_op(u8 ty) {
    if (ty == TY_A) return OP_ASTORE;
    if (ty == TY_F) return OP_FSTORE;
    return OP_STORE;
}

static u16 global_type_bytes(u8 ty) {
    if (ty == TY_A) return be->addr_bytes;
    if (ty == TY_F) return be->float_bytes;
    return be->word_bytes;
}

static int find_or_add_label(LabelInfo *labels, u16 *label_count, const char *name) {
    u16 i;
    for (i = 0; i < *label_count; i++) {
        if (strcmp(labels[i].name, name) == 0) return (int)i;
    }
    if (*label_count >= MAX_LABELS) ir4_die("too many labels in function");
    strcpy(labels[*label_count].name, name);
    labels[*label_count].target_ir = 0;
    labels[*label_count].defined = 0;
    return (int)(*label_count)++;
}

static void emit_label_br(WordIR *w, LabelInfo *labels, LabelRef *refs,
                          u16 *ref_count, u16 lidx, BranchCond cond) {
    IrInstr *ins;
    if (labels[lidx].defined) {
        ins = emit_ir(IR_BR_BACK, w);
        ins->u.br.cond = cond;
        ins->u.br.target = labels[lidx].target_ir;
    } else {
        if (*ref_count >= MAX_LABEL_REFS) ir4_die("too many label references");
        refs[*ref_count].ir_idx = w->ir_len;
        refs[*ref_count].label_idx = lidx;
        (*ref_count)++;
        ins = emit_ir(IR_BR_FWD, w);
        ins->u.br.cond = cond;
    }
    fold_fence = w->ir_len;
}

static void compile_body(WordIR *w) {
    char tok[MAX_TOK];
    IrInstr *ins;
    u8 unreachable = 0;
    i32 num_val;
    LabelInfo labels[MAX_LABELS];
    LabelRef  label_refs[MAX_LABEL_REFS];
    u16 label_count = 0;
    u16 label_ref_count = 0;

    w->ir_start = ir_count;
    w->ir_len = 0;
    fold_fence = 0;

    /* Parse, type-check, and record IR for word body (Pass 1) */
    while (next_tok(tok)) {
        int idx;

        if (strcmp(tok, "end") == 0) {
            u16 r;
            if (bsp != 0) ir4_die("unclosed control block at 'end'");
            for (r = 0; r < label_ref_count; r++) {
                u16 lidx = label_refs[r].label_idx;
                if (!labels[lidx].defined) ir4_die("undefined label");
                ir_buf[w->ir_start + label_refs[r].ir_idx].u.br.target = labels[lidx].target_ir;
            }
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
            }
            return;
        }

        if (strcmp(tok, "return") == 0) {
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
                unreachable = 1;
            }
            continue;
        }

        /* Bare integer literal defaults to :i, bare float literal defaults to :f */
        if (parse_int(tok, &num_val)) {
            tpush(TY_W);
            ins = emit_ir(IR_PUSH_W, w);
            ins->u.w = num_val;
            continue;
        }
        {
            double fval;
            if (parse_float(tok, &fval)) {
                tpush(TY_F);
                ins = emit_ir(IR_PUSH_F, w);
                ins->u.f = fval;
                continue;
            }
        }

        /* Address of local or typed global: & <ident> */
        if (strcmp(tok, "&") == 0) {
            if (!next_tok(tok)) ir4_die("expected identifier after '&'");
            idx = find_local(tok);
            if (idx >= 0) {
                if (locals[idx].size > 0) ir4_die("':bytes' local is already an address");
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_ADDR;
                ins->u.loc.slot = locals[idx].slot;
                ins->u.loc.type = (StackType)locals[idx].type;
                ins->u.loc.stack_bytes = tstack_bytes();
                tpush(TY_A);
                continue;
            }
            idx = find_buffer(tok);
            if (idx < 0) ir4_die("unknown variable in '&'");
            if (buffers[idx].type == TY_NONE) ir4_die("':bytes' or 'data' symbol is already an address");
            ins = emit_ir(IR_PUSH_DATA, w);
            ins->u.offset = buffers[idx].offset;
            tpush(TY_A);
            continue;
        }

        /* Local or typed global assignment: -> <ident> */
        if (strcmp(tok, "->") == 0) {
            if (!next_tok(tok)) ir4_die("expected identifier after '->'");
            idx = find_local(tok);
            if (idx >= 0) {
                if (locals[idx].size > 0) ir4_die("cannot assign to ':bytes' local");
                tpop_expect(locals[idx].type);
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_SET;
                ins->u.loc.slot = locals[idx].slot;
                ins->u.loc.type = (StackType)locals[idx].type;
                ins->u.loc.stack_bytes = tstack_bytes();
                continue;
            }
            idx = find_buffer(tok);
            if (idx < 0) ir4_die("unknown variable in '->'");
            if (buffers[idx].type == TY_NONE) ir4_die("cannot assign to ':bytes' or 'data' symbol");
            tpop_expect(buffers[idx].type);
            ins = emit_ir(IR_PUSH_DATA, w);
            ins->u.offset = buffers[idx].offset;
            ins = emit_ir(IR_ALU_OP, w);
            ins->u.op = global_store_op(buffers[idx].type);
            continue;
        }

        /* Control flow */
        if (strcmp(tok, "if") == 0) {
            BlockInfo *b;
            if (bsp >= MAX_BLOCKS) ir4_die("block nesting overflow");
            tpop_expect(TY_W);
            b = &blocks[bsp++];
            b->kind = BLOCK_IF;
            b->has_else = 0;
            b->saved_unreach = unreachable;
            b->if_unreach = 0;
            b->saved_sp = tsp;
            memcpy(b->saved_stack, tstack, tsp);
            b->patch_else = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "else") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != BLOCK_IF) ir4_die("'else' without 'if'");
            b = &blocks[bsp - 1];
            b->has_else = 1;
            b->if_unreach = unreachable;
            b->else_sp = tsp;
            memcpy(b->else_stack, tstack, tsp);
            tsp = b->saved_sp;
            memcpy(tstack, b->saved_stack, tsp);
            unreachable = b->saved_unreach;
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JMP;
            ir_buf[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "then") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != BLOCK_IF) ir4_die("'then' without 'if'");
            b = &blocks[--bsp];
            if (!b->has_else) {
                if (!unreachable) {
                    if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                        ir4_die("unbalanced stack in 'if ... then'");
                }
                tsp = b->saved_sp;
                memcpy(tstack, b->saved_stack, tsp);
                unreachable = b->saved_unreach;
                ir_buf[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            } else {
                if (!b->if_unreach && !unreachable) {
                    if (tsp != b->else_sp || memcmp(tstack, b->else_stack, tsp) != 0)
                        ir4_die("branch stack mismatch between 'if' and 'else'");
                } else if (!b->if_unreach && unreachable) {
                    tsp = b->else_sp;
                    memcpy(tstack, b->else_stack, tsp);
                    unreachable = 0;
                }
                ir_buf[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            }
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            BlockInfo *b;
            if (bsp >= MAX_BLOCKS) ir4_die("block nesting overflow");
            b = &blocks[bsp++];
            b->kind = BLOCK_BEGIN;
            b->loop_addr = w->ir_len;
            fold_fence = w->ir_len;
            b->saved_sp = tsp;
            memcpy(b->saved_stack, tstack, tsp);
            continue;
        }
        if (strcmp(tok, "while") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != BLOCK_BEGIN) ir4_die("'while' without 'begin'");
            b = &blocks[bsp - 1];
            tpop_expect(TY_W);
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "repeat") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != BLOCK_BEGIN) ir4_die("'repeat' without 'begin'");
            b = &blocks[--bsp];
            if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                ir4_die("loop body must leave stack even at 'repeat'");
            ins = emit_ir(IR_BR_BACK, w);
            ins->u.br.cond = BR_JMP;
            ins->u.br.target = b->loop_addr;
            ir_buf[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "::") == 0) {
            int lidx;
            if (bsp != 0) ir4_die("'::' inside control block");
            if (!next_tok(tok)) ir4_die("expected label name after '::'");
            if (!unreachable && tsp != 0) ir4_die("stack must be empty at '::'");
            tsp = 0;
            unreachable = 0;
            lidx = find_or_add_label(labels, &label_count, tok);
            if (labels[lidx].defined) ir4_die("duplicate label");
            labels[lidx].defined = 1;
            labels[lidx].target_ir = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "jmp") == 0) {
            int lidx;
            if (!next_tok(tok)) ir4_die("expected label name after 'jmp'");
            if (!unreachable && tsp != 0) ir4_die("stack must be empty at 'jmp'");
            lidx = find_or_add_label(labels, &label_count, tok);
            if (!unreachable) {
                emit_label_br(w, labels, label_refs, &label_ref_count, (u16)lidx, BR_JMP);
                unreachable = 1;
            }
            continue;
        }
        if (strcmp(tok, "jz") == 0) {
            int lidx;
            if (!next_tok(tok)) ir4_die("expected label name after 'jz'");
            tpop_expect(TY_W);
            if (!unreachable && tsp != 0) ir4_die("stack must be empty after condition at 'jz'");
            lidx = find_or_add_label(labels, &label_count, tok);
            if (!unreachable) {
                emit_label_br(w, labels, label_refs, &label_ref_count, (u16)lidx, BR_JZ);
            }
            continue;
        }

        /* Core built-in word */
        if (handle_builtin(tok, w)) continue;

        /* Built-in heap start address (:a) */
        if (strcmp(tok, "__heap_start") == 0) {
            emit_ir(IR_PUSH_HEAP_START, w);
            tpush(TY_A);
            continue;
        }

        /* Local variable read (or ':bytes' local base address) */
        idx = find_local(tok);
        if (idx >= 0) {
            ins = emit_ir(IR_LOCAL_OP, w);
            ins->u.loc.op = (locals[idx].size > 0) ? LOCAL_ADDR : LOCAL_GET;
            ins->u.loc.slot = locals[idx].slot;
            ins->u.loc.type = (StackType)locals[idx].type;
            ins->u.loc.stack_bytes = tstack_bytes();
            tpush((locals[idx].size > 0) ? TY_A : locals[idx].type);
            continue;
        }

        /* Buffer address or typed global read */
        idx = find_buffer(tok);
        if (idx >= 0) {
            if (buffers[idx].type == TY_NONE) {
                tpush(TY_A);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = buffers[idx].offset;
            } else {
                tpush(buffers[idx].type);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = buffers[idx].offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_fetch_op(buffers[idx].type);
            }
            continue;
        }

        /* User word call */
        idx = ir4_find_func(tok);
        if (idx >= 0) {
            FuncSig *callee = &funcs[idx];
            u8 i;
            for (i = callee->in_count; i > 0; i--) {
                tpop_expect(callee->in_types[i - 1]);
            }
            for (i = 0; i < callee->out_count; i++) {
                tpush(callee->out_types[i]);
            }
            if (callee->is_extern) {
                ins = emit_ir(IR_CALL_EXTERN, w);
            } else {
                ins = emit_ir(IR_CALL, w);
            }
            ins->u.func_idx = (u8)idx;
            continue;
        }

        {
            const SrcFile *cur = &src_stack[src_depth - 1];
            fprintf(stderr, "ir4c error (%s:%u): unknown word '%s'\n",
                    cur->path, cur->line_num, tok);
        }
        exit(1);
    }

    ir4_die("unexpected EOF inside word definition (missing ';')");
}

static u8 can_reach_func(u8 cur, u8 target, u8 *visited) {
    WordIR *w = &words[cur];
    u16 i;
    if (funcs[cur].is_extern || w->is_code) return 0;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            u8 callee = ins->u.func_idx;
            if (callee == target) return 1;
            if (!visited[callee]) {
                visited[callee] = 1;
                if (can_reach_func(callee, target, visited)) return 1;
            }
        }
    }
    return 0;
}

static void mark_reachable(u8 fidx) {
    WordIR *w = &words[fidx];
    u16 i;
    if (funcs[fidx].is_extern) return;
    if (!funcs[fidx].is_defined) ir4_die("called 'fwd' function was never defined");
    if (w->reachable) return;
    w->reachable = 1;
    if (w->is_code) return;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            mark_reachable(ins->u.func_idx);
        }
    }
}

typedef struct {
    u16 patch_offset;
    u16 target_ir;
} PendingBr;

#define MAX_FWD_CALLS 1024
typedef struct {
    u16 call_pc;
    u16 call_addr;
    u8  func_idx;
} FwdCall;

static FwdCall fwd_calls[MAX_FWD_CALLS];
static u16     fwd_call_count = 0;
static u16     heap_start_off = 0;

static void emit_word(u8 fidx) {
    static u16 ir_addr[MAX_IR];
    FuncSig *sig = &funcs[fidx];
    WordIR *w = &words[fidx];
    PendingBr pending[MAX_BLOCKS * 4];
    u16 pend_cnt = 0;
    u16 i, p;

    if (w->is_code) {
        sig->addr = be->cur_addr();
        sig->has_addr = 1;
        for (i = 0; i < w->ir_len; i++) {
            emit8(raw_code_buf[w->ir_start + i]);
        }
        return;
    }

    if (w->ir_len >= MAX_IR) ir4_die("word IR length overflow");
    sig->addr = be->cur_addr();
    sig->has_addr = 1;
    be->fn_begin(sig->name, w->param_cnt, w->locals, w->local_count, sig->is_recursive);

    for (i = 0; i <= w->ir_len; i++) {
        for (p = 0; p < pend_cnt; p++) {
            if (pending[p].target_ir == i) {
                be->patch_fwd(pending[p].patch_offset);
            }
        }
        if (i == w->ir_len) break;
        ir_addr[i] = be->cur_addr();
        {
            const IrInstr *ins = &ir_buf[w->ir_start + i];
            switch (ins->kind) {
            case IR_FN_END:
                be->fn_end(sig, w->locals, w->local_count);
                break;
            case IR_PUSH_W:
                if (i + 2 < w->ir_len &&
                    ir_buf[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    ir_buf[w->ir_start + i + 2].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf[w->ir_start + i + 1].u.op;
                    IrOp mop = ir_buf[w->ir_start + i + 2].u.op;
                    if ((aop == OP_AADD || aop == OP_ASUB) &&
                        mop >= OP_I8FETCH && mop <= OP_FSTORE && be->mem_imm) {
                        i32 off = (aop == OP_ASUB)
                                  ? (i32)(0UL - (u32)ins->u.w) : ins->u.w;
                        if (be->mem_imm(mop, off)) {
                            i += 2;
                            break;
                        }
                    }
                }
                if (i + 1 < w->ir_len && ir_buf[w->ir_start + i + 1].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf[w->ir_start + i + 1].u.op;
                    i32 imm = ins->u.w;
                    if (aop == OP_ADD || aop == OP_SUB || aop == OP_AADD || aop == OP_ASUB) {
                        i32 delta = (aop == OP_SUB || aop == OP_ASUB)
                                    ? (i32)(0UL - (u32)imm) : imm;
                        if (be->add_imm && be->add_imm(delta)) {
                            i++;
                            break;
                        }
                    } else if (aop >= OP_EQ && aop <= OP_UGE) {
                        if (be->cmp_imm && be->cmp_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    }
                }
                be->push_w(ins->u.w);
                break;
            case IR_PUSH_A:
                be->push_a(ins->u.a);
                break;
            case IR_PUSH_F:
                be->push_f(ins->u.f);
                break;
            case IR_PUSH_DATA:
                if (i + 1 < w->ir_len &&
                    ir_buf[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    be->data_mem &&
                    be->data_mem(ir_buf[w->ir_start + i + 1].u.op, ins->u.offset)) {
                    i++;
                    break;
                }
                be->push_data(ins->u.offset);
                break;
            case IR_PUSH_HEAP_START:
                be->push_data(heap_start_off);
                break;
            case IR_LOCAL_OP:
                if (ins->u.loc.op == LOCAL_ADDR && be->local_mem) {
                    if (i + 3 < w->ir_len &&
                        ir_buf[w->ir_start + i + 1].kind == IR_PUSH_W &&
                        ir_buf[w->ir_start + i + 2].kind == IR_ALU_OP &&
                        ir_buf[w->ir_start + i + 3].kind == IR_ALU_OP) {
                        IrOp aop = ir_buf[w->ir_start + i + 2].u.op;
                        IrOp mop = ir_buf[w->ir_start + i + 3].u.op;
                        if ((aop == OP_AADD || aop == OP_ASUB) &&
                            mop >= OP_I8FETCH && mop <= OP_FSTORE) {
                            i32 imm = ir_buf[w->ir_start + i + 1].u.w;
                            i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                            if (be->local_mem(ins->u.loc.slot, off, mop, ins->u.loc.stack_bytes)) {
                                i += 3;
                                break;
                            }
                        }
                    } else if (i + 1 < w->ir_len &&
                               ir_buf[w->ir_start + i + 1].kind == IR_ALU_OP) {
                        IrOp mop = ir_buf[w->ir_start + i + 1].u.op;
                        if (mop >= OP_I8FETCH && mop <= OP_FSTORE &&
                            be->local_mem(ins->u.loc.slot, 0, mop, ins->u.loc.stack_bytes)) {
                            i += 1;
                            break;
                        }
                    }
                }
                be->local_op(ins->u.loc.op, ins->u.loc.slot,
                             ins->u.loc.type, ins->u.loc.stack_bytes);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                be->op(ins->u.op);
                break;
            case IR_CALL: {
                u8 target = ins->u.func_idx;
                u16 call_addr = be->cur_addr();
                u16 call_pc = cb_pc();
                be->call(funcs[target].addr);
                if (!funcs[target].has_addr) {
                    if (fwd_call_count >= MAX_FWD_CALLS) ir4_die("too many forward calls");
                    fwd_calls[fwd_call_count].call_pc = call_pc;
                    fwd_calls[fwd_call_count].call_addr = call_addr;
                    fwd_calls[fwd_call_count].func_idx = target;
                    fwd_call_count++;
                }
                break;
            }
            case IR_CALL_EXTERN:
                be->call_extern(&funcs[ins->u.func_idx]);
                break;
            case IR_BR_FWD:
                if (pend_cnt >= MAX_BLOCKS * 4) ir4_die("too many pending branches");
                pending[pend_cnt].patch_offset = be->emit_br_fwd(ins->u.br.cond);
                pending[pend_cnt].target_ir = ins->u.br.target;
                pend_cnt++;
                break;
            case IR_BR_BACK:
                be->emit_br_back(ins->u.br.cond, ir_addr[ins->u.br.target]);
                break;
            }
        }
    }
}

static void flush_vals_word(i32 val) {
    if (be->word_bytes == 2 && (val < -32768 || val > 65535)) {
        ir4_die(":i value out of range");
    }
    cb_emit_data_word(val, be->word_bytes);
}

static void emit_vals_string(void) {
    SrcFile *cur = &src_stack[src_depth - 1];
    for (;;) {
        int c = fgetc(cur->fp);
        if (c == EOF || c == '\n') ir4_die("unterminated string literal in 'data'");
        if (c == '"') return;
        if (c == '\\') {
            c = fgetc(cur->fp);
            if (c == EOF || c == '\n') ir4_die("unterminated escape in string literal");
            switch (c) {
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case '0':  c = '\0'; break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            default: ir4_die("unknown escape sequence in string literal");
            }
        }
        cb_emit_data_u8((u8)c);
    }
}

static void compile_data(void) {
    char tok[MAX_TOK];
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;
    int ch;

    if (buffer_count >= MAX_SYMS) ir4_die("too many buffers");
    if (!next_tok(buffers[buffer_count].name)) {
        ir4_die("expected name after 'data'");
    }
    if (find_buffer(buffers[buffer_count].name) >= 0) {
        ir4_die("duplicate global/data name");
    }
    buffers[buffer_count].offset = cb_alloc_data(0, be->word_bytes);
    buffers[buffer_count].type = TY_NONE;
    buffer_count++;

    while ((ch = skip_ws_and_comments()) != EOF) {
        if (ch == '"') {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            emit_vals_string();
            continue;
        }
        ungetc(ch, src_stack[src_depth - 1].fp);
        if (!next_tok(tok)) break;
        if (strcmp(tok, "end") == 0) {
            if (has_pending) flush_vals_word(pending);
            return;
        } else if (strcmp(tok, "align") == 0) {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            cb_alloc_data(0, be->word_bytes);
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending) ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255) ir4_die(":i8 value out of range");
            cb_emit_data_u8((u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            if (!has_pending) ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535) ir4_die(":i16 value out of range");
            cb_emit_data_u16((u16)(pending & 0xFFFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i") == 0) {
            if (!has_pending) ir4_die("expected integer before ':i'");
            flush_vals_word(pending);
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending) flush_vals_word(pending);
            pending = num_val;
            has_pending = 1;
        } else {
            ir4_die("unexpected token in 'data'");
        }
    }
    ir4_die("unexpected EOF in 'data' (missing 'end')");
}

void ir4_compile_stream(FILE *in, const char *path, const Backend *target_be,
                        const char *platform) {
    char tok[MAX_TOK];
    i32 num_val;
    const char *root_path = (path != NULL) ? path : "<stdin>";
    size_t plen = strlen(root_path);

    if (plen >= MAX_PATH) ir4_die("input file path too long");
    src_depth = 1;
    src_stack[0].fp = in;
    memcpy(src_stack[0].path, root_path, plen + 1);
    src_stack[0].line_num = 1;

    seen_count = 1;
    memcpy(seen_files[0], root_path, plen + 1);

    be = target_be;
    platform_name = platform;
    func_count = 0;
    buffer_count = 0;
    ir_count = 0;
    raw_code_len = 0;
    fwd_call_count = 0;
    init_count = 0;

    while (next_tok(tok)) {
        if (strcmp(tok, "fn") == 0) {
            compile_word();
        } else if (strcmp(tok, "code") == 0) {
            compile_code();
        } else if (strcmp(tok, "init") == 0) {
            compile_init();
        } else if (strcmp(tok, "fwd") == 0) {
            FuncSig *sig;
            char name[MAX_TOK];
            if (!next_tok(name)) ir4_die("expected word name after 'fwd'");
            if (ir4_find_func(name) >= 0) ir4_die("duplicate 'fwd' declaration");
            if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
            sig = &funcs[func_count++];
            memset(sig, 0, sizeof(FuncSig));
            memcpy(sig->name, name, strlen(name) + 1);
            if (!next_tok(tok) || strcmp(tok, "(") != 0) ir4_die("expected '(' after 'fwd' name");
            parse_sig_body(sig, NULL, 0);
        } else if (strcmp(tok, "extern") == 0) {
            FuncSig *sig;
            if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
            sig = &funcs[func_count++];
            memset(sig, 0, sizeof(FuncSig));
            sig->is_extern = 1;
            sig->is_defined = 1;
            if (!next_tok(sig->name)) ir4_die("expected word name after 'extern'");
            if (!next_tok(tok)) ir4_die("expected address or '(' after extern name");
            if (strcmp(tok, "(") == 0) {
                sig->has_addr = 0;
            } else if (parse_int(tok, &num_val)) {
                if (num_val < 0 || num_val > 0xFFFF) ir4_die("invalid extern address");
                sig->addr = (u16)num_val;
                sig->has_addr = 1;
                if (!next_tok(tok) || strcmp(tok, "(") != 0) {
                    ir4_die("expected '(' after extern address");
                }
            } else {
                ir4_die("expected address or '(' after extern name");
            }
            parse_sig_body(sig, NULL, 0);
            if (sig->out_count > 1) ir4_die("extern supports at most 1 return value");
        } else if (strcmp(tok, "require") == 0) {
            if (!next_tok(tok)) ir4_die("expected filename after 'require'");
            push_require(tok);
        } else if (strcmp(tok, "data") == 0) {
            compile_data();
        } else if (strcmp(tok, "var") == 0) {
            u8 ty;
            if (buffer_count >= MAX_SYMS) ir4_die("too many buffers");
            if (!next_tok(buffers[buffer_count].name)) {
                ir4_die("expected var name after 'var'");
            }
            if (find_buffer(buffers[buffer_count].name) >= 0) {
                ir4_die("duplicate global/data name");
            }
            if (!next_tok(tok)) {
                ir4_die("expected type (:i, :a, :f) or ':bytes' after var name");
            }
            if (strcmp(tok, ":bytes") == 0) {
                if (!next_tok(tok) || !parse_int(tok, &num_val) || num_val <= 0 || num_val > MAX_DATA) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                buffers[buffer_count].offset = cb_alloc_data((u16)num_val, be->word_bytes);
                buffers[buffer_count].type = TY_NONE;
            } else {
                u16 sz;
                ty = parse_type_tok(tok);
                if (ty == TY_NONE) {
                    ir4_die("expected ':i', ':a', ':f', or ':bytes' after var name");
                }
                sz = global_type_bytes(ty);
                buffers[buffer_count].offset = cb_alloc_data(sz, sz);
                buffers[buffer_count].type = ty;
            }
            buffer_count++;
        } else {
            ir4_die("expected top-level 'fn', 'var', 'data', 'extern', or 'require'");
        }
    }

    /* Pass 1.5: Reachability / Dead-Code Elimination from 'main' */
    {
        int main_idx = ir4_find_func("main");
        u8 f;
        if (main_idx >= 0 && init_count > 0) {
            u8 wrap_idx;
            FuncSig *wsig;
            WordIR *ww;
            IrInstr *ins;
            u8 k;
            if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
            strcpy(funcs[main_idx].name, "__main_body");
            wrap_idx = func_count++;
            wsig = &funcs[wrap_idx];
            ww = &words[wrap_idx];
            memset(wsig, 0, sizeof(FuncSig));
            memset(ww, 0, sizeof(WordIR));
            strcpy(wsig->name, "main");
            wsig->is_defined = 1;
            wsig->out_count = funcs[main_idx].out_count;
            memcpy(wsig->out_types, funcs[main_idx].out_types, wsig->out_count);
            ww->ir_start = ir_count;
            for (k = 0; k < init_count; k++) {
                ins = emit_ir(IR_CALL, ww);
                ins->u.func_idx = init_funcs[k];
            }
            ins = emit_ir(IR_CALL, ww);
            ins->u.func_idx = (u8)main_idx;
            emit_ir(IR_FN_END, ww);
            main_idx = wrap_idx;
        }
        if (main_idx >= 0) {
            mark_reachable((u8)main_idx);
        } else {
            for (f = 0; f < func_count; f++) {
                if (!funcs[f].is_extern && funcs[f].is_defined) words[f].reachable = 1;
            }
        }
        for (f = 0; f < func_count; f++) {
            if (words[f].reachable) {
                u8 visited[MAX_SYMS];
                memset(visited, 0, sizeof(visited));
                funcs[f].is_recursive = can_reach_func(f, f, visited);
            }
        }

        if (be == &be_6502) (void)be_6502_stack_data_off();
        heap_start_off = cb_alloc_data(0, be->word_bytes);

        /* Pass 2: Emit reachable words and patch forward calls */
        for (f = 0; f < func_count; f++) {
            if (!funcs[f].is_extern && words[f].reachable) {
                emit_word(f);
            }
        }
        {
            u16 c;
            for (c = 0; c < fwd_call_count; c++) {
                be->patch_call(fwd_calls[c].call_pc, fwd_calls[c].call_addr,
                               funcs[fwd_calls[c].func_idx].addr);
            }
        }
    }
}

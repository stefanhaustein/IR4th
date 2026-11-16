#include "codebuf.h"
#include "frontend.h"

#define MAX_INCLUDE_DEPTH 16
#define MAX_INCLUDES      64
#define MAX_PATH         256
#define MAX_IR          8192
#define MAX_SYMS          48
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
} WordIR;

static IrInstr ir_buf[MAX_IR];
static u16     ir_count = 0;
static WordIR  words[MAX_SYMS];

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
    if (t == TY_W) return "W:";
    if (t == TY_A) return "A:";
    if (t == TY_F) return "F:";
    return "?";
}

static u8 parse_type_tok(const char *tok) {
    if (strcmp(tok, "W:") == 0) return TY_W;
    if (strcmp(tok, "A:") == 0) return TY_A;
    if (strcmp(tok, "F:") == 0) return TY_F;
    return TY_NONE;
}

static int next_tok(char *buf) {
    int c;
    u8 len = 0;

    for (;;) {
        SrcFile *cur;
        if (src_depth == 0) return 0;
        cur = &src_stack[src_depth - 1];
        c = fgetc(cur->fp);
        if (c == EOF) {
            if (src_depth > 1) {
                fclose(cur->fp);
                src_depth--;
                continue;
            }
            return 0;
        }
        if (c == '\n') { cur->line_num++; continue; }
        if (isspace((unsigned char)c)) continue;
        if (c == '\\') {
            while ((c = fgetc(cur->fp)) != EOF && c != '\n') ;
            if (c == '\n') cur->line_num++;
            continue;
        }
        break;
    }

    {
        SrcFile *cur = &src_stack[src_depth - 1];
        while (c != EOF && !isspace((unsigned char)c)) {
            if (len < MAX_TOK - 1) buf[len++] = (char)c;
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

static void push_require(const char *name) {
    char cands[3][MAX_PATH];
    int n_cands = 0;
    int c;
    char path[MAX_PATH];
    u8 i;
    FILE *fp = NULL;

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
        for (i = 0; i < seen_count; i++) {
            if (strcmp(seen_files[i], path) == 0) {
                return;
            }
        }
        fp = fopen(path, "r");
        if (fp) break;
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
    /* Word ALU & bitwise */
    { "+",       OP_ADD,      TY_W, TY_W,    TY_W },
    { "-",       OP_SUB,      TY_W, TY_W,    TY_W },
    { "*",       OP_MUL,      TY_W, TY_W,    TY_W },
    { "/",       OP_DIV,      TY_W, TY_W,    TY_W },
    { "mod",     OP_MOD,      TY_W, TY_W,    TY_W },
    { "u/",      OP_UDIV,     TY_W, TY_W,    TY_W },
    { "umod",    OP_UMOD,     TY_W, TY_W,    TY_W },
    { "and",     OP_AND,      TY_W, TY_W,    TY_W },
    { "or",      OP_OR,       TY_W, TY_W,    TY_W },
    { "xor",     OP_XOR,      TY_W, TY_W,    TY_W },
    { "invert",  OP_INVERT,   TY_W, TY_NONE, TY_W },
    { "lshift",  OP_LSHIFT,   TY_W, TY_W,    TY_W },
    { "arshift", OP_ARSHIFT,  TY_W, TY_W,    TY_W },
    { "rshift",  OP_RSHIFT,   TY_W, TY_W,    TY_W },

    /* Address arithmetic */
    { "a+",      OP_AADD,     TY_W, TY_A,    TY_A },
    { "a-",      OP_ASUB,     TY_W, TY_A,    TY_A },
    { "adiff",   OP_ADIFF,    TY_A, TY_A,    TY_W },

    /* Floating-point operations */
    { "f+",      OP_FADD,     TY_F, TY_F,    TY_F },
    { "f-",      OP_FSUB,     TY_F, TY_F,    TY_F },
    { "f*",      OP_FMUL,     TY_F, TY_F,    TY_F },
    { "f/",      OP_FDIV,     TY_F, TY_F,    TY_F },
    { "fnegate", OP_FNEG,     TY_F, TY_NONE, TY_F },

    /* Comparisons */
    { "=",       OP_EQ,       TY_W, TY_W,    TY_W },
    { "<>",      OP_NE,       TY_W, TY_W,    TY_W },
    { "<",       OP_LT,       TY_W, TY_W,    TY_W },
    { ">",       OP_GT,       TY_W, TY_W,    TY_W },
    { "<=",      OP_LE,       TY_W, TY_W,    TY_W },
    { ">=",      OP_GE,       TY_W, TY_W,    TY_W },
    { "u<",      OP_ULT,      TY_W, TY_W,    TY_W },
    { "u>",      OP_UGT,      TY_W, TY_W,    TY_W },
    { "u<=",     OP_ULE,      TY_W, TY_W,    TY_W },
    { "u>=",     OP_UGE,      TY_W, TY_W,    TY_W },
    { "a=",      OP_AEQ,      TY_A, TY_A,    TY_W },
    { "a<>",     OP_ANE,      TY_A, TY_A,    TY_W },
    { "au<",     OP_ALT,      TY_A, TY_A,    TY_W },
    { "au>",     OP_AGT,      TY_A, TY_A,    TY_W },
    { "f=",      OP_FEQ,      TY_F, TY_F,    TY_W },
    { "f<>",     OP_FNE,      TY_F, TY_F,    TY_W },
    { "f<",      OP_FLT,      TY_F, TY_F,    TY_W },
    { "f>",      OP_FGT,      TY_F, TY_F,    TY_W },

    /* Memory load/store */
    { "i8@",     OP_I8FETCH,  TY_A, TY_NONE, TY_W },
    { "u8@",     OP_U8FETCH,  TY_A, TY_NONE, TY_W },
    { "i8!",     OP_I8STORE,  TY_A, TY_W,    TY_NONE },
    { "i16@",    OP_I16FETCH, TY_A, TY_NONE, TY_W },
    { "u16@",    OP_U16FETCH, TY_A, TY_NONE, TY_W },
    { "i16!",    OP_I16STORE, TY_A, TY_W,    TY_NONE },
    { "@",       OP_FETCH,    TY_A, TY_NONE, TY_W },
    { "!",       OP_STORE,    TY_A, TY_W,    TY_NONE },
    { "a@",      OP_AFETCH,   TY_A, TY_NONE, TY_A },
    { "a!",      OP_ASTORE,   TY_A, TY_A,    TY_NONE },
    { "f@",      OP_FFETCH,   TY_A, TY_NONE, TY_F },
    { "f!",      OP_FSTORE,   TY_A, TY_F,    TY_NONE },

    /* Type conversions */
    { "sext",    OP_SEXT,     TY_W, TY_NONE, TY_W },
    { "w>a",     OP_W2A,      TY_W, TY_NONE, TY_A },
    { "a>w",     OP_A2W,      TY_A, TY_NONE, TY_W },
    { "w>f",     OP_W2F,      TY_W, TY_NONE, TY_F },
    { "f>w",     OP_F2W,      TY_F, TY_NONE, TY_W }
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

    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(tok, builtins[i].name) == 0) {
            tpop_expect(builtins[i].in1);
            if (builtins[i].in2 != TY_NONE) tpop_expect(builtins[i].in2);
            if (builtins[i].out != TY_NONE) tpush(builtins[i].out);
            ins = emit_ir(IR_ALU_OP, w);
            ins->u.op = builtins[i].op;
            return 1;
        }
    }

    return 0;
}

static void parse_sig_body(FuncSig *sig, u8 *param_cnt_out, int allow_locals) {
    char tok[MAX_TOK];
    u8 param_cnt = 0;
    u8 in_locals = 0;
    u8 in_outs = 0;

    local_count = 0;
    cur_out_count = 0;

    /* Parse inputs | locals -- outputs } (after '{' has been consumed) */
    while (next_tok(tok)) {
        u8 ty;
        if (strcmp(tok, "}") == 0) {
            if (param_cnt_out) *param_cnt_out = param_cnt;
            return;
        }
        if (strcmp(tok, "|") == 0) {
            if (!allow_locals) ir4_die("locals '|' not allowed in extern signature");
            in_locals = 1;
            continue;
        }
        if (strcmp(tok, "--") == 0) { in_outs = 1; continue; }

        ty = parse_type_tok(tok);
        if (ty == TY_NONE) {
            if (!allow_locals && in_outs && cur_out_count == 1 && sig->out_reg[0] == '\0') {
                size_t len = strlen(tok);
                memcpy(sig->out_reg, tok, len + 1);
                continue;
            }
            ir4_die("expected type token (W:, A:, F:) in signature");
        }

        if (in_outs) {
            if (cur_out_count >= 4) ir4_die("too many return types");
            sig->out_types[sig->out_count++] = ty;
            cur_outs[cur_out_count++] = ty;
        } else {
            if (local_count >= MAX_LOCALS) ir4_die("too many parameters/locals");
            if (!next_tok(locals[local_count].name) ||
                strcmp(locals[local_count].name, "|") == 0 ||
                strcmp(locals[local_count].name, "--") == 0 ||
                strcmp(locals[local_count].name, "}") == 0 ||
                parse_type_tok(locals[local_count].name) != TY_NONE) {
                ir4_die("expected identifier after type in signature");
            }
            locals[local_count].type = ty;
            locals[local_count].slot = local_count;
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

    ir4_die("unexpected EOF in signature (missing '}')");
}

static void compile_word(void) {
    char tok[MAX_TOK];
    FuncSig *sig;
    WordIR *w;
    IrInstr *ins;
    u8 fidx;
    u8 param_cnt = 0;
    u8 unreachable = 0;
    i32 num_val;

    if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
    fidx = func_count++;
    sig = &funcs[fidx];
    w = &words[fidx];
    memset(sig, 0, sizeof(FuncSig));
    memset(w, 0, sizeof(WordIR));

    if (!next_tok(sig->name)) ir4_die("expected word name after ':'");
    if (!next_tok(tok) || strcmp(tok, "{") != 0) ir4_die("expected '{' after word name");
    tsp = 0;
    bsp = 0;
    parse_sig_body(sig, &param_cnt, 1);

    w->ir_start = ir_count;
    w->ir_len = 0;
    w->param_cnt = param_cnt;
    w->local_count = local_count;
    if (local_count > 0) {
        memcpy(w->locals, locals, local_count * sizeof(LocalVar));
    }

    /* Parse, type-check, and record IR for word body (Pass 1) */
    while (next_tok(tok)) {
        u8 ty;
        int idx;

        if (strcmp(tok, ";") == 0) {
            if (bsp != 0) ir4_die("unclosed control block at ';'");
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
            }
            return;
        }

        if (strcmp(tok, "exit") == 0) {
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
                unreachable = 1;
            }
            continue;
        }

        /* Prefixed typed literal: A: 0x0400, F: 3.14 */
        ty = parse_type_tok(tok);
        if (ty != TY_NONE) {
            if (!next_tok(tok)) ir4_die("expected literal after type prefix");
            if (ty == TY_A) {
                if (!parse_int(tok, &num_val)) ir4_die("expected integer address after A:");
                tpush(TY_A);
                ins = emit_ir(IR_PUSH_A, w);
                ins->u.a = (u32)num_val;
            } else if (ty == TY_W) {
                if (!parse_int(tok, &num_val)) ir4_die("expected integer after W:");
                tpush(TY_W);
                ins = emit_ir(IR_PUSH_W, w);
                ins->u.w = num_val;
            } else {
                char *endp;
                double fval;
                if (tok[0] == '\0') ir4_die("expected float literal after F:");
                fval = strtod(tok, &endp);
                if (*endp != '\0') ir4_die("expected float literal after F:");
                tpush(TY_F);
                ins = emit_ir(IR_PUSH_F, w);
                ins->u.f = fval;
            }
            continue;
        }

        /* Bare integer literal defaults to W: */
        if (parse_int(tok, &num_val)) {
            tpush(TY_W);
            ins = emit_ir(IR_PUSH_W, w);
            ins->u.w = num_val;
            continue;
        }

        /* Local assignment: to <ident> */
        if (strcmp(tok, "to") == 0) {
            if (!next_tok(tok)) ir4_die("expected identifier after 'to'");
            idx = find_local(tok);
            if (idx < 0) ir4_die("unknown local in 'to'");
            tpop_expect(locals[idx].type);
            ins = emit_ir(IR_LOCAL_OP, w);
            ins->u.loc.op = LOCAL_SET;
            ins->u.loc.slot = locals[idx].slot;
            ins->u.loc.type = (StackType)locals[idx].type;
            ins->u.loc.stack_bytes = tstack_bytes();
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
            }
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            BlockInfo *b;
            if (bsp >= MAX_BLOCKS) ir4_die("block nesting overflow");
            b = &blocks[bsp++];
            b->kind = BLOCK_BEGIN;
            b->loop_addr = w->ir_len;
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
            continue;
        }
        if (strcmp(tok, "until") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != BLOCK_BEGIN) ir4_die("'until' without 'begin'");
            b = &blocks[--bsp];
            tpop_expect(TY_W);
            if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                ir4_die("loop body must leave stack even at 'until'");
            ins = emit_ir(IR_BR_BACK, w);
            ins->u.br.cond = BR_JZ;
            ins->u.br.target = b->loop_addr;
            continue;
        }

        /* Core built-in word */
        if (handle_builtin(tok, w)) continue;

        /* Local variable read */
        idx = find_local(tok);
        if (idx >= 0) {
            ins = emit_ir(IR_LOCAL_OP, w);
            ins->u.loc.op = LOCAL_GET;
            ins->u.loc.slot = locals[idx].slot;
            ins->u.loc.type = (StackType)locals[idx].type;
            ins->u.loc.stack_bytes = tstack_bytes();
            tpush(locals[idx].type);
            continue;
        }

        /* Buffer address */
        idx = find_buffer(tok);
        if (idx >= 0) {
            tpush(TY_A);
            ins = emit_ir(IR_PUSH_DATA, w);
            ins->u.offset = buffers[idx].offset;
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

static void mark_reachable(u8 fidx) {
    WordIR *w = &words[fidx];
    u16 i;
    if (funcs[fidx].is_extern || w->reachable) return;
    w->reachable = 1;
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

static void emit_word(u8 fidx) {
    static u16 ir_addr[MAX_IR];
    FuncSig *sig = &funcs[fidx];
    WordIR *w = &words[fidx];
    PendingBr pending[MAX_BLOCKS * 4];
    u16 pend_cnt = 0;
    u16 i, p;

    if (w->ir_len >= MAX_IR) ir4_die("word IR length overflow");
    sig->addr = be->cur_addr();
    be->fn_begin(sig->name, w->param_cnt, w->locals, w->local_count);

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
                be->push_w(ins->u.w);
                break;
            case IR_PUSH_A:
                be->push_a(ins->u.a);
                break;
            case IR_PUSH_F:
                be->push_f(ins->u.f);
                break;
            case IR_PUSH_DATA:
                be->push_data(ins->u.offset);
                break;
            case IR_LOCAL_OP:
                be->local_op(ins->u.loc.op, ins->u.loc.slot,
                             ins->u.loc.type, ins->u.loc.stack_bytes);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                be->op(ins->u.op);
                break;
            case IR_CALL:
                be->call(funcs[ins->u.func_idx].addr);
                break;
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

void ir4_compile_stream(FILE *in, const char *path, const Backend *target_be,
                        const char *platform) {
    char tok[MAX_TOK];
    i32 num_val;
    i32 ct_stack[MAX_STACK];
    int ct_sp = 0;
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

    while (next_tok(tok)) {
        if (strcmp(tok, ":") == 0) {
            if (ct_sp != 0) ir4_die("unconsumed integer at top level");
            compile_word();
        } else if (strcmp(tok, "extern") == 0) {
            FuncSig *sig;
            if (ct_sp != 0) ir4_die("unconsumed integer at top level");
            if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
            sig = &funcs[func_count++];
            memset(sig, 0, sizeof(FuncSig));
            sig->is_extern = 1;
            if (!next_tok(sig->name)) ir4_die("expected word name after 'extern'");
            if (!next_tok(tok)) ir4_die("expected address or '{' after extern name");
            if (strcmp(tok, "{") == 0) {
                sig->has_addr = 0;
            } else if (parse_int(tok, &num_val)) {
                if (num_val < 0 || num_val > 0xFFFF) ir4_die("invalid extern address");
                sig->addr = (u16)num_val;
                sig->has_addr = 1;
                if (!next_tok(tok) || strcmp(tok, "{") != 0) {
                    ir4_die("expected '{' after extern address");
                }
            } else {
                ir4_die("expected address or '{' after extern name");
            }
            parse_sig_body(sig, NULL, 0);
            if (sig->out_count > 1) ir4_die("extern supports at most 1 return value");
        } else if (strcmp(tok, "require") == 0) {
            if (ct_sp != 0) ir4_die("unconsumed integer at top level");
            if (!next_tok(tok)) ir4_die("expected filename after 'require'");
            push_require(tok);
        } else if (strcmp(tok, "create") == 0) {
            if (ct_sp != 0) ir4_die("unconsumed integer at top level");
            if (buffer_count >= MAX_SYMS) ir4_die("too many buffers");
            if (!next_tok(buffers[buffer_count].name)) {
                ir4_die("expected name after 'create'");
            }
            buffers[buffer_count].offset = cb_alloc_data(0, be->word_bytes);
            buffer_count++;
        } else if (strcmp(tok, "align") == 0) {
            if (ct_sp != 0) ir4_die("unconsumed integer at top level");
            cb_alloc_data(0, be->word_bytes);
        } else if (strcmp(tok, "buffer:") == 0) {
            if (ct_sp < 1) ir4_die("expected size before 'buffer:'");
            num_val = ct_stack[--ct_sp];
            if (num_val <= 0 || num_val > MAX_DATA) {
                ir4_die("invalid buffer size");
            }
            if (buffer_count >= MAX_SYMS) {
                ir4_die("too many buffers");
            }
            if (!next_tok(buffers[buffer_count].name)) {
                ir4_die("expected buffer name after 'buffer:'");
            }
            buffers[buffer_count].offset = cb_alloc_data((u16)num_val, be->word_bytes);
            buffer_count++;
        } else if (strcmp(tok, "i8,") == 0) {
            if (ct_sp < 1) ir4_die("expected integer before 'i8,'");
            num_val = ct_stack[--ct_sp];
            if (num_val < -128 || num_val > 255) ir4_die("i8, value out of range");
            cb_emit_data_u8((u8)(num_val & 0xFF));
        } else if (strcmp(tok, "i16,") == 0) {
            if (ct_sp < 1) ir4_die("expected integer before 'i16,'");
            num_val = ct_stack[--ct_sp];
            if (num_val < -32768 || num_val > 65535) ir4_die("i16, value out of range");
            cb_emit_data_u16((u16)(num_val & 0xFFFF));
        } else if (strcmp(tok, ",") == 0) {
            if (ct_sp < 1) ir4_die("expected integer before ','");
            num_val = ct_stack[--ct_sp];
            if (be->word_bytes == 2 && (num_val < -32768 || num_val > 65535)) {
                ir4_die(", value out of range");
            }
            cb_emit_data_word(num_val, be->word_bytes);
        } else if (parse_int(tok, &num_val)) {
            if (ct_sp >= MAX_STACK) ir4_die("top-level stack overflow");
            ct_stack[ct_sp++] = num_val;
        } else {
            ir4_die("expected top-level ':', 'require', 'create', 'buffer:', 'align', or data");
        }
    }
    if (ct_sp != 0) ir4_die("unconsumed integer at top level");

    /* Pass 1.5: Reachability / Dead-Code Elimination from 'main' */
    {
        int main_idx = ir4_find_func("main");
        u8 f;
        if (main_idx >= 0) {
            mark_reachable((u8)main_idx);
        } else {
            for (f = 0; f < func_count; f++) {
                if (!funcs[f].is_extern) words[f].reachable = 1;
            }
        }

        /* Pass 2: Emit reachable words */
        for (f = 0; f < func_count; f++) {
            if (!funcs[f].is_extern && words[f].reachable) {
                emit_word(f);
            }
        }
    }
}

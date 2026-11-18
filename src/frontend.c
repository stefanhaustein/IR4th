#include "frontend.h"
#include "codebuf.h"
#include "strpool.h"
#include "vec.h"

VEC_OF(ByteVec, u8);

typedef enum { BLOCK_IF = 1, BLOCK_BEGIN = 2 } BlockKind;

typedef struct {
    BlockKind kind;
    size_t patch_else;
    size_t patch_end;
    size_t loop_addr;
    ByteVec saved_stack;
    ByteVec else_stack;
    u8 has_else;
    u8 saved_unreach;
    u8 if_unreach;
} BlockInfo;

typedef enum {
    IR_FN_END,
    IR_PUSH_W,
    IR_PUSH_A,
    IR_PUSH_F,
    IR_PUSH_I32,
    IR_PUSH_DATA,
    IR_PUSH_HEAP_START,
    IR_LOCAL_OP,
    IR_STACK_OP,
    IR_ALU_OP,
    IR_CALL,
    IR_CALL_EXTERN,
    IR_PUSH_FN,
    IR_ICALL,
    IR_BR_FWD,
    IR_BR_BACK
} IrKind;

typedef struct {
    IrKind kind;
    union {
        i32 w;
        u32 a;
        double f;
        u16 offset;
        struct {
            LocalOp op;
            u8 slot;
            StackType type;
            u16 stack_bytes;
        } loc;
        struct {
            StackOp op;
            StackType t1;
            StackType t2;
        } stk;
        IrOp op;
        size_t func_idx;
        struct {
            BranchCond cond;
            size_t target;
        } br;
    } u;
} IrInstr;

enum { CODE_REL_SELF = 1, CODE_REL_STACK = 2 };

typedef struct {
    size_t ir_off;
    u16 addend;
    u8 kind;
} CodeReloc;

typedef struct {
    size_t ir_start;
    size_t ir_len;
    size_t reloc_start;
    size_t reloc_cnt;
    u8 param_cnt;
    u8 local_count;
    LocalVar locals[MAX_LOCALS];
    u8 reachable;
    u8 is_code;
} WordIR;

VEC_OF(IrVec, IrInstr);
VEC_OF(WordVec, WordIR *);
VEC_OF(CodeRelocVec, CodeReloc);

static IrVec ir_buf;
static size_t fold_fence = 0;
static WordVec words;
static ByteVec raw_code_buf;
static CodeRelocVec code_relocs;

typedef struct {
    FILE *fp;
    const char *path;
    u16 line_num;
    u16 req_group;
} SrcFile;

VEC_OF(SrcVec, SrcFile);
VEC_OF(StrVec, const char *);

static SrcVec src_stack;
static u16 next_req_group = 1;
static StrVec extra_lib_roots;

void ir4_add_lib_root(const char *path) {
    if (!path || path[0] == '\0')
        return;
    VEC_PUSH(extra_lib_roots, path);
}
static StrVec seen_files;
static const Backend *be;
static const char *platform_name = NULL;

VEC_OF(FuncVec, FuncSig *);
static FuncVec funcs;

typedef struct {
    const char *name;
    u16 offset;
    u8 type; /* TY_NONE for :bytes / data buffers; TY_W/TY_A/TY_F for typed globals */
} BufferSym;

static StrBuf path_scratch;

VEC_OF(BufVec, BufferSym *);
static BufVec buffers;

static LocalVar locals[MAX_LOCALS];
static u8 local_count = 0;
static u8 cur_outs[4];
static u8 cur_out_count = 0;

VEC_OF(BlockVec, BlockInfo *);

static ByteVec tstack;
static BlockVec blocks;

void ir4_die(const char *msg) {
    if (src_stack.len > 0) {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%u): %s\n", cur->path, cur->line_num, msg);
    } else {
        fprintf(stderr, "ir4c error: %s\n", msg);
    }
    exit(1);
}

static const char *type_name(u8 t) {
    if (t == TY_W)
        return ":i";
    if (t == TY_A)
        return ":a";
    if (t == TY_F)
        return ":f";
    if (t == TY_I32)
        return ":i32";
    return "?";
}

static u8 parse_type_tok(const char *tok) {
    if (strcmp(tok, ":i") == 0)
        return TY_W;
    if (strcmp(tok, ":a") == 0)
        return TY_A;
    if (strcmp(tok, ":f") == 0)
        return TY_F;
    if (strcmp(tok, ":i32") == 0)
        return TY_I32;
    return TY_NONE;
}

static int skip_ws_and_comments(void) {
    for (;;) {
        SrcFile *cur;
        int c;
        if (src_stack.len == 0)
            return EOF;
        cur = &src_stack.data[src_stack.len - 1];
        c = fgetc(cur->fp);
        if (c == EOF) {
            if (src_stack.len > 1) {
                fclose(cur->fp);
                src_stack.len--;
                continue;
            }
            return EOF;
        }
        if (c == '\n') {
            cur->line_num++;
            continue;
        }
        if (isspace((unsigned char)c))
            continue;
        if (c == '/') {
            int c2 = fgetc(cur->fp);
            if (c2 == '/') {
                while ((c = fgetc(cur->fp)) != EOF && c != '\n')
                    ;
                if (c == '\n')
                    cur->line_num++;
                continue;
            }
            if (c2 != EOF)
                ungetc(c2, cur->fp);
        }
        return c;
    }
}

static const char *read_quoted_intern(FILE *fp, const char *err_unterm) {
    int c = fgetc(fp);
    sb_reset(&path_scratch);
    while (c != EOF && c != '\n' && c != '"') {
        sb_putc(&path_scratch, (char)c);
        c = fgetc(fp);
    }
    if (c != '"')
        ir4_die(err_unterm);
    return sb_intern(&path_scratch);
}

static void parse_line_directive(SrcFile *cur, int last_c) {
    int c = last_c;
    u32 new_line = 0;
    int has_digits = 0;

    if (c == '\n' || c == EOF)
        ir4_die("expected line number after '#line'");
    while (c == ' ' || c == '\t')
        c = fgetc(cur->fp);
    while (c >= '0' && c <= '9') {
        new_line = new_line * 10 + (u32)(c - '0');
        has_digits = 1;
        c = fgetc(cur->fp);
    }
    if (!has_digits || new_line == 0)
        ir4_die("invalid line number in '#line'");
    while (c == ' ' || c == '\t')
        c = fgetc(cur->fp);
    if (c == '"') {
        cur->path = read_quoted_intern(cur->fp, "unterminated filename in '#line'");
        c = fgetc(cur->fp);
    }
    while (c != EOF && c != '\n')
        c = fgetc(cur->fp);
    cur->line_num = (u16)new_line;
}

static StrBuf tok_sb;

static const char *next_tok(void) {
    for (;;) {
        int c = skip_ws_and_comments();
        if (c == EOF)
            return NULL;

        {
            SrcFile *cur = &src_stack.data[src_stack.len - 1];
            sb_reset(&tok_sb);
            while (c != EOF && !isspace((unsigned char)c)) {
                sb_putc(&tok_sb, (char)c);
                c = fgetc(cur->fp);
            }
            if (strcmp(tok_sb.data, "#line") == 0) {
                parse_line_directive(cur, c);
                continue;
            }
            if (c == '\n')
                cur->line_num++;
        }
        return tok_sb.data;
    }
}

static const char *next_tok_intern(void) { return next_tok() ? sb_intern(&tok_sb) : NULL; }

static const char *join_path(const char *a, const char *b) {
    sb_reset(&path_scratch);
    sb_append(&path_scratch, a);
    sb_putc(&path_scratch, '/');
    sb_append(&path_scratch, b);
    return sb_intern(&path_scratch);
}

static const char *resolve_path(const char *base_path, const char *rel) {
    const char *slash;
    size_t dir_len = 0;

    while (rel[0] == '.' && rel[1] == '/') {
        rel += 2;
    }

    if (rel[0] != '/' && base_path != NULL) {
        slash = strrchr(base_path, '/');
        if (slash != NULL) {
            dir_len = (size_t)(slash - base_path + 1);
        }
    }

    if (dir_len == 0)
        return intern_str(rel);
    sb_reset(&path_scratch);
    sb_append_n(&path_scratch, base_path, dir_len);
    sb_append(&path_scratch, rel);
    return sb_intern(&path_scratch);
}

static FILE *try_open_require(const char *path, int *already_seen) {
    size_t i;
    const char *p = intern_find(path);
    *already_seen = 0;
    if (p) {
        for (i = 0; i < seen_files.len; i++) {
            if (seen_files.data[i] == p) {
                *already_seen = 1;
                return NULL;
            }
        }
    }
    return fopen(path, "r");
}

static const char *exe_dir = NULL;

void ir4_set_exe_path(const char *exe_path) {
    const char *sep = NULL;
    const char *p;
    size_t len;
    exe_dir = NULL;
    if (!exe_path)
        return;
    for (p = exe_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            sep = p;
    }
    if (!sep)
        return;
    len = (size_t)(sep - exe_path);
    exe_dir = (len == 0) ? intern_str("/") : intern_str_n(exe_path, len);
}

static void push_src_file(FILE *fp, const char *path, u16 req_group) {
    const char *ipath = intern_str(path);
    SrcFile *sf = VEC_ADD(src_stack);
    VEC_PUSH(seen_files, ipath);
    sf->fp = fp;
    sf->path = ipath;
    sf->line_num = 1;
    sf->req_group = req_group;
}

static void push_require_ex(const char *name, int lib_only) {
    const char *cands[3];
    int n_cands = 0;
    int c;
    FILE *found_fps[3];
    const char *found_paths[3];
    int found_cnt = 0;
    int seen_cnt = 0;

    if (name[0] != '/') {
        if (platform_name != NULL && platform_name[0] != '\0') {
            cands[n_cands++] = join_path(platform_name, name);
        }
        cands[n_cands++] = join_path(be->name, name);
    }
    cands[n_cands++] = intern_str(name);

    if (!lib_only) {
        for (c = 0; c < n_cands; c++) {
            const char *path = resolve_path(src_stack.data[src_stack.len - 1].path, cands[c]);
            int seen = 0;
            FILE *fp = try_open_require(path, &seen);
            if (seen)
                seen_cnt++;
            else if (fp) {
                found_fps[found_cnt] = fp;
                found_paths[found_cnt] = path;
                found_cnt++;
            }
        }
    }

    if (found_cnt == 0 && seen_cnt == 0 && name[0] != '/') {
        StrVec roots;
        size_t r;
        roots.data = NULL;
        roots.len = 0;
        roots.cap = 0;
        for (r = 0; r < extra_lib_roots.len; r++) {
            VEC_PUSH(roots, extra_lib_roots.data[r]);
        }
        if (exe_dir != NULL) {
            VEC_PUSH(roots, join_path(exe_dir, "../lib"));
            VEC_PUSH(roots, join_path(exe_dir, "lib"));
        }
        VEC_PUSH(roots, "lib");
        for (r = 0; r < roots.len && found_cnt == 0 && seen_cnt == 0; r++) {
            for (c = 0; c < n_cands; c++) {
                const char *path = join_path(roots.data[r], cands[c]);
                int seen = 0;
                FILE *fp = try_open_require(path, &seen);
                if (seen)
                    seen_cnt++;
                else if (fp) {
                    found_fps[found_cnt] = fp;
                    found_paths[found_cnt] = path;
                    found_cnt++;
                }
            }
        }
        free(roots.data);
    }

    if (found_cnt > 0) {
        u16 grp = next_req_group++;
        int i;
        /* Push least-specific first so most-specific is parsed first on LIFO stack. */
        for (i = found_cnt - 1; i >= 0; i--) {
            push_src_file(found_fps[i], found_paths[i], grp);
        }
        return;
    }
    if (seen_cnt > 0)
        return;

    {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%u): cannot open required file '%s'\n", cur->path,
                cur->line_num, name);
        exit(1);
    }
}

static const char *read_require_path(void) {
    int c = skip_ws_and_comments();
    SrcFile *cur = &src_stack.data[src_stack.len - 1];

    if (c != '"')
        ir4_die("expected quoted filename after 'require'");
    return read_quoted_intern(cur->fp, "unterminated filename in 'require'");
}

static void push_require(const char *name) { push_require_ex(name, 0); }

static int parse_int(const char *s, i32 *val) {
    char *endp;
    int base = 0;
    const char *p = s;

    if (*p == '$') {
        base = 16;
        p++;
        if (*p == '\0')
            return 0;
    } else {
        if (*p == '-' || *p == '+')
            p++;
        if (!isdigit((unsigned char)*p))
            return 0;
        p = s;
    }

    *val = (i32)strtol(p, &endp, base);
    return (*endp == '\0');
}

static int parse_float(const char *s, double *val) {
    char *endp;
    const char *p = s;

    if (*p == '-' || *p == '+')
        p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return 0;
    if (!isdigit((unsigned char)*p) && *p != '.')
        return 0;
    if (*p == '.' && !isdigit((unsigned char)p[1]))
        return 0;
    if (strchr(p, '.') == NULL && strchr(p, 'e') == NULL && strchr(p, 'E') == NULL) {
        return 0;
    }

    *val = strtod(s, &endp);
    return (*endp == '\0');
}

static u16 tstack_bytes(void) {
    u16 bytes = 0;
    size_t i;
    for (i = 0; i < tstack.len; i++) {
        if (tstack.data[i] == TY_F)
            bytes += be->float_bytes;
        else if (tstack.data[i] == TY_A)
            bytes += be->addr_bytes;
        else if (tstack.data[i] == TY_I32)
            bytes += 4;
        else
            bytes += be->word_bytes;
    }
    return bytes;
}

static void tpush(u8 t) { VEC_PUSH(tstack, t); }

static u8 tpop(void) {
    if (tstack.len == 0)
        ir4_die("type stack underflow");
    return tstack.data[--tstack.len];
}

static void tpop_expect(u8 expected) {
    u8 got = tpop();
    if (got != expected) {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%u): expected %s, got %s\n", cur->path, cur->line_num,
                type_name(expected), type_name(got));
        exit(1);
    }
}

static void check_return_stack(void) {
    size_t i;
    if (tstack.len != cur_out_count)
        ir4_die("return stack depth mismatch");
    for (i = 0; i < cur_out_count; i++) {
        if (tstack.data[i] != cur_outs[i])
            ir4_die("return stack type mismatch");
    }
}

static void stack_copy(ByteVec *dst, const ByteVec *src) {
    VEC_ENSURE(*dst, src->len);
    if (src->len > 0)
        memcpy(dst->data, src->data, src->len);
    dst->len = src->len;
}

static int stack_eq(const ByteVec *a, const ByteVec *b) {
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

static void free_block(BlockInfo *b) {
    free(b->saved_stack.data);
    free(b->else_stack.data);
    free(b);
}

static int find_local(const char *name) {
    u8 i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < local_count; i++) {
        if (locals[i].name == p)
            return (int)i;
    }
    return -1;
}

int ir4_find_func(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < funcs.len; i++) {
        if (funcs.data[i]->name == p)
            return (int)i;
    }
    return -1;
}

static int find_buffer(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < buffers.len; i++) {
        if (buffers.data[i]->name == p)
            return (int)i;
    }
    return -1;
}

u16 ir4_func_addr(size_t idx) {
    if (idx >= funcs.len)
        ir4_die("function index out of bounds");
    return funcs.data[idx]->addr;
}

const FuncSig *ir4_func_sig(size_t idx) {
    if (idx >= funcs.len)
        ir4_die("function index out of bounds");
    return funcs.data[idx];
}

typedef struct {
    const char *name;
    IrOp op;
    u8 in1;
    u8 in2;
    u8 out;
} BuiltinDef;

static const BuiltinDef builtins[] = {/* Native word (:i) arithmetic & bitwise */
                                      {"+", OP_ADD, TY_W, TY_W, TY_W},
                                      {"-", OP_SUB, TY_W, TY_W, TY_W},
                                      {"*", OP_MUL, TY_W, TY_W, TY_W},
                                      {"/", OP_DIV, TY_W, TY_W, TY_W},
                                      {"mod", OP_MOD, TY_W, TY_W, TY_W},
                                      {"u/", OP_UDIV, TY_W, TY_W, TY_W},
                                      {"umod", OP_UMOD, TY_W, TY_W, TY_W},
                                      {"and", OP_AND, TY_W, TY_W, TY_W},
                                      {"or", OP_OR, TY_W, TY_W, TY_W},
                                      {"xor", OP_XOR, TY_W, TY_W, TY_W},
                                      {"invert", OP_INVERT, TY_W, TY_NONE, TY_W},
                                      {"<<", OP_LSHIFT, TY_W, TY_W, TY_W},
                                      {">>", OP_ARSHIFT, TY_W, TY_W, TY_W},
                                      {"u>>", OP_RSHIFT, TY_W, TY_W, TY_W},

                                      /* Address (:a) arithmetic & comparisons (unsigned implied) */
                                      {"a+", OP_AADD, TY_W, TY_A, TY_A},
                                      {"a-", OP_ASUB, TY_W, TY_A, TY_A},
                                      {"adiff", OP_ADIFF, TY_A, TY_A, TY_W},
                                      {"a==", OP_AEQ, TY_A, TY_A, TY_W},
                                      {"a!=", OP_ANE, TY_A, TY_A, TY_W},
                                      {"a<", OP_ALT, TY_A, TY_A, TY_W},
                                      {"a>", OP_AGT, TY_A, TY_A, TY_W},

                                      /* Floating-point (:f) arithmetic & comparisons */
                                      {"f+", OP_FADD, TY_F, TY_F, TY_F},
                                      {"f-", OP_FSUB, TY_F, TY_F, TY_F},
                                      {"f*", OP_FMUL, TY_F, TY_F, TY_F},
                                      {"f/", OP_FDIV, TY_F, TY_F, TY_F},
                                      {"fnegate", OP_FNEG, TY_F, TY_NONE, TY_F},
                                      {"f==", OP_FEQ, TY_F, TY_F, TY_W},
                                      {"f!=", OP_FNE, TY_F, TY_F, TY_W},
                                      {"f<", OP_FLT, TY_F, TY_F, TY_W},
                                      {"f>", OP_FGT, TY_F, TY_F, TY_W},

                                      /* 32-bit integer (:i32) arithmetic, bitwise & comparisons */
                                      {"i32+", OP_ADD32, TY_I32, TY_I32, TY_I32},
                                      {"i32-", OP_SUB32, TY_I32, TY_I32, TY_I32},
                                      {"i32*", OP_MUL32, TY_I32, TY_I32, TY_I32},
                                      {"i32/", OP_DIV32, TY_I32, TY_I32, TY_I32},
                                      {"i32mod", OP_MOD32, TY_I32, TY_I32, TY_I32},
                                      {"u32/", OP_UDIV32, TY_I32, TY_I32, TY_I32},
                                      {"u32mod", OP_UMOD32, TY_I32, TY_I32, TY_I32},
                                      {"i32and", OP_AND32, TY_I32, TY_I32, TY_I32},
                                      {"i32or", OP_OR32, TY_I32, TY_I32, TY_I32},
                                      {"i32xor", OP_XOR32, TY_I32, TY_I32, TY_I32},
                                      {"i32invert", OP_INV32, TY_I32, TY_NONE, TY_I32},
                                      {"i32<<", OP_SHL32, TY_W, TY_I32, TY_I32},
                                      {"i32>>", OP_ASHR32, TY_W, TY_I32, TY_I32},
                                      {"u32>>", OP_RSHR32, TY_W, TY_I32, TY_I32},
                                      {"i32==", OP_EQ32, TY_I32, TY_I32, TY_W},
                                      {"i32!=", OP_NE32, TY_I32, TY_I32, TY_W},
                                      {"i32<", OP_LT32, TY_I32, TY_I32, TY_W},
                                      {"i32>", OP_GT32, TY_I32, TY_I32, TY_W},
                                      {"i32<=", OP_LE32, TY_I32, TY_I32, TY_W},
                                      {"i32>=", OP_GE32, TY_I32, TY_I32, TY_W},
                                      {"u32<", OP_ULT32, TY_I32, TY_I32, TY_W},
                                      {"u32>", OP_UGT32, TY_I32, TY_I32, TY_W},
                                      {"u32<=", OP_ULE32, TY_I32, TY_I32, TY_W},
                                      {"u32>=", OP_UGE32, TY_I32, TY_I32, TY_W},

                                      /* Native word (:i) comparisons */
                                      {"==", OP_EQ, TY_W, TY_W, TY_W},
                                      {"!=", OP_NE, TY_W, TY_W, TY_W},
                                      {"<", OP_LT, TY_W, TY_W, TY_W},
                                      {">", OP_GT, TY_W, TY_W, TY_W},
                                      {"<=", OP_LE, TY_W, TY_W, TY_W},
                                      {">=", OP_GE, TY_W, TY_W, TY_W},
                                      {"u<", OP_ULT, TY_W, TY_W, TY_W},
                                      {"u>", OP_UGT, TY_W, TY_W, TY_W},
                                      {"u<=", OP_ULE, TY_W, TY_W, TY_W},
                                      {"u>=", OP_UGE, TY_W, TY_W, TY_W},

                                      /* Memory load/store */
                                      {"i8@", OP_I8FETCH, TY_A, TY_NONE, TY_W},
                                      {"u8@", OP_U8FETCH, TY_A, TY_NONE, TY_W},
                                      {"i8!", OP_I8STORE, TY_A, TY_W, TY_NONE},
                                      {"i16@", OP_I16FETCH, TY_A, TY_NONE, TY_W},
                                      {"u16@", OP_U16FETCH, TY_A, TY_NONE, TY_W},
                                      {"i16!", OP_I16STORE, TY_A, TY_W, TY_NONE},
                                      {"@", OP_FETCH, TY_A, TY_NONE, TY_W},
                                      {"!", OP_STORE, TY_A, TY_W, TY_NONE},
                                      {"a@", OP_AFETCH, TY_A, TY_NONE, TY_A},
                                      {"a!", OP_ASTORE, TY_A, TY_A, TY_NONE},
                                      {"f@", OP_FFETCH, TY_A, TY_NONE, TY_F},
                                      {"f!", OP_FSTORE, TY_A, TY_F, TY_NONE},
                                      {"i32@", OP_I32FETCH, TY_A, TY_NONE, TY_I32},
                                      {"i32!", OP_I32STORE, TY_A, TY_I32, TY_NONE},

                                      /* Type conversions */
                                      {"sext", OP_SEXT, TY_W, TY_NONE, TY_W},
                                      {"i>a", OP_W2A, TY_W, TY_NONE, TY_A},
                                      {"a>i", OP_A2W, TY_A, TY_NONE, TY_W},
                                      {"i>f", OP_W2F, TY_W, TY_NONE, TY_F},
                                      {"f>i", OP_F2W, TY_F, TY_NONE, TY_W},
                                      {"i>i32", OP_W2I32, TY_W, TY_NONE, TY_I32},
                                      {"i>u32", OP_U2I32, TY_W, TY_NONE, TY_I32},
                                      {"i32>i", OP_I322W, TY_I32, TY_NONE, TY_W}};

static IrInstr *emit_ir(IrKind kind, WordIR *w) {
    IrInstr *ins = VEC_ADD(ir_buf);
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
        case OP_ADD:
            *out = (i32)(i16)(ua + ub);
            return 1;
        case OP_SUB:
            *out = (i32)(i16)(ua - ub);
            return 1;
        case OP_EQ:
            *out = (sa == sb) ? -1 : 0;
            return 1;
        case OP_NE:
            *out = (sa != sb) ? -1 : 0;
            return 1;
        case OP_LT:
            *out = (sa < sb) ? -1 : 0;
            return 1;
        case OP_GT:
            *out = (sa > sb) ? -1 : 0;
            return 1;
        case OP_LE:
            *out = (sa <= sb) ? -1 : 0;
            return 1;
        case OP_GE:
            *out = (sa >= sb) ? -1 : 0;
            return 1;
        case OP_ULT:
            *out = (ua < ub) ? -1 : 0;
            return 1;
        case OP_UGT:
            *out = (ua > ub) ? -1 : 0;
            return 1;
        case OP_ULE:
            *out = (ua <= ub) ? -1 : 0;
            return 1;
        case OP_UGE:
            *out = (ua >= ub) ? -1 : 0;
            return 1;
        default:
            return 0;
        }
    }
    if (be->word_bytes == 8) {
        if ((op == OP_ADD || op == OP_SUB) &&
            (a < -0x3FFFFFFF || a > 0x3FFFFFFF || b < -0x3FFFFFFF || b > 0x3FFFFFFF)) {
            return 0;
        }
        if ((op == OP_ULT || op == OP_UGT || op == OP_ULE || op == OP_UGE) && (a < 0 || b < 0)) {
            return 0;
        }
    }
    switch (op) {
    case OP_ADD:
        *out = (i32)((u32)a + (u32)b);
        return 1;
    case OP_SUB:
        *out = (i32)((u32)a - (u32)b);
        return 1;
    case OP_EQ:
        *out = (a == b) ? -1 : 0;
        return 1;
    case OP_NE:
        *out = (a != b) ? -1 : 0;
        return 1;
    case OP_LT:
        *out = (a < b) ? -1 : 0;
        return 1;
    case OP_GT:
        *out = (a > b) ? -1 : 0;
        return 1;
    case OP_LE:
        *out = (a <= b) ? -1 : 0;
        return 1;
    case OP_GE:
        *out = (a >= b) ? -1 : 0;
        return 1;
    case OP_ULT:
        *out = ((u32)a < (u32)b) ? -1 : 0;
        return 1;
    case OP_UGT:
        *out = ((u32)a > (u32)b) ? -1 : 0;
        return 1;
    case OP_ULE:
        *out = ((u32)a <= (u32)b) ? -1 : 0;
        return 1;
    case OP_UGE:
        *out = ((u32)a >= (u32)b) ? -1 : 0;
        return 1;
    default:
        return 0;
    }
}

static IrOp swapped_store_op(IrOp op) {
    switch (op) {
    case OP_I8STORE:
        return OP_SI8STORE;
    case OP_I16STORE:
        return OP_SI16STORE;
    case OP_STORE:
        return OP_SSTORE;
    case OP_ASTORE:
        return OP_SASTORE;
    case OP_FSTORE:
        return OP_SFSTORE;
    default:
        return op;
    }
}

static int handle_builtin(const char *tok, WordIR *w) {
    u8 a, b;
    size_t i;
    IrInstr *ins;

    /* Stack manipulation (polymorphic) */
    if (strcmp(tok, "dup") == 0) {
        a = tpop();
        tpush(a);
        tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DUP;
        ins->u.stk.t1 = (StackType)a;
        ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "drop") == 0) {
        a = tpop();
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DROP;
        ins->u.stk.t1 = (StackType)a;
        ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "swap") == 0) {
        b = tpop();
        a = tpop();
        tpush(b);
        tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_SWAP;
        ins->u.stk.t1 = (StackType)b;
        ins->u.stk.t2 = (StackType)a;
        return 1;
    }
    if (strcmp(tok, "over") == 0) {
        b = tpop();
        a = tpop();
        tpush(a);
        tpush(b);
        tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_OVER;
        ins->u.stk.t1 = (StackType)b;
        ins->u.stk.t2 = (StackType)a;
        return 1;
    }

    {
        int matched_name = 0;
        for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            int need;
            if (strcmp(tok, builtins[i].name) != 0)
                continue;
            if (be->word_bytes == 2 &&
                (builtins[i].op == OP_MUL32 || builtins[i].op == OP_DIV32 ||
                 builtins[i].op == OP_MOD32 || builtins[i].op == OP_UDIV32 ||
                 builtins[i].op == OP_UMOD32 || builtins[i].op == OP_SHL32 ||
                 builtins[i].op == OP_ASHR32 || builtins[i].op == OP_RSHR32)) {
                continue;
            }
            matched_name = 1;
            need = (builtins[i].in2 != TY_NONE) ? 2 : 1;
            if (tstack.len < (size_t)need)
                continue;
            if (tstack.data[tstack.len - 1] != builtins[i].in1)
                continue;
            if (builtins[i].in2 != TY_NONE && tstack.data[tstack.len - 2] != builtins[i].in2)
                continue;

            tstack.len -= need;
            if (builtins[i].out != TY_NONE)
                tpush(builtins[i].out);
            if ((builtins[i].op == OP_W2A || builtins[i].op == OP_W2I32 ||
                 builtins[i].op == OP_U2I32) &&
                w->ir_len >= fold_fence + 1) {
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                if (p1->kind == IR_PUSH_W) {
                    if (builtins[i].op == OP_W2A) {
                        p1->kind = IR_PUSH_A;
                        p1->u.a = (u32)p1->u.w;
                    } else {
                        p1->kind = IR_PUSH_I32;
                    }
                    return 1;
                }
            }
            if (w->ir_len >= fold_fence + 1) {
                IrOp sop = swapped_store_op(builtins[i].op);
                if (sop != builtins[i].op) {
                    IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                    if (p1->kind == IR_STACK_OP && p1->u.stk.op == OP_SWAP) {
                        p1->kind = IR_ALU_OP;
                        p1->u.op = sop;
                        return 1;
                    }
                }
            }
            if (w->ir_len >= fold_fence + 2) {
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 2];
                IrInstr *p2 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                i32 folded;
                if (p1->kind == IR_PUSH_W && p2->kind == IR_PUSH_W &&
                    try_fold_const_w(builtins[i].op, p1->u.w, p2->u.w, &folded)) {
                    p1->u.w = folded;
                    w->ir_len--;
                    ir_buf.len--;
                    return 1;
                }
            }
            if (w->ir_len >= fold_fence + 1) {
                IrOp rop = builtins[i].op;
                if (rop == OP_MUL || rop == OP_UDIV || rop == OP_UMOD || rop == OP_MUL32 ||
                    rop == OP_UDIV32 || rop == OP_UMOD32) {
                    IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                    u8 is32 = (rop == OP_MUL32 || rop == OP_UDIV32 || rop == OP_UMOD32);
                    if ((!is32 && p1->kind == IR_PUSH_W) || (is32 && p1->kind == IR_PUSH_I32)) {
                        u32 v = (!is32 && be->word_bytes == 2) ? (u32)(u16)p1->u.w : (u32)p1->u.w;
                        if (v == 1 && (rop == OP_MUL || rop == OP_UDIV || rop == OP_MUL32 ||
                                       rop == OP_UDIV32)) {
                            w->ir_len--;
                            ir_buf.len--;
                            return 1;
                        }
                        if (v >= 2 && (v & (v - 1)) == 0 &&
                            (is32 || be->word_bytes == 2 || p1->u.w > 0)) {
                            u8 sh = 0;
                            while ((v >> sh) > 1)
                                sh++;
                            if (rop == OP_UMOD || rop == OP_UMOD32) {
                                p1->u.w = (i32)(v - 1);
                                ins = emit_ir(IR_ALU_OP, w);
                                ins->u.op = is32 ? OP_AND32 : OP_AND;
                                return 1;
                            }
                            p1->kind = IR_PUSH_W;
                            p1->u.w = (i32)sh;
                            ins = emit_ir(IR_ALU_OP, w);
                            ins->u.op = (rop == OP_MUL)     ? OP_LSHIFT
                                        : (rop == OP_UDIV)  ? OP_RSHIFT
                                        : (rop == OP_MUL32) ? OP_SHL32
                                                            : OP_RSHR32;
                            return 1;
                        }
                    }
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
    u8 src_slot;
    u8 dst_slot;
    u16 size;
    const char *name;
} BytesParamCopy;

static BytesParamCopy cur_bytes_copies[8];
static u8 cur_bytes_copy_count = 0;
static u8 cur_bytes_cnt_slot = 0;

static void flush_bytes_params(u8 *params_flushed) {
    u8 k;
    if (*params_flushed)
        return;
    *params_flushed = 1;
    if (cur_bytes_copy_count == 0)
        return;
    for (k = 0; k < cur_bytes_copy_count; k++) {
        if (local_count >= MAX_LOCALS)
            ir4_die("too many parameters/locals");
        locals[local_count].name = cur_bytes_copies[k].name;
        locals[local_count].type = TY_A;
        locals[local_count].slot = local_count;
        locals[local_count].size = cur_bytes_copies[k].size;
        cur_bytes_copies[k].dst_slot = local_count;
        local_count++;
    }
}

static void parse_icall_sig(FuncSig *sig) {
    const char *tok;
    u8 in_outs = 0;

    while ((tok = next_tok()) != NULL) {
        u8 ty;
        if (strcmp(tok, ")") == 0) {
            if (in_outs && sig->out_count == 0)
                ir4_die("expected return type after '->'");
            return;
        }
        if (strcmp(tok, "->") == 0) {
            if (in_outs)
                ir4_die("duplicate '->' in signature");
            in_outs = 1;
            continue;
        }
        ty = parse_type_tok(tok);
        if (ty == TY_NONE)
            ir4_die("expected type token (:i, :a, :f, :i32) in indirect call signature");
        if (in_outs) {
            if (sig->out_count >= 4)
                ir4_die("too many return types");
            sig->out_types[sig->out_count++] = ty;
        } else {
            if (sig->in_count >= 8)
                ir4_die("too many input types");
            sig->in_types[sig->in_count++] = ty;
        }
    }
    ir4_die("unterminated '(' in indirect call");
}

static void parse_sig_body(FuncSig *sig, u8 *param_cnt_out, int allow_bytes) {
    const char *tok;
    u8 param_cnt = 0;
    u8 in_outs = 0;
    u8 params_flushed = 0;

    local_count = 0;
    cur_out_count = 0;
    cur_bytes_copy_count = 0;
    cur_bytes_cnt_slot = 0;
    sig->out_reg = intern_str("");

    /* Parse inputs -> outputs ) (after '(' has been consumed) */
    while ((tok = next_tok()) != NULL) {
        u8 ty;
        const char *ty_tok;
        if (strcmp(tok, ")") == 0) {
            flush_bytes_params(&params_flushed);
            if (in_outs && sig->out_count == 0)
                ir4_die("expected return type after '->'");
            if (param_cnt_out)
                *param_cnt_out = param_cnt;
            return;
        }
        if (strcmp(tok, "->") == 0) {
            if (in_outs)
                ir4_die("duplicate '->' in signature");
            flush_bytes_params(&params_flushed);
            in_outs = 1;
            continue;
        }

        if (in_outs) {
            ty = parse_type_tok(tok);
            if (ty == TY_NONE) {
                if (!allow_bytes && cur_out_count == 0 && sig->out_reg[0] == '\0') {
                    sig->out_reg = intern_str(tok);
                    tok = next_tok();
                    if (!tok)
                        ir4_die("expected type token (:i, :a, :f) after extern register");
                    ty = parse_type_tok(tok);
                }
                if (ty == TY_NONE)
                    ir4_die("expected type token (:i, :a, :f) in signature");
            }
            if (cur_out_count >= 4)
                ir4_die("too many return types");
            sig->out_types[sig->out_count++] = ty;
            cur_outs[cur_out_count++] = ty;
        } else {
            if (local_count >= MAX_LOCALS)
                ir4_die("too many parameters/locals");
            if (parse_type_tok(tok) != TY_NONE) {
                ir4_die("expected identifier before type in signature");
            }
            locals[local_count].name = intern_str(tok);
            ty_tok = next_tok();
            if (!ty_tok)
                ir4_die("expected type token (:i, :a, :f) after identifier in signature");
            if (strcmp(ty_tok, ":bytes") == 0) {
                i32 sz = 0;
                char bp_name[32];
                ty_tok = next_tok();
                if (!ty_tok || !parse_int(ty_tok, &sz) || sz <= 0 || sz > 0xFFFF) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                if (!allow_bytes)
                    ir4_die("':bytes' not allowed in extern signature");
                if (sig->in_count >= 8)
                    ir4_die("too many parameters");
                cur_bytes_copies[cur_bytes_copy_count].src_slot = local_count;
                cur_bytes_copies[cur_bytes_copy_count].size = (u16)sz;
                cur_bytes_copies[cur_bytes_copy_count].name = locals[local_count].name;
                cur_bytes_copy_count++;
                sprintf(bp_name, "__bp%u", (unsigned)param_cnt);
                locals[local_count].name = intern_str(bp_name);
                locals[local_count].type = TY_A;
                locals[local_count].slot = local_count;
                locals[local_count].size = 0;
                sig->in_types[sig->in_count] = TY_A;
                sig->in_count++;
                param_cnt++;
                local_count++;
                continue;
            }
            ty = parse_type_tok(ty_tok);
            if (ty == TY_NONE)
                ir4_die("expected type token (:i, :a, :f) in signature");
            locals[local_count].type = ty;
            locals[local_count].slot = local_count;
            locals[local_count].size = 0;
            if (sig->in_count >= 8)
                ir4_die("too many parameters");
            sig->in_types[sig->in_count] = ty;
            sig->in_regs[sig->in_count] = locals[local_count].name;
            sig->in_count++;
            param_cnt++;
            local_count++;
        }
    }

    ir4_die("unexpected EOF in signature (missing ')')");
}

static void compile_body(WordIR *w);

static void emit_bytes_param_copies(WordIR *w) {
    u8 k;
    for (k = 0; k < cur_bytes_copy_count; k++) {
        IrInstr *ins;
        u8 src = cur_bytes_copies[k].src_slot;
        u8 dst = cur_bytes_copies[k].dst_slot;
        u16 sz = cur_bytes_copies[k].size;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_ADDR;
        ins->u.loc.slot = dst;
        ins->u.loc.type = TY_A;
        ins->u.loc.stack_bytes = 0;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_GET;
        ins->u.loc.slot = src;
        ins->u.loc.type = TY_A;
        ins->u.loc.stack_bytes = be->addr_bytes;

        ins = emit_ir(IR_PUSH_W, w);
        ins->u.w = (i32)sz;

        ins = emit_ir(IR_ALU_OP, w);
        ins->u.op = OP_BCOPY;
        fold_fence = w->ir_len;
    }
}

static int sigs_match(const FuncSig *a, const FuncSig *b) {
    return a->in_count == b->in_count && a->out_count == b->out_count &&
           memcmp(a->in_types, b->in_types, a->in_count) == 0 &&
           memcmp(a->out_types, b->out_types, a->out_count) == 0;
}

static size_t alloc_func(FuncSig **sig_out, WordIR **w_out) {
    size_t idx = funcs.len;
    FuncSig *sig = NEW(FuncSig);
    WordIR *w = NEW(WordIR);
    VEC_PUSH(funcs, sig);
    VEC_PUSH(words, w);
    if (sig_out)
        *sig_out = sig;
    if (w_out)
        *w_out = w;
    return idx;
}

static void compile_word(int is_default) {
    const char *tok;
    const char *name;
    FuncSig *sig;
    WordIR *w;
    WordIR discard_w;
    int discard_body = 0;
    size_t saved_ir_count = ir_buf.len;
    size_t fidx;
    int existing;
    u8 param_cnt = 0;
    u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;

    name = next_tok_intern();
    if (!name)
        ir4_die("expected word name after 'fn'");
    tok = next_tok();
    if (!tok || strcmp(tok, "(") != 0)
        ir4_die("expected '(' after word name");

    existing = ir4_find_func(name);
    if (existing >= 0) {
        FuncSig fn_sig;
        FuncSig *ex = funcs.data[existing];
        if (ex->is_extern) {
            ir4_die("duplicate function definition");
        }
        if (ex->is_defined) {
            if (cur_grp == 0 || ex->req_group != cur_grp) {
                ir4_die("duplicate function definition");
            }
            if (is_default && ex->is_default) {
                ir4_die("duplicate 'default' function definition");
            }
            if (!is_default && !ex->is_default) {
                ir4_die("duplicate function definition");
            }
        }
        fidx = (size_t)existing;
        memset(&fn_sig, 0, sizeof(FuncSig));
        fn_sig.name = intern_str(name);
        tstack.len = 0;
        blocks.len = 0;
        parse_sig_body(&fn_sig, &param_cnt, 1);
        if (!sigs_match(&fn_sig, ex)) {
            ir4_die(ex->is_defined
                        ? "override signature does not match 'default' declaration"
                        : "function signature does not match 'fwd' declaration");
        }
        if (is_default && ex->is_defined) {
            /* Already overridden by a more-specific file in this require group. */
            discard_body = 1;
            memset(&discard_w, 0, sizeof(WordIR));
            w = &discard_w;
        } else {
            *ex = fn_sig;
            ex->is_defined = 1;
            ex->is_default = (u8)is_default;
            ex->req_group = cur_grp;
            sig = ex;
            w = words.data[fidx];
            memset(w, 0, sizeof(WordIR));
        }
    } else {
        fidx = alloc_func(&sig, &w);
        sig->name = intern_str(name);
        sig->is_defined = 1;
        sig->is_default = (u8)is_default;
        sig->req_group = cur_grp;
        tstack.len = 0;
        blocks.len = 0;
        parse_sig_body(sig, &param_cnt, 1);
    }

    w->param_cnt = param_cnt;
    emit_bytes_param_copies(w);
    compile_body(w);
    if (discard_body) {
        ir_buf.len = saved_ir_count;
    }
}

VEC_OF(IdxVec, size_t);
static IdxVec init_funcs;

static void raw_emit_u8(WordIR *w, u8 b) {
    VEC_PUSH(raw_code_buf, b);
    w->ir_len++;
}

static void compile_code(void) {
    const char *tok;
    const char *name;
    FuncSig *sig;
    WordIR *w;
    size_t fidx;
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;

    int existing;
    u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;

    name = next_tok_intern();
    if (!name)
        ir4_die("expected word name after 'code'");
    existing = ir4_find_func(name);
    if (existing >= 0) {
        FuncSig code_sig;
        FuncSig *ex = funcs.data[existing];
        if (!ex->is_defined || ex->is_extern || !ex->is_default || cur_grp == 0 ||
            ex->req_group != cur_grp) {
            ir4_die("duplicate function definition");
        }
        tok = next_tok();
        if (!tok || strcmp(tok, "(") != 0)
            ir4_die("expected '(' after 'code' name");
        fidx = (size_t)existing;
        memset(&code_sig, 0, sizeof(FuncSig));
        code_sig.name = name;
        parse_sig_body(&code_sig, NULL, 0);
        if (!sigs_match(&code_sig, ex)) {
            ir4_die("override signature does not match 'default' declaration");
        }
        *ex = code_sig;
        ex->is_defined = 1;
        ex->is_default = 0;
        ex->req_group = cur_grp;
        sig = ex;
        w = words.data[fidx];
        memset(w, 0, sizeof(WordIR));
    } else {
        tok = next_tok();
        if (!tok || strcmp(tok, "(") != 0)
            ir4_die("expected '(' after 'code' name");
        fidx = alloc_func(&sig, &w);
        sig->name = name;
        sig->is_defined = 1;
        sig->req_group = cur_grp;
        parse_sig_body(sig, NULL, 0);
    }

    w->is_code = 1;
    w->ir_start = raw_code_buf.len;
    w->ir_len = 0;
    w->reloc_start = code_relocs.len;
    w->reloc_cnt = 0;

    while ((tok = next_tok()) != NULL) {
        if (strcmp(tok, "end") == 0) {
            if (has_pending)
                ir4_die("missing width (:i8, :i16, :i32) in 'code'");
            return;
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255)
                ir4_die(":i8 value out of range");
            raw_emit_u8(w, (u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            u16 v;
            if (!has_pending)
                ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535)
                ir4_die(":i16 value out of range");
            v = (u16)(pending & 0xFFFF);
            raw_emit_u8(w, (u8)(v & 0xFF));
            raw_emit_u8(w, (u8)((v >> 8) & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i32") == 0) {
            u32 v;
            if (!has_pending)
                ir4_die("expected integer before ':i32'");
            v = (u32)pending;
            raw_emit_u8(w, (u8)(v & 0xFF));
            raw_emit_u8(w, (u8)((v >> 8) & 0xFF));
            raw_emit_u8(w, (u8)((v >> 16) & 0xFF));
            raw_emit_u8(w, (u8)((v >> 24) & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":ar") == 0 || strcmp(tok, ":as") == 0) {
            CodeReloc *rel;
            if (!has_pending)
                ir4_die("expected offset before ':ar' or ':as'");
            if (pending < 0 || pending > 0xFFFF)
                ir4_die("relocation offset out of range");
            rel = VEC_ADD(code_relocs);
            rel->ir_off = w->ir_len;
            rel->addend = (u16)pending;
            rel->kind = (strcmp(tok, ":ar") == 0) ? CODE_REL_SELF : CODE_REL_STACK;
            if (rel->kind == CODE_REL_STACK && !be->stack_data_off) {
                ir4_die("':as' is only supported on targets with a RAM data stack");
            }
            w->reloc_cnt++;
            raw_emit_u8(w, 0x00);
            raw_emit_u8(w, 0x00);
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending)
                ir4_die("missing width (:i8, :i16, :i32) before literal in 'code'");
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
    size_t fidx;
    char iname[32];

    fidx = alloc_func(&sig, &w);
    sprintf(iname, "__init_%u", (unsigned)init_funcs.len);
    sig->name = intern_str(iname);
    sig->is_defined = 1;
    VEC_PUSH(init_funcs, fidx);

    tstack.len = 0;
    blocks.len = 0;
    cur_out_count = 0;
    local_count = 0;
    compile_body(w);
}

typedef struct {
    const char *name;
    size_t target_ir;
    u8 defined;
} LabelInfo;

typedef struct {
    size_t ir_idx;
    size_t label_idx;
} LabelRef;

VEC_OF(LabelVec, LabelInfo);
VEC_OF(LabelRefVec, LabelRef);

static IrOp global_fetch_op(u8 ty) {
    if (ty == TY_A)
        return OP_AFETCH;
    if (ty == TY_F)
        return OP_FFETCH;
    if (ty == TY_I32)
        return OP_I32FETCH;
    return OP_FETCH;
}

static IrOp global_store_op(u8 ty) {
    if (ty == TY_A)
        return OP_ASTORE;
    if (ty == TY_F)
        return OP_FSTORE;
    if (ty == TY_I32)
        return OP_I32STORE;
    return OP_STORE;
}

static u16 global_type_bytes(u8 ty) {
    if (ty == TY_A)
        return be->addr_bytes;
    if (ty == TY_F)
        return be->float_bytes;
    if (ty == TY_I32)
        return 4;
    return be->word_bytes;
}

static size_t find_or_add_label(LabelVec *labels, const char *name) {
    size_t i;
    const char *p = intern_str(name);
    LabelInfo *li;
    for (i = 0; i < labels->len; i++) {
        if (labels->data[i].name == p)
            return i;
    }
    li = VEC_ADD(*labels);
    li->name = p;
    li->target_ir = 0;
    li->defined = 0;
    return labels->len - 1;
}

static void emit_label_br(WordIR *w, LabelVec *labels, LabelRefVec *refs, size_t lidx,
                          BranchCond cond) {
    IrInstr *ins;
    if (labels->data[lidx].defined) {
        ins = emit_ir(IR_BR_BACK, w);
        ins->u.br.cond = cond;
        ins->u.br.target = labels->data[lidx].target_ir;
    } else {
        LabelRef *ref = VEC_ADD(*refs);
        ref->ir_idx = w->ir_len;
        ref->label_idx = lidx;
        ins = emit_ir(IR_BR_FWD, w);
        ins->u.br.cond = cond;
    }
    fold_fence = w->ir_len;
}

/* Parse a local 'var <name> :<type>' declaration and return its local slot. */
static int parse_local_var(int allow_bytes) {
    const char *vname, *vtype;
    u8 ty;
    int slot;
    vname = next_tok_intern();
    if (!vname)
        ir4_die("expected local variable name after 'var'");
    if (parse_type_tok(vname) != TY_NONE)
        ir4_die("expected identifier after 'var'");
    if (find_local(vname) >= 0)
        ir4_die("duplicate local variable name");
    if (local_count >= MAX_LOCALS)
        ir4_die("too many parameters/locals");
    vtype = next_tok();
    if (!vtype)
        ir4_die("expected type after local 'var' name");
    slot = local_count;
    locals[slot].name = vname;
    locals[slot].slot = (u8)slot;
    if (strcmp(vtype, ":bytes") == 0) {
        i32 sz = 0;
        if (!allow_bytes)
            ir4_die("cannot assign to ':bytes' local");
        vtype = next_tok();
        if (!vtype || !parse_int(vtype, &sz) || sz <= 0 || sz > 0xFFFF)
            ir4_die("invalid byte count after ':bytes'");
        locals[slot].type = TY_A;
        locals[slot].size = (u16)sz;
    } else {
        ty = parse_type_tok(vtype);
        if (ty == TY_NONE)
            ir4_die("expected type token (:i, :i32, :a, :f, :bytes) after 'var'");
        locals[slot].type = ty;
        locals[slot].size = 0;
    }
    local_count++;
    return slot;
}

static void compile_body(WordIR *w) {
    const char *tok;
    IrInstr *ins;
    u8 unreachable = 0;
    i32 num_val;
    LabelVec labels;
    LabelRefVec label_refs;

    labels.data = NULL;
    labels.len = 0;
    labels.cap = 0;
    label_refs.data = NULL;
    label_refs.len = 0;
    label_refs.cap = 0;

    w->ir_start = ir_buf.len;
    w->ir_len = 0;
    fold_fence = 0;

    /* Parse, type-check, and record IR for word body (Pass 1) */
    while ((tok = next_tok()) != NULL) {
        int idx;

        if (strcmp(tok, "end") == 0) {
            size_t r;
            if (blocks.len != 0)
                ir4_die("unclosed control block at 'end'");
            for (r = 0; r < label_refs.len; r++) {
                size_t lidx = label_refs.data[r].label_idx;
                if (!labels.data[lidx].defined)
                    ir4_die("undefined label");
                ir_buf.data[w->ir_start + label_refs.data[r].ir_idx].u.br.target =
                    labels.data[lidx].target_ir;
            }
            free(labels.data);
            free(label_refs.data);
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
            }
            w->local_count = local_count;
            if (local_count > 0) {
                memcpy(w->locals, locals, local_count * sizeof(LocalVar));
            }
            return;
        }

        if (strcmp(tok, "var") == 0) {
            parse_local_var(1);
            continue;
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
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '&'");
            idx = find_local(tok);
            if (idx >= 0) {
                if (locals[idx].size > 0)
                    ir4_die("':bytes' local is already an address");
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_ADDR;
                ins->u.loc.slot = locals[idx].slot;
                ins->u.loc.type = (StackType)locals[idx].type;
                ins->u.loc.stack_bytes = tstack_bytes();
                tpush(TY_A);
                continue;
            }
            idx = find_buffer(tok);
            if (idx >= 0) {
                BufferSym *bs = buffers.data[idx];
                if (bs->type == TY_NONE)
                    ir4_die("':bytes' or 'data' symbol is already an address");
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                tpush(TY_A);
                continue;
            }
            idx = ir4_find_func(tok);
            if (idx >= 0) {
                if (funcs.data[idx]->is_extern)
                    ir4_die("cannot take address of 'extern' function");
                ins = emit_ir(IR_PUSH_FN, w);
                ins->u.func_idx = (size_t)idx;
                tpush(TY_A);
                continue;
            }
            ir4_die("unknown variable or function in '&'");
        }

        if (strcmp(tok, "(") == 0) {
            FuncSig icall_sig;
            int k;
            memset(&icall_sig, 0, sizeof(FuncSig));
            parse_icall_sig(&icall_sig);
            tpop_expect(TY_A);
            for (k = (int)icall_sig.in_count - 1; k >= 0; k--) {
                tpop_expect(icall_sig.in_types[k]);
            }
            for (k = 0; k < (int)icall_sig.out_count; k++) {
                tpush(icall_sig.out_types[k]);
            }
            emit_ir(IR_ICALL, w);
            continue;
        }

        /* Local or typed global assignment: -> <ident> */
        if (strcmp(tok, "->") == 0) {
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '->'");
            idx = (strcmp(tok, "var") == 0) ? parse_local_var(0) : find_local(tok);
            if (idx >= 0) {
                if (locals[idx].size > 0)
                    ir4_die("cannot assign to ':bytes' local");
                tpop_expect(locals[idx].type);
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_SET;
                ins->u.loc.slot = locals[idx].slot;
                ins->u.loc.type = (StackType)locals[idx].type;
                ins->u.loc.stack_bytes = tstack_bytes();
                continue;
            }
            idx = find_buffer(tok);
            if (idx < 0)
                ir4_die("unknown variable in '->'");
            {
                BufferSym *bs = buffers.data[idx];
                if (bs->type == TY_NONE)
                    ir4_die("cannot assign to ':bytes' or 'data' symbol");
                tpop_expect(bs->type);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_store_op(bs->type);
            }
            continue;
        }

        /* Control flow */
        if (strcmp(tok, "if") == 0) {
            BlockInfo *b;
            tpop_expect(TY_W);
            b = NEW(BlockInfo);
            VEC_PUSH(blocks, b);
            b->kind = BLOCK_IF;
            b->has_else = 0;
            b->saved_unreach = unreachable;
            b->if_unreach = 0;
            stack_copy(&b->saved_stack, &tstack);
            b->patch_else = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "else") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_IF)
                ir4_die("'else' without 'if'");
            b = blocks.data[blocks.len - 1];
            b->has_else = 1;
            b->if_unreach = unreachable;
            stack_copy(&b->else_stack, &tstack);
            stack_copy(&tstack, &b->saved_stack);
            unreachable = b->saved_unreach;
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JMP;
            ir_buf.data[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "then") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_IF)
                ir4_die("'then' without 'if'");
            b = blocks.data[--blocks.len];
            if (!b->has_else) {
                if (!unreachable && !stack_eq(&tstack, &b->saved_stack))
                    ir4_die("unbalanced stack in 'if ... then'");
                stack_copy(&tstack, &b->saved_stack);
                unreachable = b->saved_unreach;
                ir_buf.data[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            } else {
                if (!b->if_unreach && !unreachable) {
                    if (!stack_eq(&tstack, &b->else_stack))
                        ir4_die("branch stack mismatch between 'if' and 'else'");
                } else if (!b->if_unreach && unreachable) {
                    stack_copy(&tstack, &b->else_stack);
                    unreachable = 0;
                }
                ir_buf.data[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            }
            free_block(b);
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            BlockInfo *b = NEW(BlockInfo);
            VEC_PUSH(blocks, b);
            b->kind = BLOCK_BEGIN;
            b->loop_addr = w->ir_len;
            fold_fence = w->ir_len;
            stack_copy(&b->saved_stack, &tstack);
            continue;
        }
        if (strcmp(tok, "while") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_BEGIN)
                ir4_die("'while' without 'begin'");
            b = blocks.data[blocks.len - 1];
            tpop_expect(TY_W);
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "repeat") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_BEGIN)
                ir4_die("'repeat' without 'begin'");
            b = blocks.data[--blocks.len];
            if (!stack_eq(&tstack, &b->saved_stack))
                ir4_die("loop body must leave stack even at 'repeat'");
            ins = emit_ir(IR_BR_BACK, w);
            ins->u.br.cond = BR_JMP;
            ins->u.br.target = b->loop_addr;
            ir_buf.data[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            free_block(b);
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "::") == 0) {
            size_t lidx;
            if (blocks.len != 0)
                ir4_die("'::' inside control block");
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after '::'");
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty at '::'");
            tstack.len = 0;
            unreachable = 0;
            lidx = find_or_add_label(&labels, tok);
            if (labels.data[lidx].defined)
                ir4_die("duplicate label");
            labels.data[lidx].defined = 1;
            labels.data[lidx].target_ir = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "jmp") == 0) {
            size_t lidx;
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after 'jmp'");
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty at 'jmp'");
            lidx = find_or_add_label(&labels, tok);
            if (!unreachable) {
                emit_label_br(w, &labels, &label_refs, lidx, BR_JMP);
                unreachable = 1;
            }
            continue;
        }
        if (strcmp(tok, "jz") == 0) {
            size_t lidx;
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after 'jz'");
            tpop_expect(TY_W);
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty after condition at 'jz'");
            lidx = find_or_add_label(&labels, tok);
            if (!unreachable) {
                emit_label_br(w, &labels, &label_refs, lidx, BR_JZ);
            }
            continue;
        }

        /* Core built-in word */
        if (handle_builtin(tok, w))
            continue;

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
            BufferSym *bs = buffers.data[idx];
            if (bs->type == TY_NONE) {
                tpush(TY_A);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
            } else {
                tpush(bs->type);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_fetch_op(bs->type);
            }
            continue;
        }

        /* User word call */
        idx = ir4_find_func(tok);
        if (idx >= 0) {
            FuncSig *callee = funcs.data[idx];
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
            ins->u.func_idx = (size_t)idx;
            continue;
        }

        {
            const SrcFile *cur = &src_stack.data[src_stack.len - 1];
            fprintf(stderr, "ir4c error (%s:%u): unknown word '%s'\n", cur->path, cur->line_num,
                    tok);
        }
        exit(1);
    }

    ir4_die("unexpected EOF inside word definition (missing ';')");
}

static u8 can_reach_func(size_t cur, size_t target, u8 *visited) {
    WordIR *w = words.data[cur];
    size_t i;
    if (funcs.data[cur]->is_extern || w->is_code)
        return 0;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            size_t callee = ins->u.func_idx;
            if (callee == target)
                return 1;
            if (!visited[callee]) {
                visited[callee] = 1;
                if (can_reach_func(callee, target, visited))
                    return 1;
            }
        }
    }
    return 0;
}

static void mark_reachable(size_t fidx) {
    WordIR *w = words.data[fidx];
    size_t i;
    if (funcs.data[fidx]->is_extern)
        return;
    if (!funcs.data[fidx]->is_defined)
        ir4_die("called 'fwd' function was never defined");
    if (w->reachable)
        return;
    w->reachable = 1;
    if (w->is_code)
        return;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            mark_reachable(ins->u.func_idx);
        } else if (ins->kind == IR_PUSH_FN) {
            mark_reachable(ins->u.func_idx);
        }
    }
}

typedef struct {
    u16 patch_offset;
    size_t target_ir;
} PendingBr;

typedef struct {
    u16 call_pc;
    u16 call_addr;
    size_t func_idx;
    u8 is_fn_ptr;
} FwdCall;

VEC_OF(FwdCallVec, FwdCall);
VEC_OF(U16Vec, u16);
VEC_OF(PendingBrVec, PendingBr);

static FwdCallVec fwd_calls;
static u16 heap_start_off = 0;

static void emit_word(size_t fidx) {
    static U16Vec ir_addr;
    FuncSig *sig = funcs.data[fidx];
    WordIR *w = words.data[fidx];
    PendingBrVec pending;
    size_t i, p;

    pending.data = NULL;
    pending.len = 0;
    pending.cap = 0;

    if (w->is_code) {
        u16 base_pc = cb_pc();
        sig->addr = be->cur_addr();
        sig->has_addr = 1;
        for (i = 0; i < w->ir_len; i++) {
            emit8(raw_code_buf.data[w->ir_start + i]);
        }
        for (i = 0; i < w->reloc_cnt; i++) {
            const CodeReloc *rel = &code_relocs.data[w->reloc_start + i];
            u16 pc = (u16)(base_pc + rel->ir_off);
            if (rel->kind == CODE_REL_SELF) {
                patch16(pc, (u16)(sig->addr + rel->addend));
            } else if (rel->kind == CODE_REL_STACK) {
                if (be == &be_6502 && be_6502_stack_zp_base() != 0) {
                    patch16(pc, (u16)(be_6502_stack_zp_base() + rel->addend));
                } else {
                    cb_add_data_reloc(pc, (u16)(be->stack_data_off() + rel->addend));
                }
            }
        }
        return;
    }

    VEC_ENSURE(ir_addr, w->ir_len + 1);
    sig->addr = be->cur_addr();
    sig->has_addr = 1;
    be->fn_begin(sig->name, w->param_cnt, w->locals, w->local_count, sig->is_recursive);

    for (i = 0; i <= w->ir_len; i++) {
        for (p = 0; p < pending.len; p++) {
            if (pending.data[p].target_ir == i) {
                be->patch_fwd(pending.data[p].patch_offset);
            }
        }
        if (i == w->ir_len)
            break;
        ir_addr.data[i] = be->cur_addr();
        {
            const IrInstr *ins = &ir_buf.data[w->ir_start + i];
            switch (ins->kind) {
            case IR_FN_END:
                be->fn_end(sig, w->locals, w->local_count);
                break;
            case IR_PUSH_W:
                if (i + 2 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    IrOp mop = ir_buf.data[w->ir_start + i + 2].u.op;
                    if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                        mop <= OP_FSTORE && be->mem_imm) {
                        i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)ins->u.w) : ins->u.w;
                        if (be->mem_imm(mop, off)) {
                            i += 2;
                            break;
                        }
                    }
                }
                if (i + 1 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    i32 imm = ins->u.w;
                    if (aop == OP_ADD || aop == OP_SUB || aop == OP_AADD || aop == OP_ASUB) {
                        i32 delta = (aop == OP_SUB || aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                        if (be->add_imm && be->add_imm(delta)) {
                            i++;
                            break;
                        }
                    } else if (aop >= OP_EQ && aop <= OP_UGE) {
                        if (be->cmp_imm && be->cmp_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    } else if (aop == OP_LSHIFT || aop == OP_RSHIFT || aop == OP_ARSHIFT ||
                               aop == OP_SHL32 || aop == OP_RSHR32 || aop == OP_ASHR32) {
                        u8 max_sh = (aop >= OP_SHL32 || be->word_bytes == 4) ? 31
                                    : (be->word_bytes == 8)                  ? 63
                                                                             : 15;
                        if (imm >= 0 && imm <= (i32)max_sh && be->shift_imm &&
                            be->shift_imm(aop, (u8)imm)) {
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
            case IR_PUSH_I32:
                be->push_i32(ins->u.w);
                break;
            case IR_PUSH_DATA:
                if (i + 1 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    be->data_mem &&
                    be->data_mem(ir_buf.data[w->ir_start + i + 1].u.op, ins->u.offset)) {
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
                    if (i + 3 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_PUSH_W &&
                        ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP &&
                        ir_buf.data[w->ir_start + i + 3].kind == IR_ALU_OP) {
                        IrOp aop = ir_buf.data[w->ir_start + i + 2].u.op;
                        IrOp mop = ir_buf.data[w->ir_start + i + 3].u.op;
                        if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                            mop <= OP_FSTORE) {
                            i32 imm = ir_buf.data[w->ir_start + i + 1].u.w;
                            i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                            if (be->local_mem(ins->u.loc.slot, off, mop, ins->u.loc.stack_bytes)) {
                                i += 3;
                                break;
                            }
                        }
                    } else if (i + 1 < w->ir_len &&
                               ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                        IrOp mop = ir_buf.data[w->ir_start + i + 1].u.op;
                        if (mop >= OP_I8FETCH && mop <= OP_FSTORE &&
                            be->local_mem(ins->u.loc.slot, 0, mop, ins->u.loc.stack_bytes)) {
                            i += 1;
                            break;
                        }
                    }
                }
                be->local_op(ins->u.loc.op, ins->u.loc.slot, ins->u.loc.type,
                             ins->u.loc.stack_bytes);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                be->op(ins->u.op);
                break;
            case IR_CALL: {
                size_t target = ins->u.func_idx;
                u16 call_addr = be->cur_addr();
                u16 call_pc = cb_pc();
                be->call(funcs.data[target]->addr);
                if (!funcs.data[target]->has_addr) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = call_pc;
                    fc->call_addr = call_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 0;
                }
                break;
            }
            case IR_PUSH_FN: {
                size_t target = ins->u.func_idx;
                u16 fn_addr = be->cur_addr();
                u16 fn_pc = be->push_fn(funcs.data[target]->addr);
                if (!funcs.data[target]->has_addr) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = fn_pc;
                    fc->call_addr = fn_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 1;
                }
                break;
            }
            case IR_ICALL:
                be->icall();
                break;
            case IR_CALL_EXTERN:
                be->call_extern(funcs.data[ins->u.func_idx]);
                break;
            case IR_BR_FWD: {
                PendingBr *pb = VEC_ADD(pending);
                pb->patch_offset = be->emit_br_fwd(ins->u.br.cond);
                pb->target_ir = ins->u.br.target;
                break;
            }
            case IR_BR_BACK:
                be->emit_br_back(ins->u.br.cond, ir_addr.data[ins->u.br.target]);
                break;
            }
        }
    }
    free(pending.data);
}

static void flush_vals_word(i32 val) {
    if (be->word_bytes == 2 && (val < -32768 || val > 65535)) {
        ir4_die(":i value out of range");
    }
    cb_emit_data_word(val, be->word_bytes);
}

static void emit_vals_string(void) {
    SrcFile *cur = &src_stack.data[src_stack.len - 1];
    for (;;) {
        int c = fgetc(cur->fp);
        if (c == EOF || c == '\n')
            ir4_die("unterminated string literal in 'data'");
        if (c == '"')
            return;
        if (c == '\\') {
            c = fgetc(cur->fp);
            if (c == EOF || c == '\n')
                ir4_die("unterminated escape in string literal");
            switch (c) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '0':
                c = '\0';
                break;
            case '\\':
                c = '\\';
                break;
            case '"':
                c = '"';
                break;
            default:
                ir4_die("unknown escape sequence in string literal");
            }
        }
        cb_emit_data_u8((u8)c);
    }
}

static void compile_data(void) {
    const char *tok;
    const char *name;
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;
    int ch;

    BufferSym *bs;
    name = next_tok_intern();
    if (!name) {
        ir4_die("expected name after 'data'");
    }
    if (find_buffer(name) >= 0) {
        ir4_die("duplicate global/data name");
    }
    bs = NEW(BufferSym);
    bs->name = name;
    bs->offset = cb_alloc_data(0, be->word_bytes);
    bs->type = TY_NONE;
    VEC_PUSH(buffers, bs);

    while ((ch = skip_ws_and_comments()) != EOF) {
        if (ch == '"') {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            emit_vals_string();
            continue;
        }
        ungetc(ch, src_stack.data[src_stack.len - 1].fp);
        tok = next_tok();
        if (!tok)
            break;
        if (strcmp(tok, "end") == 0) {
            if (has_pending)
                flush_vals_word(pending);
            return;
        } else if (strcmp(tok, "align") == 0) {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            cb_alloc_data(0, be->word_bytes);
        } else if (strcmp(tok, "&") == 0) {
            int idx;
            u16 patch_off;
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '&' in 'data'");
            patch_off = cb_data_size();
            idx = find_buffer(tok);
            if (idx >= 0) {
                cb_emit_data_word(0, be->addr_bytes);
                cb_add_data_init_reloc(patch_off, buffers.data[idx]->offset, be->addr_bytes, 0);
            } else {
                idx = ir4_find_func(tok);
                if (idx < 0)
                    ir4_die("unknown variable or function in '&' inside 'data'");
                if (funcs.data[idx]->is_extern)
                    ir4_die("cannot take address of 'extern' function in 'data'");
                cb_emit_data_word(0, be->addr_bytes);
                cb_add_data_init_reloc(patch_off, (u16)idx, be->addr_bytes, 1);
            }
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255)
                ir4_die(":i8 value out of range");
            cb_emit_data_u8((u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535)
                ir4_die(":i16 value out of range");
            cb_emit_data_u16((u16)(pending & 0xFFFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i32") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i32'");
            cb_emit_data_u16((u16)((u32)pending & 0xFFFFU));
            cb_emit_data_u16((u16)(((u32)pending >> 16) & 0xFFFFU));
            has_pending = 0;
        } else if (strcmp(tok, ":i") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i'");
            flush_vals_word(pending);
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending)
                flush_vals_word(pending);
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
    const char *tok;
    i32 num_val;
    const char *root_path = intern_str((path != NULL) ? path : "<stdin>");
    SrcFile *sf;

    src_stack.len = 0;
    sf = VEC_ADD(src_stack);
    sf->fp = in;
    sf->path = root_path;
    sf->line_num = 1;
    sf->req_group = 0;
    next_req_group = 1;

    seen_files.len = 0;
    VEC_PUSH(seen_files, root_path);

    be = target_be;
    platform_name = platform;
    funcs.len = 0;
    words.len = 0;
    buffers.len = 0;
    ir_buf.len = 0;
    raw_code_buf.len = 0;
    code_relocs.len = 0;
    fwd_calls.len = 0;
    init_funcs.len = 0;

    if (be->word_bytes == 2) {
        push_require_ex("i32.ir4", 1);
    }

    while ((tok = next_tok()) != NULL) {
        if (strcmp(tok, "fn") == 0) {
            compile_word(0);
        } else if (strcmp(tok, "default") == 0) {
            tok = next_tok();
            if (!tok || strcmp(tok, "fn") != 0) {
                ir4_die("expected 'fn' after 'default'");
            }
            compile_word(1);
        } else if (strcmp(tok, "code") == 0) {
            compile_code();
        } else if (strcmp(tok, "init") == 0) {
            compile_init();
        } else if (strcmp(tok, "fwd") == 0) {
            FuncSig *sig;
            const char *name = next_tok_intern();
            if (!name)
                ir4_die("expected word name after 'fwd'");
            if (ir4_find_func(name) >= 0)
                ir4_die("duplicate 'fwd' declaration");
            alloc_func(&sig, NULL);
            sig->name = name;
            tok = next_tok();
            if (!tok || strcmp(tok, "(") != 0)
                ir4_die("expected '(' after 'fwd' name");
            parse_sig_body(sig, NULL, 0);
        } else if (strcmp(tok, "extern") == 0) {
            FuncSig *sig;
            const char *name;
            alloc_func(&sig, NULL);
            sig->is_extern = 1;
            sig->is_defined = 1;
            name = next_tok_intern();
            if (!name)
                ir4_die("expected word name after 'extern'");
            sig->name = name;
            tok = next_tok();
            if (!tok)
                ir4_die("expected address or '(' after extern name");
            if (strcmp(tok, "(") == 0) {
                sig->has_addr = 0;
            } else if (parse_int(tok, &num_val)) {
                if (num_val < 0 || num_val > 0xFFFF)
                    ir4_die("invalid extern address");
                sig->addr = (u16)num_val;
                sig->has_addr = 1;
                tok = next_tok();
                if (!tok || strcmp(tok, "(") != 0) {
                    ir4_die("expected '(' after extern address");
                }
            } else {
                ir4_die("expected address or '(' after extern name");
            }
            parse_sig_body(sig, NULL, 0);
            if (sig->out_count > 1)
                ir4_die("extern supports at most 1 return value");
        } else if (strcmp(tok, "require") == 0) {
            push_require(read_require_path());
        } else if (strcmp(tok, "data") == 0) {
            compile_data();
        } else if (strcmp(tok, "var") == 0) {
            const char *name = next_tok_intern();
            u8 ty;
            BufferSym *bs;
            if (!name) {
                ir4_die("expected var name after 'var'");
            }
            if (find_buffer(name) >= 0) {
                ir4_die("duplicate global/data name");
            }
            bs = NEW(BufferSym);
            bs->name = name;
            tok = next_tok();
            if (!tok) {
                ir4_die("expected type (:i, :a, :f) or ':bytes' after var name");
            }
            if (strcmp(tok, ":bytes") == 0) {
                tok = next_tok();
                if (!tok || !parse_int(tok, &num_val) || num_val <= 0 || num_val > 0xFFFF) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                bs->offset = cb_alloc_data((u16)num_val, be->word_bytes);
                bs->type = TY_NONE;
            } else {
                u16 sz;
                ty = parse_type_tok(tok);
                if (ty == TY_NONE) {
                    ir4_die("expected ':i', ':a', ':f', or ':bytes' after var name");
                }
                sz = global_type_bytes(ty);
                bs->offset = cb_alloc_data(sz, sz);
                bs->type = ty;
            }
            VEC_PUSH(buffers, bs);
        } else {
            ir4_die("expected top-level 'fn', 'var', 'data', 'extern', or 'require'");
        }
    }

    /* Pass 1.5: Reachability / Dead-Code Elimination from 'main' */
    {
        int main_idx = ir4_find_func("main");
        size_t f;
        if (main_idx >= 0 && init_funcs.len > 0) {
            size_t wrap_idx;
            FuncSig *wsig;
            WordIR *ww;
            IrInstr *ins;
            size_t k;
            funcs.data[main_idx]->name = intern_str("__main_body");
            wrap_idx = alloc_func(&wsig, &ww);
            wsig->name = intern_str("main");
            wsig->is_defined = 1;
            wsig->out_count = funcs.data[main_idx]->out_count;
            memcpy(wsig->out_types, funcs.data[main_idx]->out_types, wsig->out_count);
            ww->ir_start = ir_buf.len;
            for (k = 0; k < init_funcs.len; k++) {
                ins = emit_ir(IR_CALL, ww);
                ins->u.func_idx = init_funcs.data[k];
            }
            ins = emit_ir(IR_CALL, ww);
            ins->u.func_idx = (size_t)main_idx;
            emit_ir(IR_FN_END, ww);
            main_idx = (int)wrap_idx;
        }
        if (main_idx >= 0) {
            u16 di;
            mark_reachable((size_t)main_idx);
            for (di = 0; di < cb_data_init_reloc_count(); di++) {
                u16 po, tv;
                u8 ab, is_c;
                cb_get_data_init_reloc(di, &po, &tv, &ab, &is_c);
                if (is_c)
                    mark_reachable((size_t)tv);
            }
        } else {
            for (f = 0; f < funcs.len; f++) {
                if (!funcs.data[f]->is_extern && funcs.data[f]->is_defined)
                    words.data[f]->reachable = 1;
            }
        }
        {
            u8 *visited = (u8 *)vec_calloc(funcs.len ? funcs.len : 1, 1);
            for (f = 0; f < funcs.len; f++) {
                if (words.data[f]->reachable) {
                    memset(visited, 0, funcs.len);
                    funcs.data[f]->is_recursive = can_reach_func(f, f, visited);
                }
            }
            free(visited);
        }

        if (be == &be_6502)
            (void)be_6502_stack_data_off();
        heap_start_off = cb_alloc_data(0, be->word_bytes);

        /* Pass 2: Emit reachable words and patch forward calls */
        for (f = 0; f < funcs.len; f++) {
            if (!funcs.data[f]->is_extern && words.data[f]->reachable) {
                emit_word(f);
            }
        }
        {
            size_t c;
            for (c = 0; c < fwd_calls.len; c++) {
                if (fwd_calls.data[c].is_fn_ptr) {
                    be->patch_fn(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                                 funcs.data[fwd_calls.data[c].func_idx]->addr);
                } else {
                    be->patch_call(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                                   funcs.data[fwd_calls.data[c].func_idx]->addr);
                }
            }
            for (c = 0; c < cb_data_init_reloc_count(); c++) {
                u16 po, tv;
                u8 ab, is_c;
                cb_get_data_init_reloc((u16)c, &po, &tv, &ab, &is_c);
                if (is_c) {
                    cb_patch_data_init_reloc((u16)c, funcs.data[tv]->addr);
                }
            }
        }
    }
}

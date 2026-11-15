#include "ir4c.h"

typedef struct {
    u8  kind; /* 1 = IF, 2 = BEGIN */
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

static FILE          *src_fp;
static const Backend *be;
static u16            line_num = 1;

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
    fprintf(stderr, "ir4c error (line %u): %s\n", line_num, msg);
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
        c = fgetc(src_fp);
        if (c == EOF) return 0;
        if (c == '\n') { line_num++; continue; }
        if (isspace((unsigned char)c)) continue;
        if (c == '\\') {
            while ((c = fgetc(src_fp)) != EOF && c != '\n') ;
            if (c == '\n') line_num++;
            continue;
        }
        break;
    }

    while (c != EOF && !isspace((unsigned char)c)) {
        if (len < MAX_TOK - 1) buf[len++] = (char)c;
        c = fgetc(src_fp);
    }
    if (c == '\n') line_num++;
    buf[len] = '\0';
    return 1;
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
        fprintf(stderr, "ir4c error (line %u): expected %s, got %s\n",
                line_num, type_name(expected), type_name(got));
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

static int handle_builtin(const char *tok) {
    u8 a, b;

    /* Stack manipulation */
    if (strcmp(tok, "dup") == 0) {
        a = tpop(); tpush(a); tpush(a);
        be->stack_op(OP_DUP, a, 0); return 1;
    }
    if (strcmp(tok, "drop") == 0) {
        a = tpop();
        be->stack_op(OP_DROP, a, 0); return 1;
    }
    if (strcmp(tok, "swap") == 0) {
        b = tpop(); a = tpop(); tpush(b); tpush(a);
        be->stack_op(OP_SWAP, b, a); return 1;
    }
    if (strcmp(tok, "over") == 0) {
        b = tpop(); a = tpop(); tpush(a); tpush(b); tpush(a);
        be->stack_op(OP_OVER, b, a); return 1;
    }

    /* Word ALU & bitwise: ( W: W: -- W: ) */
    if (strcmp(tok, "+") == 0)       { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_ADD); return 1; }
    if (strcmp(tok, "-") == 0)       { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_SUB); return 1; }
    if (strcmp(tok, "*") == 0)       { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_MUL); return 1; }
    if (strcmp(tok, "/") == 0)       { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_DIV); return 1; }
    if (strcmp(tok, "mod") == 0)     { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_MOD); return 1; }
    if (strcmp(tok, "u/") == 0)      { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_UDIV); return 1; }
    if (strcmp(tok, "umod") == 0)    { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_UMOD); return 1; }
    if (strcmp(tok, "and") == 0)     { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_AND); return 1; }
    if (strcmp(tok, "or") == 0)      { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_OR); return 1; }
    if (strcmp(tok, "xor") == 0)     { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_XOR); return 1; }
    if (strcmp(tok, "invert") == 0)  { tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_INVERT); return 1; }
    if (strcmp(tok, "lshift") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_LSHIFT); return 1; }
    if (strcmp(tok, "arshift") == 0) { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_ARSHIFT); return 1; }
    if (strcmp(tok, "rshift") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->alu_op(OP_RSHIFT); return 1; }

    /* Address arithmetic */
    if (strcmp(tok, "a+") == 0)      { tpop_expect(TY_W); tpop_expect(TY_A); tpush(TY_A); be->alu_op(OP_AADD); return 1; }
    if (strcmp(tok, "a-") == 0)      { tpop_expect(TY_W); tpop_expect(TY_A); tpush(TY_A); be->alu_op(OP_ASUB); return 1; }
    if (strcmp(tok, "adiff") == 0)   { tpop_expect(TY_A); tpop_expect(TY_A); tpush(TY_W); be->alu_op(OP_ADIFF); return 1; }

    /* Floating-point operations */
    if (strcmp(tok, "f+") == 0)      { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_F); be->alu_op(OP_FADD); return 1; }
    if (strcmp(tok, "f-") == 0)      { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_F); be->alu_op(OP_FSUB); return 1; }
    if (strcmp(tok, "f*") == 0)      { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_F); be->alu_op(OP_FMUL); return 1; }
    if (strcmp(tok, "f/") == 0)      { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_F); be->alu_op(OP_FDIV); return 1; }
    if (strcmp(tok, "fnegate") == 0) { tpop_expect(TY_F); tpush(TY_F); be->alu_op(OP_FNEG); return 1; }

    /* Comparisons */
    if (strcmp(tok, "=") == 0)   { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_EQ); return 1; }
    if (strcmp(tok, "<>") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_NE); return 1; }
    if (strcmp(tok, "<") == 0)   { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_LT); return 1; }
    if (strcmp(tok, ">") == 0)   { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_GT); return 1; }
    if (strcmp(tok, "<=") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_LE); return 1; }
    if (strcmp(tok, ">=") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_GE); return 1; }
    if (strcmp(tok, "u<") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_ULT); return 1; }
    if (strcmp(tok, "u>") == 0)  { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_UGT); return 1; }
    if (strcmp(tok, "u<=") == 0) { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_ULE); return 1; }
    if (strcmp(tok, "u>=") == 0) { tpop_expect(TY_W); tpop_expect(TY_W); tpush(TY_W); be->cmp_op(OP_UGE); return 1; }
    if (strcmp(tok, "a=") == 0)  { tpop_expect(TY_A); tpop_expect(TY_A); tpush(TY_W); be->cmp_op(OP_AEQ); return 1; }
    if (strcmp(tok, "a<>") == 0) { tpop_expect(TY_A); tpop_expect(TY_A); tpush(TY_W); be->cmp_op(OP_ANE); return 1; }
    if (strcmp(tok, "au<") == 0) { tpop_expect(TY_A); tpop_expect(TY_A); tpush(TY_W); be->cmp_op(OP_ALT); return 1; }
    if (strcmp(tok, "au>") == 0) { tpop_expect(TY_A); tpop_expect(TY_A); tpush(TY_W); be->cmp_op(OP_AGT); return 1; }
    if (strcmp(tok, "f=") == 0)  { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_W); be->cmp_op(OP_FEQ); return 1; }
    if (strcmp(tok, "f<>") == 0) { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_W); be->cmp_op(OP_FNE); return 1; }
    if (strcmp(tok, "f<") == 0)  { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_W); be->cmp_op(OP_FLT); return 1; }
    if (strcmp(tok, "f>") == 0)  { tpop_expect(TY_F); tpop_expect(TY_F); tpush(TY_W); be->cmp_op(OP_FGT); return 1; }

    /* Memory load/store */
    if (strcmp(tok, "i8@") == 0)  { tpop_expect(TY_A); tpush(TY_W); be->mem_op(OP_I8FETCH); return 1; }
    if (strcmp(tok, "u8@") == 0)  { tpop_expect(TY_A); tpush(TY_W); be->mem_op(OP_U8FETCH); return 1; }
    if (strcmp(tok, "i8!") == 0)  { tpop_expect(TY_A); tpop_expect(TY_W); be->mem_op(OP_I8STORE); return 1; }
    if (strcmp(tok, "i16@") == 0) { tpop_expect(TY_A); tpush(TY_W); be->mem_op(OP_I16FETCH); return 1; }
    if (strcmp(tok, "u16@") == 0) { tpop_expect(TY_A); tpush(TY_W); be->mem_op(OP_U16FETCH); return 1; }
    if (strcmp(tok, "i16!") == 0) { tpop_expect(TY_A); tpop_expect(TY_W); be->mem_op(OP_I16STORE); return 1; }
    if (strcmp(tok, "@") == 0)    { tpop_expect(TY_A); tpush(TY_W); be->mem_op(OP_FETCH); return 1; }
    if (strcmp(tok, "!") == 0)  { tpop_expect(TY_A); tpop_expect(TY_W); be->mem_op(OP_STORE); return 1; }
    if (strcmp(tok, "a@") == 0) { tpop_expect(TY_A); tpush(TY_A); be->mem_op(OP_AFETCH); return 1; }
    if (strcmp(tok, "a!") == 0) { tpop_expect(TY_A); tpop_expect(TY_A); be->mem_op(OP_ASTORE); return 1; }
    if (strcmp(tok, "f@") == 0) { tpop_expect(TY_A); tpush(TY_F); be->mem_op(OP_FFETCH); return 1; }
    if (strcmp(tok, "f!") == 0) { tpop_expect(TY_A); tpop_expect(TY_F); be->mem_op(OP_FSTORE); return 1; }

    /* Type conversions */
    if (strcmp(tok, "sext") == 0) { tpop_expect(TY_W); tpush(TY_W); be->conv_op(OP_SEXT); return 1; }
    if (strcmp(tok, "w>a") == 0)  { tpop_expect(TY_W); tpush(TY_A); be->conv_op(OP_W2A); return 1; }
    if (strcmp(tok, "a>w") == 0)  { tpop_expect(TY_A); tpush(TY_W); be->conv_op(OP_A2W); return 1; }
    if (strcmp(tok, "w>f") == 0)  { tpop_expect(TY_W); tpush(TY_F); be->conv_op(OP_W2F); return 1; }
    if (strcmp(tok, "f>w") == 0)  { tpop_expect(TY_F); tpush(TY_W); be->conv_op(OP_F2W); return 1; }

    return 0;
}

static void compile_word(void) {
    char tok[MAX_TOK];
    FuncSig *sig;
    u8 param_cnt = 0;
    u8 in_locals = 0;
    u8 in_outs = 0;
    u8 unreachable = 0;
    i32 num_val;

    if (func_count >= MAX_SYMS) ir4_die("too many word definitions");
    sig = &funcs[func_count++];
    memset(sig, 0, sizeof(FuncSig));

    if (!next_tok(sig->name)) ir4_die("expected word name after ':'");
    if (!next_tok(tok) || strcmp(tok, "{") != 0) ir4_die("expected '{' after word name");

    local_count = 0;
    cur_out_count = 0;
    tsp = 0;
    bsp = 0;

    /* Parse { inputs | locals -- outputs } */
    while (next_tok(tok)) {
        u8 ty;
        if (strcmp(tok, "}") == 0) break;
        if (strcmp(tok, "|") == 0) { in_locals = 1; continue; }
        if (strcmp(tok, "--") == 0) { in_outs = 1; continue; }

        ty = parse_type_tok(tok);
        if (ty == TY_NONE) ir4_die("expected type token (W:, A:, F:) in signature");

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
                sig->in_types[sig->in_count++] = ty;
                param_cnt++;
            }
            local_count++;
        }
    }

    sig->addr = be->cur_addr();
    be->fn_begin(sig->name, param_cnt, locals, local_count);

    /* Stream & type-check body */
    while (next_tok(tok)) {
        u8 ty;
        int idx;

        if (strcmp(tok, ";") == 0) {
            if (bsp != 0) ir4_die("unclosed control block at ';'");
            if (!unreachable) {
                check_return_stack();
                be->fn_end(sig, locals, local_count);
            }
            return;
        }

        if (strcmp(tok, "exit") == 0) {
            if (!unreachable) {
                check_return_stack();
                be->fn_end(sig, locals, local_count);
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
                be->push_a((u32)num_val);
            } else if (ty == TY_W) {
                if (!parse_int(tok, &num_val)) ir4_die("expected integer after W:");
                tpush(TY_W);
                be->push_w(num_val);
            } else {
                char *endp;
                double fval;
                if (tok[0] == '\0') ir4_die("expected float literal after F:");
                fval = strtod(tok, &endp);
                if (*endp != '\0') ir4_die("expected float literal after F:");
                tpush(TY_F);
                be->push_f(fval);
            }
            continue;
        }

        /* Bare integer literal defaults to W: */
        if (parse_int(tok, &num_val)) {
            tpush(TY_W);
            be->push_w(num_val);
            continue;
        }

        /* Local assignment: to <ident> */
        if (strcmp(tok, "to") == 0) {
            if (!next_tok(tok)) ir4_die("expected identifier after 'to'");
            idx = find_local(tok);
            if (idx < 0) ir4_die("unknown local in 'to'");
            tpop_expect(locals[idx].type);
            be->local_set(locals[idx].slot, locals[idx].type);
            continue;
        }

        /* Control flow */
        if (strcmp(tok, "if") == 0) {
            BlockInfo *b;
            if (bsp >= MAX_BLOCKS) ir4_die("block nesting overflow");
            tpop_expect(TY_W);
            b = &blocks[bsp++];
            b->kind = 1;
            b->has_else = 0;
            b->saved_unreach = unreachable;
            b->if_unreach = 0;
            b->saved_sp = tsp;
            memcpy(b->saved_stack, tstack, tsp);
            b->patch_else = be->emit_jz_fwd();
            continue;
        }
        if (strcmp(tok, "else") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != 1) ir4_die("'else' without 'if'");
            b = &blocks[bsp - 1];
            b->has_else = 1;
            b->if_unreach = unreachable;
            b->else_sp = tsp;
            memcpy(b->else_stack, tstack, tsp);
            tsp = b->saved_sp;
            memcpy(tstack, b->saved_stack, tsp);
            unreachable = b->saved_unreach;
            b->patch_end = be->emit_jmp_fwd();
            be->patch_fwd(b->patch_else);
            continue;
        }
        if (strcmp(tok, "then") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != 1) ir4_die("'then' without 'if'");
            b = &blocks[--bsp];
            if (!b->has_else) {
                if (!unreachable) {
                    if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                        ir4_die("unbalanced stack in 'if ... then'");
                }
                tsp = b->saved_sp;
                memcpy(tstack, b->saved_stack, tsp);
                unreachable = b->saved_unreach;
                be->patch_fwd(b->patch_else);
            } else {
                if (!b->if_unreach && !unreachable) {
                    if (tsp != b->else_sp || memcmp(tstack, b->else_stack, tsp) != 0)
                        ir4_die("branch stack mismatch between 'if' and 'else'");
                } else if (!b->if_unreach && unreachable) {
                    tsp = b->else_sp;
                    memcpy(tstack, b->else_stack, tsp);
                    unreachable = 0;
                }
                be->patch_fwd(b->patch_end);
            }
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            BlockInfo *b;
            if (bsp >= MAX_BLOCKS) ir4_die("block nesting overflow");
            b = &blocks[bsp++];
            b->kind = 2;
            b->loop_addr = be->cur_addr();
            b->saved_sp = tsp;
            memcpy(b->saved_stack, tstack, tsp);
            continue;
        }
        if (strcmp(tok, "while") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != 2) ir4_die("'while' without 'begin'");
            b = &blocks[bsp - 1];
            tpop_expect(TY_W);
            b->patch_end = be->emit_jz_fwd();
            continue;
        }
        if (strcmp(tok, "repeat") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != 2) ir4_die("'repeat' without 'begin'");
            b = &blocks[--bsp];
            if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                ir4_die("loop body must leave stack even at 'repeat'");
            be->emit_jmp_back(b->loop_addr);
            be->patch_fwd(b->patch_end);
            continue;
        }
        if (strcmp(tok, "until") == 0) {
            BlockInfo *b;
            if (bsp == 0 || blocks[bsp - 1].kind != 2) ir4_die("'until' without 'begin'");
            b = &blocks[--bsp];
            tpop_expect(TY_W);
            if (tsp != b->saved_sp || memcmp(tstack, b->saved_stack, tsp) != 0)
                ir4_die("loop body must leave stack even at 'until'");
            be->emit_jz_back(b->loop_addr);
            continue;
        }

        /* Core built-in word */
        if (handle_builtin(tok)) continue;

        /* Local variable read */
        idx = find_local(tok);
        if (idx >= 0) {
            tpush(locals[idx].type);
            be->local_get(locals[idx].slot, locals[idx].type);
            continue;
        }

        /* Buffer address */
        idx = find_buffer(tok);
        if (idx >= 0) {
            tpush(TY_A);
            be->push_data(buffers[idx].offset);
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
            be->call(callee->addr);
            continue;
        }

        fprintf(stderr, "ir4c error (line %u): unknown word '%s'\n", line_num, tok);
        exit(1);
    }

    ir4_die("unexpected EOF inside word definition (missing ';')");
}

void ir4_compile_stream(FILE *in, const Backend *target_be) {
    char tok[MAX_TOK];
    i32 num_val;
    src_fp = in;
    be = target_be;
    line_num = 1;
    func_count = 0;
    buffer_count = 0;

    while (next_tok(tok)) {
        if (strcmp(tok, ":") == 0) {
            compile_word();
        } else if (parse_int(tok, &num_val)) {
            if (num_val <= 0 || num_val > MAX_DATA) {
                ir4_die("invalid buffer size");
            }
            if (!next_tok(tok) || strcmp(tok, "buffer:") != 0) {
                ir4_die("expected 'buffer:' after top-level integer");
            }
            if (buffer_count >= MAX_SYMS) {
                ir4_die("too many buffers");
            }
            if (!next_tok(buffers[buffer_count].name)) {
                ir4_die("expected buffer name after 'buffer:'");
            }
            buffers[buffer_count].offset = cb_alloc_data((u16)num_val, be->word_bytes);
            buffer_count++;
        } else {
            ir4_die("expected top-level ':' or '<size> buffer:'");
        }
    }
}

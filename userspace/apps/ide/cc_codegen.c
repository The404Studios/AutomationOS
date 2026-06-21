/*
 * cc_codegen.c -- program / function / statement code generation for the
 *                 AutomationOS C-subset compiler backend.
 *
 * Walks the parsed C AST (see ide_ast.h) and emits Intel-syntax assembly text
 * restricted to the subset the native assembler (as_x64.c) accepts -- see the
 * "SUPPORTED ASSEMBLY SUBSET" block in tc.h. Expressions are handled by
 * cc_expr.c (cg_expr / cg_addr, result in RAX); type queries by cc_type.c.
 *
 * Freestanding: no libc, no malloc, no stdio. Every buffer is fixed-size and
 * bounded; recursion is depth-capped; anything unsupported funnels through
 * cc_error() so we degrade gracefully instead of crashing.
 *
 * GENERATED-code calling convention (self-consistent, SysV-like; see cc.h):
 *   args  rdi rsi rdx rcx r8 r9   (<=6)         return  rax
 *   frame  push rbp / mov rbp,rsp / sub rsp,frame ; locals at [rbp-off]
 *   rsp 16-byte aligned at each `call`.
 *   _start: call main ; mov rdi,rax ; mov rax,1 ; syscall   (exit = main ret)
 */
#include "cc.h"

/* ====================================================================== *
 *  tiny freestanding string / number helpers (file-local, self-contained)
 * ====================================================================== */

static int cc_strlen(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int cc_streq(const char* a, const char* b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* Copy up to cap-1 bytes of src into dst, always NUL-terminating. */
static void cc_strcpy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) {
        while (i < cap - 1 && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ====================================================================== *
 *  emit helpers -- the single choke-point for every byte of output.
 *  All bounded against g->cap; g->out stays NUL-terminated.
 * ====================================================================== */

/* Append a raw NUL-terminated run of bytes; silently truncates at capacity. */
static void cc_put(Cg* g, const char* s)
{
    int i = 0;
    if (!g || !g->out || g->cap <= 0 || !s) return;
    while (s[i]) {
        if (g->len >= g->cap - 1) {           /* keep room for terminating NUL */
            g->out[g->cap - 1] = '\0';
            g->ok = 0;                         /* output overflow -> not ok    */
            return;
        }
        g->out[g->len++] = s[i++];
    }
    g->out[g->len] = '\0';
}

static void cc_putc(Cg* g, char c)
{
    if (!g || !g->out || g->cap <= 0) return;
    if (g->len >= g->cap - 1) {
        g->out[g->cap - 1] = '\0';
        g->ok = 0;
        return;
    }
    g->out[g->len++] = c;
    g->out[g->len] = '\0';
}

/* Append the decimal form of n (handles negatives + LONG_MIN safely). */
static void cc_put_num(Cg* g, long n)
{
    char tmp[24];
    int  i = 0;
    unsigned long u;
    int  neg = 0;

    if (n < 0) { neg = 1; u = (unsigned long)(-(n + 1)) + 1UL; }  /* avoid UB on min */
    else         u = (unsigned long)n;

    /* build digits in reverse */
    do {
        tmp[i++] = (char)('0' + (int)(u % 10UL));
        u /= 10UL;
    } while (u && i < (int)sizeof(tmp) - 1);
    if (neg) cc_putc(g, '-');
    while (i > 0) cc_putc(g, tmp[--i]);
}

void cc_emit(Cg* g, const char* line)
{
    cc_put(g, line);
    cc_putc(g, '\n');
}

void cc_emit2(Cg* g, const char* a, const char* b)
{
    cc_put(g, a);
    cc_put(g, b);
    cc_putc(g, '\n');
}

void cc_emit_num(Cg* g, const char* a, long n, const char* b)
{
    cc_put(g, a);
    cc_put_num(g, n);
    cc_put(g, b);
    cc_putc(g, '\n');
}

int cc_new_label(Cg* g)
{
    if (!g) return 0;
    return ++g->label_id;
}

void cc_emit_labeldef(Cg* g, int n)
{
    cc_put(g, ".L");
    cc_put_num(g, (long)n);
    cc_put(g, ":");
    cc_putc(g, '\n');
}

CcLocal* cc_find_local(Cg* g, const char* name)
{
    int i;
    if (!g || !name) return 0;
    for (i = 0; i < g->nlocals; i++)
        if (cc_streq(g->locals[i].name, name))
            return &g->locals[i];
    return 0;
}

CcGlobal* cc_find_global(Cg* g, const char* name)
{
    int i;
    if (!g || !name) return 0;
    for (i = 0; i < g->nglobals; i++)
        if (cc_streq(g->globals[i].name, name))
            return &g->globals[i];
    return 0;
}

/* Intern a string literal's TEXT, deduping; returns a stable id (== index). */
int cc_intern_str(Cg* g, const char* text)
{
    int i;
    if (!g || !text) return -1;
    for (i = 0; i < g->nstrs; i++)
        if (cc_streq(g->strs[i].text, text))
            return g->strs[i].id;
    if (g->nstrs >= CC_MAXSTR) {
        cc_error(g, 0, "too many string literals");
        return -1;
    }
    {
        CcStr* s = &g->strs[g->nstrs];
        s->id = g->nstrs;
        cc_strcpy(s->text, (int)sizeof(s->text), text);
        s->len = cc_strlen(s->text);
        g->nstrs++;
        return s->id;
    }
}

void cc_error(Cg* g, int line, const char* msg)
{
    if (!g) return;
    g->ok = 0;
    if (g->diags && g->ndiags && *g->ndiags < TC_MAXDIAG) {
        TcDiag* d = &g->diags[*g->ndiags];
        d->line = line;
        cc_strcpy(d->msg, (int)sizeof(d->msg), msg ? msg : "error");
        (*g->ndiags)++;
    }
}

/* ====================================================================== *
 *  local-slot allocation
 * ====================================================================== */

#define CC_RECURSE_CAP 256

/* Register names for the first six integer/pointer arguments. */
static const char* const cc_argregs[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

/* Record a local at the next stack slot. off = -(index+1)*8. Returns slot. */
static CcLocal* cc_add_local(Cg* g, const char* name, const char* type_str)
{
    CcLocal* l;
    if (g->nlocals >= CC_MAXLOCALS) {
        cc_error(g, 0, "too many locals");
        return 0;
    }
    /* Skip anonymous / nameless decls -- nothing to address. */
    if (!name || !name[0])
        return 0;
    /* If already present (re-declared name in same fn scope), reuse the slot. */
    l = cc_find_local(g, name);
    if (l) return l;
    l = &g->locals[g->nlocals];
    cc_strcpy(l->name, (int)sizeof(l->name), name);
    l->size = cc_sizeof_type(type_str);          /* 1 for char/_Bool else 8 */
    l->off  = -((g->nlocals + 1) * 8);            /* 8 bytes/slot regardless */
    g->nlocals++;
    return l;
}

/* Recursively collect every AST_VAR_DECL in a subtree as a local. We descend
 * through all node kinds (blocks, loops, ifs) but stop at nested function
 * definitions so a function only owns its own locals. */
static void cc_collect_locals(Cg* g, const AstNode* n, int depth)
{
    const AstNode* c;
    if (!n || depth >= CC_RECURSE_CAP) return;

    if (n->kind == AST_VAR_DECL || n->kind == AST_DECL_STMT) {
        /* AST_DECL_STMT may itself wrap one or more AST_VAR_DECL children;
         * an AST_VAR_DECL carries the name directly. */
        if (n->kind == AST_VAR_DECL && n->name[0])
            cc_add_local(g, n->name, n->type_str);
    }

    for (c = n->first_child; c; c = c->next) {
        if (c->kind == AST_FUNC_DEF || c->kind == AST_FUNC_PROTO)
            continue;                            /* don't steal a nested fn's locals */
        cc_collect_locals(g, c, depth + 1);
    }
}

static int cc_round_up16(int v)
{
    if (v < 0) v = 0;
    return (v + 15) & ~15;
}

/* ====================================================================== *
 *  statement generation
 * ====================================================================== */

static void gen_stmt(Cg* g, const AstNode* n, int epilogue, int depth);

/* Store the value currently in RAX into a named local's stack slot. */
static void cc_store_local(Cg* g, CcLocal* l)
{
    if (!l) return;
    if (l->size == 1)
        cc_emit_num(g, "mov byte [rbp", (long)l->off, "], al");
    else
        cc_emit_num(g, "mov [rbp", (long)l->off, "], rax");
}

/* A declaration: if it has an initializer expression, evaluate -> rax, store. */
static void gen_var_decl(Cg* g, const AstNode* n)
{
    CcLocal* l;
    const AstNode* init;
    if (!n || !n->name[0]) return;
    l = cc_find_local(g, n->name);
    if (!l) {
        /* Was not pre-collected (e.g. cap exceeded). Best-effort: skip init. */
        cc_error(g, n->span.start_line, "decl has no stack slot");
        return;
    }
    init = n->first_child;                        /* initializer, if any */
    if (init) {
        cg_expr(g, init);                         /* value -> rax        */
        cc_store_local(g, l);
    }
}

static void gen_decl_stmt(Cg* g, const AstNode* n)
{
    const AstNode* c;
    if (!n) return;
    /* AST_DECL_STMT groups one-or-more declarators. Emit each var's init. */
    for (c = n->first_child; c; c = c->next) {
        if (c->kind == AST_VAR_DECL)
            gen_var_decl(g, c);
    }
}

static void gen_if(Cg* g, const AstNode* n, int epilogue, int depth)
{
    const AstNode* cond = n->first_child;
    const AstNode* then_s = cond ? cond->next : 0;
    const AstNode* else_s = then_s ? then_s->next : 0;
    int Lelse = cc_new_label(g);
    int Lend  = cc_new_label(g);

    cg_expr(g, cond);                             /* cond -> rax */
    cc_emit(g, "cmp rax, 0");
    cc_emit_num(g, "je .L", (long)Lelse, "");
    gen_stmt(g, then_s, epilogue, depth + 1);
    cc_emit_num(g, "jmp .L", (long)Lend, "");
    cc_emit_labeldef(g, Lelse);
    if (else_s)
        gen_stmt(g, else_s, epilogue, depth + 1);
    cc_emit_labeldef(g, Lend);
}

static void gen_while(Cg* g, const AstNode* n, int epilogue, int depth)
{
    const AstNode* cond = n->first_child;
    const AstNode* body = cond ? cond->next : 0;
    int Ltop = cc_new_label(g);
    int Lend = cc_new_label(g);
    int save_brk = g->break_label, save_cont = g->cont_label;

    cc_emit_labeldef(g, Ltop);
    cg_expr(g, cond);
    cc_emit(g, "cmp rax, 0");
    cc_emit_num(g, "je .L", (long)Lend, "");
    g->break_label = Lend;
    g->cont_label  = Ltop;
    gen_stmt(g, body, epilogue, depth + 1);
    cc_emit_num(g, "jmp .L", (long)Ltop, "");
    cc_emit_labeldef(g, Lend);
    g->break_label = save_brk;
    g->cont_label  = save_cont;
}

static void gen_for(Cg* g, const AstNode* n, int epilogue, int depth)
{
    /* AST_FOR children (subset): [init?] [cond?] [iter?] [body].  Parsers vary
     * in how they thread optional clauses, so we read up to four children and
     * treat the LAST as the body, the first three as init/cond/iter in order.
     * Empty clauses are represented by AST_EMPTY_STMT / AST_NONE and skipped. */
    const AstNode* kids[4] = { 0, 0, 0, 0 };
    const AstNode* c;
    int nk = 0;
    const AstNode *init = 0, *cond = 0, *iter = 0, *body = 0;
    int Ltop, Lend, Liter;
    int save_brk, save_cont;

    for (c = n->first_child; c && nk < 4; c = c->next)
        kids[nk++] = c;

    if (nk == 4)      { init = kids[0]; cond = kids[1]; iter = kids[2]; body = kids[3]; }
    else if (nk == 3) { init = kids[0]; cond = kids[1]; body = kids[2]; }
    else if (nk == 2) { cond = kids[0]; body = kids[1]; }
    else if (nk == 1) { body = kids[0]; }

    /* init */
    if (init) {
        if (init->kind == AST_DECL_STMT)      gen_decl_stmt(g, init);
        else if (init->kind == AST_VAR_DECL)  gen_var_decl(g, init);
        else if (init->kind == AST_EXPR_STMT) { if (init->first_child) cg_expr(g, init->first_child); }
        else if (init->kind != AST_EMPTY_STMT && init->kind != AST_NONE)
            cg_expr(g, init);
    }

    Ltop  = cc_new_label(g);
    Lend  = cc_new_label(g);
    Liter = cc_new_label(g);

    cc_emit_labeldef(g, Ltop);
    if (cond && cond->kind != AST_EMPTY_STMT && cond->kind != AST_NONE) {
        const AstNode* ce = (cond->kind == AST_EXPR_STMT) ? cond->first_child : cond;
        if (ce) {
            cg_expr(g, ce);
            cc_emit(g, "cmp rax, 0");
            cc_emit_num(g, "je .L", (long)Lend, "");
        }
    }

    save_brk = g->break_label; save_cont = g->cont_label;
    g->break_label = Lend;
    g->cont_label  = Liter;
    gen_stmt(g, body, epilogue, depth + 1);
    g->break_label = save_brk;
    g->cont_label  = save_cont;

    cc_emit_labeldef(g, Liter);
    if (iter && iter->kind != AST_EMPTY_STMT && iter->kind != AST_NONE) {
        const AstNode* ie = (iter->kind == AST_EXPR_STMT) ? iter->first_child : iter;
        if (ie) cg_expr(g, ie);
    }
    cc_emit_num(g, "jmp .L", (long)Ltop, "");
    cc_emit_labeldef(g, Lend);
}

/* AUDIT FIX (gap-org): switch/case/default codegen. The parser already emits
 * AST_SWITCH{sel,body}, AST_CASE{constexpr,stmt?}, AST_DEFAULT{stmt?}
 * (ide_pstmt.c) -- this lowers them to a linear compare-and-jump table, then the
 * body in source order so fall-through works; `break` exits via g->break_label.
 * Up to 32 cases (freestanding cc). */
static void gen_switch(Cg* g, const AstNode* n, int epilogue, int depth)
{
    const AstNode* sel = n->first_child;
    const AstNode* body = sel ? sel->next : 0;
    const AstNode* c;
    int Lend = cc_new_label(g);
    int Ldefault = -1;
    int save_brk = g->break_label;
    int case_count = 0;
    int case_labels[32];
    const AstNode* case_nodes[32];

    if (sel) cg_expr(g, sel);                      /* selector value -> rax */

    /* Pass 1: allocate a label per case (and the default) up front. */
    if (body && body->kind == AST_COMPOUND) {
        for (c = body->first_child; c; c = c->next) {
            if (c->kind == AST_CASE && case_count < 32) {
                case_labels[case_count] = cc_new_label(g);
                case_nodes[case_count] = c;
                case_count++;
            } else if (c->kind == AST_DEFAULT) {
                Ldefault = cc_new_label(g);
            }
        }
    }

    /* Pass 2: the dispatch -- cmp the selector against each constant case. */
    for (int i = 0; i < case_count; i++) {
        const AstNode* ce = case_nodes[i]->first_child;     /* the const-expr */
        long cv = 0;
        if (ce && cc_const_eval(ce, &cv)) {
            cc_emit(g, "mov rcx, rax");
            cc_emit_num(g, "cmp rcx, ", cv, "");
            cc_emit_num(g, "je .L", (long)case_labels[i], "");
        }
    }
    if (Ldefault < 0) Ldefault = Lend;
    cc_emit_num(g, "jmp .L", (long)Ldefault, "");

    /* Pass 3: emit the body in source order so fall-through is natural. */
    g->break_label = Lend;
    if (body && body->kind == AST_COMPOUND) {
        int idx = 0;
        for (c = body->first_child; c; c = c->next) {
            if (c->kind == AST_CASE && idx < case_count) {
                const AstNode* st = case_nodes[idx]->first_child;   /* const-expr */
                if (st) st = st->next;                              /* labeled stmt */
                cc_emit_labeldef(g, case_labels[idx]);
                if (st) gen_stmt(g, st, epilogue, depth + 1);
                idx++;
            } else if (c->kind == AST_DEFAULT) {
                cc_emit_labeldef(g, Ldefault);
                if (c->first_child) gen_stmt(g, c->first_child, epilogue, depth + 1);
            } else {
                gen_stmt(g, c, epilogue, depth + 1);
            }
        }
    }
    g->break_label = save_brk;
    cc_emit_labeldef(g, Lend);
}

static void gen_stmt(Cg* g, const AstNode* n, int epilogue, int depth)
{
    if (!n) return;
    if (depth >= CC_RECURSE_CAP) {
        cc_error(g, n->span.start_line, "statement nesting too deep");
        return;
    }
    if (!g->ok && g->len >= g->cap - 1) return;   /* output exhausted: stop */

    switch (n->kind) {

    case AST_COMPOUND: {
        const AstNode* c;
        for (c = n->first_child; c; c = c->next)
            gen_stmt(g, c, epilogue, depth + 1);
        return;
    }

    case AST_DECL_STMT:
        gen_decl_stmt(g, n);
        return;

    case AST_VAR_DECL:
        gen_var_decl(g, n);
        return;

    case AST_EXPR_STMT:
        if (n->first_child)
            cg_expr(g, n->first_child);            /* result in rax, discarded */
        return;

    case AST_RETURN:
        if (n->first_child)
            cg_expr(g, n->first_child);            /* return value -> rax */
        cc_emit_num(g, "jmp .L", (long)epilogue, "");
        return;

    case AST_IF:
        gen_if(g, n, epilogue, depth);
        return;

    case AST_WHILE:
        gen_while(g, n, epilogue, depth);
        return;

    case AST_FOR:
        gen_for(g, n, epilogue, depth);
        return;

    case AST_SWITCH:
        gen_switch(g, n, epilogue, depth);
        return;

    case AST_CASE:
    case AST_DEFAULT:
        /* Handled inside gen_switch; here means it appeared outside a switch. */
        cc_error(g, n->span.start_line, "case/default outside switch");
        return;

    case AST_BREAK:
        if (g->break_label < 0) { cc_error(g, n->span.start_line, "break outside loop"); return; }
        cc_emit_num(g, "jmp .L", (long)g->break_label, "");
        return;

    case AST_CONTINUE:
        if (g->cont_label < 0) { cc_error(g, n->span.start_line, "continue outside loop"); return; }
        cc_emit_num(g, "jmp .L", (long)g->cont_label, "");
        return;

    case AST_EMPTY_STMT:
    case AST_NONE:
        return;

    /* Loosely cover bare expressions/labels that show up where a statement is
     * expected; evaluate for side effects, discard the value. */
    case AST_BINARY:
    case AST_UNARY:
    case AST_ASSIGN:
    case AST_CALL:
    case AST_IDENT:
    case AST_LITERAL:
    case AST_INDEX:
    case AST_MEMBER:
    case AST_TERNARY:
    case AST_COMMA:
        cg_expr(g, n);
        return;

    default:
        cc_error(g, n->span.start_line, "unsupported statement");
        return;
    }
}

/* ====================================================================== *
 *  function emission
 * ====================================================================== */

/* Find the AST_COMPOUND body of a function def (its last/only block child). */
static const AstNode* cc_func_body(const AstNode* fn)
{
    const AstNode* c;
    const AstNode* body = 0;
    for (c = fn->first_child; c; c = c->next)
        if (c->kind == AST_COMPOUND)
            body = c;                              /* last compound = body */
    return body;
}

static void cc_gen_function(Cg* g, const AstNode* fn)
{
    const AstNode* c;
    const AstNode* body;
    int nparams = 0;
    int epilogue;

    if (!fn || !fn->name[0]) {
        cc_error(g, fn ? fn->span.start_line : 0, "function has no name");
        return;
    }

    /* ---- per-function reset ---- */
    g->nlocals     = 0;
    g->frame_size  = 0;
    g->break_label = -1;
    g->cont_label  = -1;

    /* ---- pre-pass: allocate stack slots ----
     * Params first (so param i lands at a known slot), then every VAR_DECL in
     * the body subtree. */
    for (c = fn->first_child; c; c = c->next) {
        if (c->kind == AST_PARAM) {
            if (c->name[0]) cc_add_local(g, c->name, c->type_str);
            nparams++;
        }
    }
    g->cur_param_count = nparams;

    body = cc_func_body(fn);
    if (body)
        cc_collect_locals(g, body, 0);

    g->frame_size = cc_round_up16(g->nlocals * 8);

    /* ---- label / prologue ---- */
    cc_emit2(g, fn->name, ":");
    cc_emit(g, "push rbp");
    cc_emit(g, "mov rbp, rsp");
    if (g->frame_size > 0)
        cc_emit_num(g, "sub rsp, ", (long)g->frame_size, "");

    /* ---- spill incoming params into their slots ---- */
    {
        int pi = 0;
        for (c = fn->first_child; c; c = c->next) {
            if (c->kind != AST_PARAM) continue;
            if (pi < 6 && c->name[0]) {
                CcLocal* l = cc_find_local(g, c->name);
                if (l) {
                    /* always store 8 bytes; slot is 8-wide even for char */
                    cc_put(g, "mov [rbp");
                    cc_put_num(g, (long)l->off);
                    cc_put(g, "], ");
                    cc_put(g, cc_argregs[pi]);
                    cc_putc(g, '\n');
                }
            }
            pi++;
        }
        if (nparams > 6)
            cc_error(g, fn->span.start_line, "more than 6 params (extra ignored)");
    }

    /* ---- unique epilogue label so AST_RETURN can jump to one exit ---- */
    epilogue = cc_new_label(g);

    /* ---- body ---- */
    if (body)
        gen_stmt(g, body, epilogue, 0);
    else
        cc_error(g, fn->span.start_line, "function has no body");

    /* ---- epilogue ---- */
    cc_emit_labeldef(g, epilogue);
    cc_emit(g, "leave");                           /* mov rsp,rbp ; pop rbp */
    cc_emit(g, "ret");
}

/* ====================================================================== *
 *  module globals + .data
 * ====================================================================== */

static void cc_collect_globals(Cg* g, const AstNode* tu)
{
    const AstNode* c;
    if (!tu) return;
    for (c = tu->first_child; c; c = c->next) {
        if (c->kind != AST_VAR_DECL) continue;
        if (!c->name[0]) continue;
        if (cc_find_global(g, c->name)) continue;
        if (g->nglobals >= CC_MAXGLOBALS) {
            cc_error(g, c->span.start_line, "too many globals");
            return;
        }
        {
            CcGlobal* gl = &g->globals[g->nglobals];
            cc_strcpy(gl->name, (int)sizeof(gl->name), c->name);
            cc_strcpy(gl->type, (int)sizeof(gl->type), c->type_str);
            gl->size    = cc_sizeof_type(c->type_str);
            gl->is_arr  = 0;
            gl->arrlen  = 0;
            /* Array detection: a '[' in the type string, e.g. "int [4]". */
            {
                const char* t = c->type_str;
                int j;
                for (j = 0; t[j]; j++) {
                    if (t[j] == '[') {
                        int len = 0, k = j + 1, have = 0;
                        while (t[k] && t[k] != ']') {
                            if (t[k] >= '0' && t[k] <= '9') {
                                len = len * 10 + (t[k] - '0');
                                have = 1;
                                /* Clamp before the next *10 so an oversized or
                                 * adversarial dimension can't overflow the
                                 * signed int (UB) or blow up the .data emit. */
                                if (len > CC_MAXARRLEN) len = CC_MAXARRLEN;
                            }
                            k++;
                        }
                        gl->is_arr = 1;
                        gl->arrlen = have ? len : 1;
                        break;
                    }
                }
            }
            /* B3/B4: capture the initializer. C requires file-scope inits to be
             * constant. A scalar folds to a value (B3); an array remembers its
             * brace init-list and grows arrlen to the element count when the
             * declared size was dropped to "[]" by the parser (B4). */
            gl->has_init = 0;
            gl->init_val = 0;
            gl->init = 0;
            if (c->first_child) {
                if (c->first_child->kind == AST_INIT_LIST) {
                    /* Aggregate brace-init (array OR struct). Keep the list; for
                     * an array, grow arrlen to the element count when the size
                     * was dropped to "[]". A struct's emission walks its layout
                     * (cc_struct_*), so it needs no arrlen. */
                    const AstNode* il = c->first_child;
                    gl->init = il;
                    if (gl->is_arr) {
                        int ninit = 0;
                        const AstNode* el;
                        for (el = il->first_child; el; el = el->next) ninit++;
                        if (gl->arrlen < ninit) gl->arrlen = ninit;
                        if (gl->arrlen > CC_MAXARRLEN) gl->arrlen = CC_MAXARRLEN;
                    }
                } else if (!gl->is_arr) {
                    long v;
                    if (cc_const_eval(c->first_child, &v)) {
                        gl->has_init = 1;
                        gl->init_val = v;
                    }
                }
            }
            g->nglobals++;
        }
    }
}

/* Emit a string literal's bytes as `db ...,0`. The interned text is the raw
 * source token, which may still carry surrounding double quotes -- we keep the
 * inner bytes and append a single NUL. Quotes/escapes are passed through as-is
 * to the assembler's `db` (which understands "str" runs). */
static void cc_emit_string_data(Cg* g, const CcStr* s)
{
    const char* t = s->text;
    int n = s->len;
    int has_open;

    cc_put(g, ".Lstr");
    cc_put_num(g, (long)s->id);
    cc_put(g, ":");
    cc_putc(g, '\n');

    /* If the interned text is a quoted literal ("..."), emit it verbatim as a
     * db string run then a NUL terminator. Otherwise wrap the bytes in quotes
     * defensively. */
    has_open = (n >= 1 && t[0] == '"');
    if (has_open) {
        cc_put(g, "db ");
        cc_put(g, t);                              /* includes the quotes */
        cc_put(g, ", 0");
        cc_putc(g, '\n');
    } else {
        cc_put(g, "db \"");
        cc_put(g, t);
        cc_put(g, "\", 0");
        cc_putc(g, '\n');
    }
}

/* CC-STRUCTINIT-0: emit one struct global's .data by its field LAYOUT -- each
 * field at its offset/size (db for 1-byte fields, dq for 8-byte), with alignment
 * gaps and any tail padded with `db 0`. gl->init (an AST_INIT_LIST) supplies
 * field values in declaration order; a missing or short list zero-fills the
 * rest. This keeps every field at the exact offset cc_member_offset reports, so
 * char/int mixes and packed-char runs read back correctly. */
static void cc_emit_struct_global(Cg* g, CcGlobal* gl)
{
    int nf    = cc_struct_nfields(gl->type);
    int total = cc_struct_size(gl->type);
    int cur = 0, i;
    const AstNode* el = gl->init ? gl->init->first_child : 0;

    for (i = 0; i < nf; i++) {
        int  off = 0, sz = 8;
        long v = 0;
        cc_struct_field(gl->type, i, &off, &sz);
        while (cur < off) { cc_emit(g, "db 0"); cur++; }   /* alignment gap */
        if (el) cc_const_eval(el, &v);                     /* 0 if non-constant */
        if (sz == 1) cc_emit_num(g, "db ", v & 0xFF, "");
        else         cc_emit_num(g, "dq ", v, "");
        cur += sz;
        if (el) el = el->next;
    }
    while (cur < total) { cc_emit(g, "db 0"); cur++; }     /* tail padding */
    if (nf == 0) cc_emit(g, "dq 0");                       /* defensive: unknown */
}

static void cc_emit_data_section(Cg* g)
{
    int i;
    if (g->nglobals == 0 && g->nstrs == 0)
        return;
    cc_emit(g, "section .data");

    for (i = 0; i < g->nglobals; i++) {
        CcGlobal* gl = &g->globals[i];
        cc_emit2(g, gl->name, ":");
        if (gl->is_arr) {
            int count = gl->arrlen > 0 ? gl->arrlen : 1;
            const AstNode* el = gl->init ? gl->init->first_child : 0;
            int k;
            for (k = 0; k < count; k++) {
                long v;
                if (el && cc_const_eval(el, &v))   /* B4: emit the init value */
                    cc_emit_num(g, "dq ", v, "");
                else
                    cc_emit(g, "dq 0");            /* unset elements zero-fill */
                if (el) el = el->next;
            }
        } else if (cc_struct_nfields(gl->type) > 0) {
            cc_emit_struct_global(g, gl);          /* CC-STRUCTINIT-0 */
        } else {
            if (gl->has_init)
                cc_emit_num(g, "dq ", gl->init_val, "");
            else
                cc_emit(g, "dq 0");
        }
    }

    for (i = 0; i < g->nstrs; i++)
        cc_emit_string_data(g, &g->strs[i]);
}

/* ====================================================================== *
 *  program driver
 * ====================================================================== */

int cc_gen_program(Cg* g, const AstNode* tu)
{
    const AstNode* c;
    const AstNode* first_func = 0;
    int have_main = 0;

    if (!g) return 0;
    if (!tu) { cc_error(g, 0, "empty translation unit"); return 0; }

    /* 1. module-scope globals */
    cc_collect_globals(g, tu);

    /* scout the function list for main / a fallback entry */
    for (c = tu->first_child; c; c = c->next) {
        if (c->kind != AST_FUNC_DEF) continue;
        if (!first_func) first_func = c;
        if (cc_streq(c->name, "main")) have_main = 1;
    }

    /* 2. .text + _start trampoline */
    cc_emit(g, "section .text");
    cc_emit(g, "global _start");
    cc_emit(g, "_start:");
    if (have_main) {
        cc_emit(g, "call main");
    } else if (first_func && first_func->name[0]) {
        cc_emit2(g, "call ", first_func->name);
    } else {
        cc_error(g, 0, "no function to call from _start");
        cc_emit(g, "mov rax, 0");                  /* degrade: exit(0) */
    }
    cc_emit(g, "mov rdi, rax");                     /* exit code = ret value */
    cc_emit(g, "mov rax, 0");                       /* SYS_EXIT (==0 on AutomationOS) */
    cc_emit(g, "syscall");

    /* 3. each function definition */
    for (c = tu->first_child; c; c = c->next) {
        if (c->kind == AST_FUNC_DEF)
            cc_gen_function(g, c);
    }

    /* 4. .data: globals then interned strings (cg_expr interns during step 3) */
    cc_emit_data_section(g);

    return g->ok;
}

/* ====================================================================== *
 *  top-level entry (tc.h) -- set up Cg state, drive, expose asm text.
 * ====================================================================== */

int cc_compile(const AstNode* tu, char* asm_out, int asm_cap,
               TcDiag* diags, int* ndiags)
{
    Cg g;
    int i;

    /* zero the state by hand (freestanding: no memset reliance) */
    {
        unsigned char* p = (unsigned char*)&g;
        for (i = 0; i < (int)sizeof(g); i++) p[i] = 0;
    }

    g.out         = asm_out;
    g.cap         = asm_cap;
    g.len         = 0;
    g.diags       = diags;
    g.ndiags      = ndiags;
    g.label_id    = 0;
    g.break_label = -1;
    g.cont_label  = -1;
    g.ok          = 1;
    if (g.out && g.cap > 0) g.out[0] = '\0';

    cc_gen_program(&g, tu);
    return g.ok;
}

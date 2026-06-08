/*
 * cc_expr.c -- expression code generator for the C-subset compiler backend.
 *
 *   Evaluates an AST expression to x86-64 (Intel-subset) assembly text, leaving
 *   the rvalue result in RAX; cg_addr leaves the LVALUE ADDRESS of a node in RAX.
 *
 * Register discipline (must match cc_codegen.c):
 *   - rax  = result of every (sub)expression.
 *   - the STACK carries the "first / left" operand while the "second / right"
 *     operand is being evaluated:  cg_expr(left); push rax; cg_expr(right);
 *     mov rcx,rax; pop rax;  then operate on (rax OP rcx).
 *   - rcx / rbx are scratch.
 *
 * Emits ONLY the instruction subset described in tc.h. Freestanding: no libc,
 * no malloc, no stdio. All string / number handling uses tiny static helpers
 * writing into bounded static buffers. Best-effort: unsupported nodes call
 * cc_error but never crash, and rsp is always kept balanced.
 */
#include "cc.h"

/* ---------------------------------------------------------------------- *
 *  bounded recursion guard
 * ---------------------------------------------------------------------- */
#define CE_MAX_DEPTH 64

/* ---------------------------------------------------------------------- *
 *  tiny freestanding string / number helpers (file-local, no libc)
 * ---------------------------------------------------------------------- */

static int ce_len(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Copy up to cap-1 bytes of src into dst, always NUL-terminating. */
static void ce_copy(char* dst, int cap, const char* src)
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

/* Append src to dst (cap), NUL-terminated, truncating safely. */
static void ce_cat(char* dst, int cap, const char* src)
{
    int n = ce_len(dst);
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) {
        while (n < cap - 1 && src[i]) {
            dst[n++] = src[i++];
        }
    }
    dst[n] = '\0';
}

/* Append a signed decimal of v to dst (cap). */
static void ce_cat_num(char* dst, int cap, long v)
{
    char tmp[24];
    int  ti = 0;
    unsigned long u;
    int  neg = 0;

    if (v < 0) { neg = 1; u = (unsigned long)(-(v + 1)) + 1UL; }
    else        u = (unsigned long)v;

    if (u == 0) tmp[ti++] = '0';
    while (u && ti < (int)sizeof(tmp)) {
        tmp[ti++] = (char)('0' + (int)(u % 10UL));
        u /= 10UL;
    }
    {
        char buf[26];
        int  bi = 0;
        if (neg) buf[bi++] = '-';
        while (ti > 0) buf[bi++] = tmp[--ti];
        buf[bi] = '\0';
        ce_cat(dst, cap, buf);
    }
}

/* Compare a NUL-terminated string against a literal; 1 if exactly equal. */
static int ce_eq(const char* a, const char* b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* Is this type-string char-sized (1 byte)? (drives byte vs qword loads/stores) */
static int ce_is_byte_type(const char* ty)
{
    return cc_sizeof_type(ty) == 1;
}

/*
 * Parse a C literal's text (as stored in node->name) into its numeric value.
 *   - decimal:        123
 *   - hex:            0x1A / 0X1a
 *   - octal:          0755 (leading 0)
 *   - char literal:   'x', '\n', '\t', '\0', '\\', '\'' ...
 * String literals are NOT handled here (the caller interns them instead).
 */
static long ce_parse_num(const char* s)
{
    long v = 0;
    int  neg = 0;
    const char* p = s;

    if (!p) return 0;

    /* character literal: 'c' or '\x' */
    if (p[0] == '\'') {
        char c = p[1];
        if (c == '\\') {
            switch (p[2]) {
            case 'n':  return '\n';
            case 't':  return '\t';
            case 'r':  return '\r';
            case '0':  return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            case '"':  return '"';
            case 'b':  return '\b';
            case 'f':  return '\f';
            case 'v':  return '\v';
            case 'a':  return '\a';
            default:   return (unsigned char)p[2];
            }
        }
        return (unsigned char)c;
    }

    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    /* hex */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            char c = *p;
            int  d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
            else break;
            v = v * 16 + d;
            p++;
        }
        return neg ? -v : v;
    }

    /* octal */
    if (p[0] == '0' && p[1] >= '0' && p[1] <= '7') {
        p++;
        while (*p >= '0' && *p <= '7') {
            v = v * 8 + (*p - '0');
            p++;
        }
        return neg ? -v : v;
    }

    /* decimal */
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return neg ? -v : v;
}

/* ---------------------------------------------------------------------- *
 *  forward decls
 * ---------------------------------------------------------------------- */
static void ce_expr(Cg* g, const AstNode* e, int depth);
static void ce_addr(Cg* g, const AstNode* e, int depth);

/* nth child of e (0-based), or NULL. */
static const AstNode* ce_child(const AstNode* e, int n)
{
    const AstNode* c;
    if (!e) return 0;
    c = e->first_child;
    while (c && n > 0) { c = c->next; n--; }
    return (n == 0) ? c : 0;
}

/* Load through the address currently in rax, using the inferred type of e to
 * pick a byte (movzx) vs qword (mov) load. */
static void ce_load_rax(Cg* g, const AstNode* e)
{
    char ty[CC_NAME];
    cc_infer_type(g, e, ty, (int)sizeof(ty));
    if (ce_is_byte_type(ty))
        cc_emit(g, "movzx rax, byte [rax]");
    else
        cc_emit(g, "mov rax, [rax]");
}

/* ---------------------------------------------------------------------- *
 *  cg_addr -- LVALUE ADDRESS of e -> rax
 * ---------------------------------------------------------------------- */
static void ce_addr(Cg* g, const AstNode* e, int depth)
{
    if (!e) { cc_error(g, 0, "null node in cg_addr"); return; }
    if (depth >= CE_MAX_DEPTH) {
        cc_error(g, e->span.start_line, "expression too deeply nested");
        cc_emit(g, "mov rax, 0");
        return;
    }

    switch (e->kind) {

    case AST_IDENT: {
        CcLocal* loc = cc_find_local(g, e->name);
        if (loc) {
            /* lea rax, [rbp-off]   (off is stored as -(bytes from rbp)) */
            char line[CC_NAME + 24];
            int  disp = -loc->off;           /* positive distance below rbp */
            ce_copy(line, (int)sizeof(line), "lea rax, [rbp-");
            ce_cat_num(line, (int)sizeof(line), (long)disp);
            ce_cat(line, (int)sizeof(line), "]");
            cc_emit(g, line);
            return;
        }
        if (cc_find_global(g, e->name)) {
            /* absolute address of the global label as imm64 */
            cc_emit2(g, "mov rax, ", e->name);
            return;
        }
        cc_error(g, e->span.start_line, "unknown identifier (lvalue)");
        cc_emit(g, "mov rax, 0");
        return;
    }

    case AST_UNARY: {
        const char* op = e->name;
        if (op && op[0] == '*' && op[1] == '\0') {
            /* &(*p) == p : the pointer value IS the address. */
            ce_expr(g, ce_child(e, 0), depth + 1);
            return;
        }
        cc_error(g, e->span.start_line, "unary expr is not an lvalue");
        cc_emit(g, "mov rax, 0");
        return;
    }

    case AST_INDEX: {
        /* a[i] : address = base_ptr + index * elem_size */
        const AstNode* base = ce_child(e, 0);
        const AstNode* idx  = ce_child(e, 1);
        char ty[CC_NAME];
        int  esz;

        cc_infer_type(g, base, ty, (int)sizeof(ty));
        esz = cc_elem_size(ty);
        if (esz <= 0) esz = 1;

        ce_expr(g, base, depth + 1);          /* base ptr -> rax */
        cc_emit(g, "push rax");               /* save base */
        ce_expr(g, idx, depth + 1);           /* index -> rax */
        if (esz != 1)
            cc_emit_num(g, "imul rax, ", (long)esz, "");
        cc_emit(g, "pop rbx");                /* base -> rbx */
        cc_emit(g, "add rax, rbx");           /* base + index*elem -> rax */
        return;
    }

    case AST_MEMBER: {
        /* s.field  : address = &s + offset       (base is a struct lvalue)
         * p->field : address =  p + offset       (base is a pointer value)
         * Resolve the base's struct type, look the field offset up in the
         * struct registry, then add it to the base address. */
        const AstNode* base = ce_child(e, 0);
        const char* ops = e->type_str;            /* "." or "->" */
        int is_arrow = (ops && ops[0] == '-' && ops[1] == '>');
        char bty[CC_NAME];
        int  off, fsz = 8;

        /* base object's struct type (peel one '*' for the '->' case). */
        cc_infer_type(g, base, bty, (int)sizeof(bty));
        if (is_arrow) {
            int n = ce_len(bty), i = n - 1;
            while (i >= 0 && (bty[i] == ' ' || bty[i] == '\t')) bty[i--] = '\0';
            if (i >= 0 && bty[i] == '*') {        /* strip one level of indir */
                bty[i] = '\0';
                i--;
                while (i >= 0 && (bty[i] == ' ' || bty[i] == '\t')) bty[i--] = '\0';
            }
        }

        off = cc_member_offset(bty, e->name, &fsz);

        if (is_arrow)
            ce_expr(g, base, depth + 1);          /* pointer VALUE -> rax */
        else
            ce_addr(g, base, depth + 1);          /* &struct        -> rax */

        if (off != 0)
            cc_emit_num(g, "add rax, ", (long)off, "");
        return;
    }

    default:
        cc_error(g, e->span.start_line, "not an lvalue");
        cc_emit(g, "mov rax, 0");
        return;
    }
}

void cg_addr(Cg* g, const AstNode* e)
{
    ce_addr(g, e, 0);
}

/* ---------------------------------------------------------------------- *
 *  comparison / setcc emit
 * ---------------------------------------------------------------------- */

/* Given a comparison operator string, return the setcc mnemonic, or NULL. */
static const char* ce_setcc_for(const char* op)
{
    if (ce_eq(op, "==")) return "sete al";
    if (ce_eq(op, "!=")) return "setne al";
    if (ce_eq(op, "<"))  return "setl al";
    if (ce_eq(op, "<=")) return "setle al";
    if (ce_eq(op, ">"))  return "setg al";
    if (ce_eq(op, ">=")) return "setge al";
    return 0;
}

/* ---------------------------------------------------------------------- *
 *  compound-assignment / inc-dec op application
 *
 *  Applies a simple binary op to (rax OP rcx), with optional pointer scaling
 *  of rcx for + / -.  Used by both AST_ASSIGN compound forms and ++/--.
 *  `ptr_elem` is the element size to scale rcx by for +/- (1 = no scaling).
 * ---------------------------------------------------------------------- */
static void ce_apply_binop(Cg* g, const char* op, int ptr_elem)
{
    if (ce_eq(op, "+")) {
        if (ptr_elem != 1) cc_emit_num(g, "imul rcx, ", (long)ptr_elem, "");
        cc_emit(g, "add rax, rcx");
    } else if (ce_eq(op, "-")) {
        if (ptr_elem != 1) cc_emit_num(g, "imul rcx, ", (long)ptr_elem, "");
        cc_emit(g, "sub rax, rcx");
    } else if (ce_eq(op, "*")) {
        cc_emit(g, "imul rax, rcx");
    } else if (ce_eq(op, "/")) {
        cc_emit(g, "cqo");
        cc_emit(g, "idiv rcx");
    } else if (ce_eq(op, "%")) {
        cc_emit(g, "cqo");
        cc_emit(g, "idiv rcx");
        cc_emit(g, "mov rax, rdx");
    } else if (ce_eq(op, "&")) {
        cc_emit(g, "and rax, rcx");
    } else if (ce_eq(op, "|")) {
        cc_emit(g, "or rax, rcx");
    } else if (ce_eq(op, "^")) {
        cc_emit(g, "xor rax, rcx");
    } else if (ce_eq(op, "<<")) {
        cc_emit(g, "shl rax, cl");
    } else if (ce_eq(op, ">>")) {
        cc_emit(g, "shr rax, cl");
    } else {
        cc_error(g, 0, "unsupported compound operator");
    }
}

/* Map a compound-assign operator ("+=") to its base op ("+"); NULL if plain. */
static const char* ce_compound_base(const char* op)
{
    if (ce_eq(op, "+="))  return "+";
    if (ce_eq(op, "-="))  return "-";
    if (ce_eq(op, "*="))  return "*";
    if (ce_eq(op, "/="))  return "/";
    if (ce_eq(op, "%="))  return "%";
    if (ce_eq(op, "&="))  return "&";
    if (ce_eq(op, "|="))  return "|";
    if (ce_eq(op, "^="))  return "^";
    if (ce_eq(op, "<<=")) return "<<";
    if (ce_eq(op, ">>=")) return ">>";
    return 0;
}

/* Store rax through the address currently in rcx, byte vs qword by lvalue type. */
static void ce_store_to_rcx(Cg* g, const AstNode* lhs)
{
    char ty[CC_NAME];
    cc_infer_type(g, lhs, ty, (int)sizeof(ty));
    if (ce_is_byte_type(ty))
        cc_emit(g, "mov byte [rcx], al");
    else
        cc_emit(g, "mov [rcx], rax");
}

/* ---------------------------------------------------------------------- *
 *  function-call argument placement
 *
 *  Evaluate each arg to rax and push; after all are evaluated, pop them into
 *  the SysV arg registers in order. We pop in reverse push order, so we push
 *  args left-to-right then pop into reversed register slots to land each arg in
 *  the right place. 16-byte rsp alignment is restored at the `call`.
 * ---------------------------------------------------------------------- */
static const char* const CE_ARG_REGS[6] = {
    "rdi", "rsi", "rdx", "rcx", "r8", "r9"
};

/* Pop into reg: "pop <reg>". */
static void ce_pop_into(Cg* g, const char* reg)
{
    cc_emit2(g, "pop ", reg);
}

/* ---------------------------------------------------------------------- *
 *  cg_expr -- evaluate rvalue of e -> rax
 * ---------------------------------------------------------------------- */
static void ce_expr(Cg* g, const AstNode* e, int depth)
{
    if (!e) { cc_error(g, 0, "null node in cg_expr"); cc_emit(g, "mov rax, 0"); return; }
    if (depth >= CE_MAX_DEPTH) {
        cc_error(g, e->span.start_line, "expression too deeply nested");
        cc_emit(g, "mov rax, 0");
        return;
    }

    switch (e->kind) {

    /* -------------------------------------------------- literals */
    case AST_LITERAL: {
        const char* t = e->name;
        if (t && t[0] == '"') {
            int id = cc_intern_str(g, t);
            char label[24];
            ce_copy(label, (int)sizeof(label), ".Lstr");
            ce_cat_num(label, (int)sizeof(label), (long)id);
            cc_emit2(g, "mov rax, ", label);    /* absolute address of string */
        } else {
            long v = ce_parse_num(t);
            cc_emit_num(g, "mov rax, ", v, "");
        }
        return;
    }

    /* -------------------------------------------------- lvalue rvalues */
    case AST_IDENT:
    case AST_INDEX:
    case AST_MEMBER: {
        ce_addr(g, e, depth);                   /* address -> rax */
        ce_load_rax(g, e);                      /* load value -> rax */
        return;
    }

    /* -------------------------------------------------- unary */
    case AST_UNARY: {
        const char* op = e->name;
        const AstNode* ch = ce_child(e, 0);

        if (!op) { cc_error(g, e->span.start_line, "bad unary op"); cc_emit(g, "mov rax, 0"); return; }

        /* prefix / postfix increment & decrement */
        if (ce_eq(op, "++") || ce_eq(op, "--") ||
            ce_eq(op, "post++") || ce_eq(op, "post--")) {
            int is_post = (op[0] == 'p');
            int is_inc  = (op[is_post ? 4 : 0] == '+');
            char ty[CC_NAME];
            int  step;

            cc_infer_type(g, ch, ty, (int)sizeof(ty));
            step = cc_is_pointer(ty) ? cc_elem_size(ty) : 1;
            if (step <= 0) step = 1;

            ce_addr(g, ch, depth + 1);          /* &lvalue -> rax */
            cc_emit(g, "push rax");             /* save address */
            ce_load_rax(g, ch);                 /* current value -> rax */
            cc_emit(g, "pop rcx");              /* address -> rcx */
            if (is_post) {
                /* result = OLD value: stash old (rax) on stack, compute new */
                cc_emit(g, "push rax");         /* save old value */
                if (is_inc) cc_emit_num(g, "add rax, ", (long)step, "");
                else        cc_emit_num(g, "sub rax, ", (long)step, "");
                ce_store_to_rcx(g, ch);         /* store new value */
                cc_emit(g, "pop rax");          /* restore old value as result */
            } else {
                /* result = NEW value */
                if (is_inc) cc_emit_num(g, "add rax, ", (long)step, "");
                else        cc_emit_num(g, "sub rax, ", (long)step, "");
                ce_store_to_rcx(g, ch);         /* store new value */
                /* rax already holds the new value */
            }
            return;
        }

        if (ce_eq(op, "-")) {
            ce_expr(g, ch, depth + 1);
            cc_emit(g, "neg rax");
            return;
        }
        if (ce_eq(op, "+")) {
            ce_expr(g, ch, depth + 1);          /* unary plus: no-op */
            return;
        }
        if (ce_eq(op, "~")) {
            ce_expr(g, ch, depth + 1);
            cc_emit(g, "not rax");
            return;
        }
        if (ce_eq(op, "!")) {
            ce_expr(g, ch, depth + 1);
            cc_emit(g, "cmp rax, 0");
            cc_emit(g, "sete al");
            cc_emit(g, "movzx rax, al");
            return;
        }
        if (ce_eq(op, "&")) {
            ce_addr(g, ch, depth + 1);          /* address-of */
            return;
        }
        if (ce_eq(op, "*")) {
            ce_expr(g, ch, depth + 1);          /* pointer value -> rax (= addr) */
            ce_load_rax(g, e);                  /* load element (byte/qword by type) */
            return;
        }

        cc_error(g, e->span.start_line, "unsupported unary operator");
        cc_emit(g, "mov rax, 0");
        return;
    }

    /* -------------------------------------------------- binary */
    case AST_BINARY: {
        const char* op = e->name;
        const AstNode* l = ce_child(e, 0);
        const AstNode* r = ce_child(e, 1);

        if (!op) { cc_error(g, e->span.start_line, "bad binary op"); cc_emit(g, "mov rax, 0"); return; }

        /* short-circuit && and || */
        if (ce_eq(op, "&&")) {
            int lfalse = cc_new_label(g);
            int ldone  = cc_new_label(g);
            ce_expr(g, l, depth + 1);
            cc_emit(g, "cmp rax, 0");
            cc_emit_num(g, "je .L", (long)lfalse, "");
            ce_expr(g, r, depth + 1);
            cc_emit(g, "cmp rax, 0");
            cc_emit_num(g, "je .L", (long)lfalse, "");
            cc_emit(g, "mov rax, 1");
            cc_emit_num(g, "jmp .L", (long)ldone, "");
            cc_emit_labeldef(g, lfalse);
            cc_emit(g, "mov rax, 0");
            cc_emit_labeldef(g, ldone);
            return;
        }
        if (ce_eq(op, "||")) {
            int ltrue = cc_new_label(g);
            int ldone = cc_new_label(g);
            ce_expr(g, l, depth + 1);
            cc_emit(g, "cmp rax, 0");
            cc_emit_num(g, "jne .L", (long)ltrue, "");
            ce_expr(g, r, depth + 1);
            cc_emit(g, "cmp rax, 0");
            cc_emit_num(g, "jne .L", (long)ltrue, "");
            cc_emit(g, "mov rax, 0");
            cc_emit_num(g, "jmp .L", (long)ldone, "");
            cc_emit_labeldef(g, ltrue);
            cc_emit(g, "mov rax, 1");
            cc_emit_labeldef(g, ldone);
            return;
        }

        /* general: left -> rax (push), right -> rcx, left popped to rax */
        ce_expr(g, l, depth + 1);
        cc_emit(g, "push rax");                 /* save left operand */
        ce_expr(g, r, depth + 1);
        cc_emit(g, "mov rcx, rax");             /* right -> rcx */
        cc_emit(g, "pop rax");                  /* left -> rax */

        /* comparisons */
        {
            const char* sc = ce_setcc_for(op);
            if (sc) {
                cc_emit(g, "cmp rax, rcx");
                cc_emit(g, sc);
                cc_emit(g, "movzx rax, al");
                return;
            }
        }

        /* + / - with pointer scaling: if the LEFT operand is a pointer, scale
         * the integer right operand (rcx) by the element size. */
        if (ce_eq(op, "+") || ce_eq(op, "-")) {
            char lty[CC_NAME];
            int  esz = 1;
            cc_infer_type(g, l, lty, (int)sizeof(lty));
            if (cc_is_pointer(lty)) {
                esz = cc_elem_size(lty);
                if (esz <= 0) esz = 1;
            }
            ce_apply_binop(g, op, esz);
            return;
        }

        /* arithmetic / bitwise / shift */
        ce_apply_binop(g, op, 1);
        return;
    }

    /* -------------------------------------------------- assignment */
    case AST_ASSIGN: {
        const char* op  = e->name;
        const AstNode* lhs = ce_child(e, 0);
        const AstNode* rhs = ce_child(e, 1);
        const char* base = op ? ce_compound_base(op) : 0;

        if (!op) { cc_error(g, e->span.start_line, "bad assign op"); cc_emit(g, "mov rax, 0"); return; }

        if (base == 0 && ce_eq(op, "=")) {
            /* plain assignment: store rhs through &lhs */
            ce_addr(g, lhs, depth + 1);         /* &lhs -> rax */
            cc_emit(g, "push rax");             /* save address */
            ce_expr(g, rhs, depth + 1);         /* rhs value -> rax */
            cc_emit(g, "pop rcx");              /* address -> rcx */
            ce_store_to_rcx(g, lhs);            /* store; value stays in rax */
            return;
        }

        if (base) {
            /* compound assignment: lhs = lhs OP rhs */
            char lty[CC_NAME];
            int  esz = 1;
            cc_infer_type(g, lhs, lty, (int)sizeof(lty));
            if ((ce_eq(base, "+") || ce_eq(base, "-")) && cc_is_pointer(lty)) {
                esz = cc_elem_size(lty);
                if (esz <= 0) esz = 1;
            }

            ce_addr(g, lhs, depth + 1);         /* &lhs -> rax */
            cc_emit(g, "push rax");             /* save address */
            ce_load_rax(g, lhs);                /* current lhs value -> rax */
            cc_emit(g, "push rax");             /* save current value */
            ce_expr(g, rhs, depth + 1);         /* rhs -> rax */
            cc_emit(g, "mov rcx, rax");         /* rhs -> rcx (second operand) */
            cc_emit(g, "pop rax");              /* current lhs value -> rax */
            ce_apply_binop(g, base, esz);       /* rax = rax OP rcx -> rax */
            cc_emit(g, "pop rcx");              /* &lhs -> rcx */
            ce_store_to_rcx(g, lhs);            /* store result; value in rax */
            return;
        }

        cc_error(g, e->span.start_line, "unsupported assignment operator");
        cc_emit(g, "mov rax, 0");
        return;
    }

    /* -------------------------------------------------- ternary */
    case AST_TERNARY: {
        const AstNode* cond = ce_child(e, 0);
        const AstNode* then_e = ce_child(e, 1);
        const AstNode* else_e = ce_child(e, 2);
        int lelse = cc_new_label(g);
        int ldone = cc_new_label(g);

        ce_expr(g, cond, depth + 1);
        cc_emit(g, "cmp rax, 0");
        cc_emit_num(g, "je .L", (long)lelse, "");
        ce_expr(g, then_e, depth + 1);
        cc_emit_num(g, "jmp .L", (long)ldone, "");
        cc_emit_labeldef(g, lelse);
        ce_expr(g, else_e, depth + 1);
        cc_emit_labeldef(g, ldone);
        return;
    }

    /* -------------------------------------------------- call */
    case AST_CALL: {
        const char* callee = e->name;            /* simple ident name, or "" */
        const AstNode* base = ce_child(e, 0);     /* child0 = callee */

        /* gather argument nodes (children after the callee) */
        const AstNode* args[6];
        int nargs = 0;
        {
            const AstNode* a = base ? base->next : 0;
            while (a && nargs < 6) {
                args[nargs++] = a;
                a = a->next;
            }
            /* drain (count) any beyond 6 so we can warn but stay bounded */
            if (a) cc_error(g, e->span.start_line, "more than 6 args (extra ignored)");
        }

        /* ---- builtins: sys_write(fd,buf,len) / sys_exit(code) ---- */
        if (callee && ce_eq(callee, "sys_write")) {
            /* evaluate the three args, each to rax, push, then pop into regs */
            int i;
            int n = nargs < 3 ? nargs : 3;
            for (i = 0; i < n; i++) {
                ce_expr(g, args[i], depth + 1);
                cc_emit(g, "push rax");
            }
            /* pop in reverse: last pushed (arg n-1) first */
            for (i = n - 1; i >= 0; i--) {
                /* arg0->rdi, arg1->rsi, arg2->rdx */
                ce_pop_into(g, (i == 0) ? "rdi" : (i == 1) ? "rsi" : "rdx");
            }
            cc_emit(g, "mov rax, 3");            /* SYS_WRITE */
            cc_emit(g, "syscall");
            return;
        }
        if (callee && ce_eq(callee, "sys_exit")) {
            if (nargs >= 1) {
                ce_expr(g, args[0], depth + 1);
                cc_emit(g, "mov rdi, rax");
            } else {
                cc_emit(g, "mov rdi, 0");
            }
            cc_emit(g, "mov rax, 0");            /* SYS_EXIT (==0 on AutomationOS) */
            cc_emit(g, "syscall");
            return;
        }
        /* ---- generic syscall builtin: syscall(n[, a1, a2, a3]) ----
         * Lets a SELF-CONTAINED single-file program reach ANY AutomationOS
         * syscall (yield=15, get_ticks=40, fb_acquire=39, open/read/close, ...)
         * WITHOUT inline asm or linking a libc -- the missing piece that made
         * graphical/interactive programs impossible on the on-device compiler.
         * ABI: n->rax, a1->rdi, a2->rsi, a3->rdx (up to 3 syscall args; covers
         * almost every syscall). Returns the kernel's result in rax. */
        if (callee && (ce_eq(callee, "syscall") || ce_eq(callee, "sys_call"))) {
            static const char* const SCR[4] = { "rax", "rdi", "rsi", "rdx" };
            int i;
            if (nargs < 1) {
                cc_error(g, e->span.start_line, "syscall(n,...) needs a number");
                cc_emit(g, "mov rax, 0");
                return;
            }
            if (nargs > 4)
                cc_error(g, e->span.start_line, "syscall: at most 3 args after the number");
            int n = nargs < 4 ? nargs : 4;
            for (i = 0; i < n; i++) {
                ce_expr(g, args[i], depth + 1);   /* arg i -> rax */
                cc_emit(g, "push rax");
            }
            for (i = n - 1; i >= 0; i--)          /* reverse pop: top is arg n-1 */
                ce_pop_into(g, SCR[i]);           /* arg0->rax(num), 1->rdi, 2->rsi, 3->rdx */
            cc_emit(g, "syscall");                /* result left in rax */
            return;
        }

        /* ---- normal call ----
         * Evaluate each arg to rax and push (left-to-right). Maintain 16-byte
         * rsp alignment at the `call`: if an odd number of 8-byte temps are on
         * the stack, sub rsp,8 first / add it back after. Then pop args into
         * rdi,rsi,rdx,rcx,r8,r9 (reverse pop order lands them correctly). */
        {
            int i;
            int pad = (nargs & 1) ? 1 : 0;        /* odd pushes -> need 8 pad */

            if (pad) cc_emit(g, "sub rsp, 8");
            for (i = 0; i < nargs; i++) {
                ce_expr(g, args[i], depth + 1);
                cc_emit(g, "push rax");
            }
            /* pop into arg registers in order: arg(nargs-1) was pushed last and
             * is on top -> pop into its register, ... down to arg0. */
            for (i = nargs - 1; i >= 0; i--) {
                ce_pop_into(g, CE_ARG_REGS[i]);
            }
            if (callee && callee[0]) {
                cc_emit2(g, "call ", callee);
            } else {
                /* indirect/complex callee not supported in v1 */
                cc_error(g, e->span.start_line, "only simple-named calls supported");
                cc_emit(g, "mov rax, 0");
            }
            if (pad) cc_emit(g, "add rsp, 8");
            /* result already in rax */
            return;
        }
    }

    /* -------------------------------------------------- casts / sizeof */
    case AST_CAST: {
        /* v1: treat a cast as its operand (all widths are 8 bytes except char). */
        ce_expr(g, ce_child(e, 0), depth + 1);
        return;
    }

    case AST_SIZEOF: {
        const AstNode* ch = ce_child(e, 0);
        long sz;
        if (e->type_str[0]) {
            sz = (long)cc_sizeof_type(e->type_str);
        } else if (ch) {
            char ty[CC_NAME];
            cc_infer_type(g, ch, ty, (int)sizeof(ty));
            sz = (long)cc_sizeof_type(ty);
        } else {
            sz = 8;
        }
        cc_emit_num(g, "mov rax, ", sz, "");
        return;
    }

    case AST_COMMA: {
        /* evaluate each, result of the last is kept in rax */
        const AstNode* c = e->first_child;
        if (!c) { cc_emit(g, "mov rax, 0"); return; }
        while (c) {
            ce_expr(g, c, depth + 1);
            c = c->next;
        }
        return;
    }

    default:
        cc_error(g, e->span.start_line, "unsupported expression node");
        cc_emit(g, "mov rax, 0");
        return;
    }
}

void cg_expr(Cg* g, const AstNode* e)
{
    ce_expr(g, e, 0);
}

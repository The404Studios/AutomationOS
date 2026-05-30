/*
 * js_parse.c -- recursive-descent + Pratt parser producing the AST.
 * =================================================================
 *
 * Statements are parsed by recursive descent; expressions by a Pratt
 * (precedence-climbing) parser so all binary/assignment/conditional operators
 * get correct precedence and associativity in one place.
 *
 * Output: a NODE_PROGRAM whose kids[] are top-level statements. All nodes are
 * allocated in the VM arena. On error a NULL is returned and js_parse_error()
 * yields a message (also stored in vm->errmsg).
 *
 * Precedence (low -> high), matching ECMAScript:
 *   assignment (= += ...)            right-assoc, lowest
 *   conditional ?:                   right-assoc
 *   nullish ??, logical ||, &&
 *   bitwise | ^ &
 *   equality == != === !==
 *   relational < <= > >= in instanceof
 *   shift << >> >>>
 *   additive + -
 *   multiplicative * / %
 *   exponent **                      right-assoc
 *   unary  ! ~ + - typeof void delete  (prefix)
 *   postfix ++ --
 *   call / member / new
 *   primary
 */

#include "js_internal.h"

/* last parse error, also mirrored into vm->errmsg */
static char g_parse_err[JS_ERRMSG_CAP];
const char *js_parse_error(void) { return g_parse_err; }

typedef struct {
    js_vm   *vm;
    js_lexer lx;
    int      error;
} parser;

/* ------------------------------------------------------------------ */
/*  Error reporting                                                    */
/* ------------------------------------------------------------------ */
static void p_err(parser *p, const char *msg)
{
    if (p->error) return;
    p->error = 1;
    /* "Parse error (line N): msg" */
    js_usize n = 0;
    const char *pre = "Parse error: ";
    while (pre[n] && n < JS_ERRMSG_CAP-1) { g_parse_err[n] = pre[n]; n++; }
    js_usize i = 0;
    while (msg[i] && n < JS_ERRMSG_CAP-1) g_parse_err[n++] = msg[i++];
    g_parse_err[n] = 0;
}

/* ------------------------------------------------------------------ */
/*  Token helpers                                                      */
/* ------------------------------------------------------------------ */
static js_token *cur(parser *p) { return &p->lx.tok; }
static js_tok_kind kind(parser *p) { return p->lx.tok.kind; }
static void advance(parser *p) { js_lex_next(&p->lx); if (p->lx.error) p_err(p, "lexer error"); }

static int accept(parser *p, js_tok_kind k)
{
    if (kind(p) == k) { advance(p); return 1; }
    return 0;
}
static int expect(parser *p, js_tok_kind k, const char *what)
{
    if (kind(p) == k) { advance(p); return 1; }
    p_err(p, what);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Node construction                                                  */
/* ------------------------------------------------------------------ */
static js_node *mknode(parser *p, js_node_kind k)
{
    js_node *n = (js_node *)js_arena_alloc(p->vm, sizeof(js_node));
    if (!n) { p_err(p, "out of memory"); return NULL; }
    n->kind = k;
    n->line = cur(p)->line;
    return n;
}

static void push_kid(parser *p, js_node *parent, js_node *child)
{
    if (parent->nkids >= parent->kcap) {
        int nc = parent->kcap ? parent->kcap * 2 : 4;
        js_node **nk = (js_node **)js_arena_alloc(p->vm, (js_usize)nc * sizeof(js_node*));
        if (!nk) { p_err(p, "out of memory"); return; }
        for (int i = 0; i < parent->nkids; i++) nk[i] = parent->kids[i];
        parent->kids = nk;
        parent->kcap = nc;
    }
    parent->kids[parent->nkids++] = child;
}

/* ------------------------------------------------------------------ */
/*  Forward decls                                                      */
/* ------------------------------------------------------------------ */
static js_node *parse_stmt(parser *p);
static js_node *parse_block(parser *p);
static js_node *parse_expr(parser *p);            /* full (with comma) */
static js_node *parse_assign(parser *p);          /* no comma          */
static js_node *parse_binary(parser *p, int min_prec);
static js_node *parse_unary(parser *p);
static js_node *parse_postfix(parser *p);
static js_node *parse_call_member(parser *p, js_node *base);
static js_node *parse_primary(parser *p);
static js_node *parse_function(parser *p, int is_decl);

/* ------------------------------------------------------------------ */
/*  Precedence table for binary operators                             */
/* ------------------------------------------------------------------ */
static int binop_prec(js_tok_kind k)
{
    switch (k) {
    case T_NULLISH: return 1;
    case T_OR:      return 2;
    case T_AND:     return 3;
    case T_BOR:     return 4;
    case T_BXOR:    return 5;
    case T_BAND:    return 6;
    case T_EQ: case T_NEQ: case T_SEQ: case T_SNEQ: return 7;
    case T_LT: case T_LE: case T_GT: case T_GE:
    case T_IN: case T_INSTANCEOF: return 8;
    case T_SHL: case T_SHR: case T_USHR: return 9;
    case T_PLUS: case T_MINUS: return 10;
    case T_STAR: case T_SLASH: case T_PERCENT: return 11;
    case T_STARSTAR: return 12;   /* right-assoc, handled specially */
    default: return -1;
    }
}
static int is_logical(js_tok_kind k)
{
    return k==T_AND || k==T_OR || k==T_NULLISH;
}
static int is_assign_op(js_tok_kind k)
{
    switch (k) {
    case T_ASSIGN: case T_PLUSEQ: case T_MINUSEQ: case T_STAREQ:
    case T_SLASHEQ: case T_PERCENTEQ: case T_BANDEQ: case T_BOREQ:
    case T_BXOREQ: case T_SHLEQ: case T_SHREQ: case T_USHREQ:
    case T_ANDEQ: case T_OREQ:
        return 1;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Expressions (Pratt)                                                */
/* ------------------------------------------------------------------ */

/* assignment level (right-assoc, also handles ternary below it) */
static js_node *parse_assign(parser *p)
{
    js_node *left = parse_binary(p, 1);
    if (!left) return NULL;

    /* ternary ?: */
    if (kind(p) == T_QUESTION) {
        advance(p);
        js_node *cons = parse_assign(p);
        if (!expect(p, T_COLON, "expected ':' in conditional")) return NULL;
        js_node *alt = parse_assign(p);
        js_node *n = mknode(p, NODE_COND);
        if (!n) return NULL;
        n->a = left; n->b_ = cons; n->c = alt;
        return n;
    }

    if (is_assign_op(kind(p))) {
        js_tok_kind op = kind(p);
        advance(p);
        js_node *rhs = parse_assign(p);   /* right-assoc */
        js_node *n = mknode(p, NODE_ASSIGN);
        if (!n) return NULL;
        n->op = op;
        n->a = left;     /* target */
        n->b_ = rhs;
        return n;
    }
    return left;
}

/* binary precedence climbing; min_prec is the lowest precedence allowed */
static js_node *parse_binary(parser *p, int min_prec)
{
    js_node *left = parse_unary(p);
    if (!left) return NULL;

    for (;;) {
        js_tok_kind op = kind(p);
        int prec = binop_prec(op);
        if (prec < min_prec || prec < 0) break;

        advance(p);

        /* right-assoc for ** : parse rhs at same prec; else prec+1 */
        int next_min = (op == T_STARSTAR) ? prec : prec + 1;
        js_node *right = parse_binary(p, next_min);
        if (!right) return NULL;

        js_node *n = mknode(p, is_logical(op) ? NODE_LOGICAL : NODE_BINARY);
        if (!n) return NULL;
        n->op = op;
        n->a = left;
        n->b_ = right;
        left = n;
    }
    return left;
}

static js_node *parse_unary(parser *p)
{
    js_tok_kind k = kind(p);
    switch (k) {
    case T_NOT: case T_BNOT: case T_PLUS: case T_MINUS:
    case T_TYPEOF: case T_VOID: case T_DELETE: {
        advance(p);
        js_node *operand = parse_unary(p);
        if (!operand) return NULL;
        js_node *n = mknode(p, NODE_UNARY);
        if (!n) return NULL;
        n->op = k;
        n->a = operand;
        return n;
    }
    case T_INC: case T_DEC: {
        advance(p);
        js_node *operand = parse_unary(p);
        if (!operand) return NULL;
        js_node *n = mknode(p, NODE_UPDATE);
        if (!n) return NULL;
        n->op = k;
        n->flag = 0;          /* prefix */
        n->a = operand;
        return n;
    }
    default:
        return parse_postfix(p);
    }
}

static js_node *parse_postfix(parser *p)
{
    js_node *base = parse_primary(p);
    if (!base) return NULL;
    base = parse_call_member(p, base);
    if (!base) return NULL;

    /* postfix ++ / -- (no newline between for ASI correctness) */
    if ((kind(p)==T_INC || kind(p)==T_DEC) && !cur(p)->nl_before) {
        js_tok_kind op = kind(p);
        advance(p);
        js_node *n = mknode(p, NODE_UPDATE);
        if (!n) return NULL;
        n->op = op;
        n->flag = UPDATE_POSTFIX;
        n->a = base;
        return n;
    }
    return base;
}

/* member access, indexing, and calls, left-associative chains */
static js_node *parse_call_member(parser *p, js_node *base)
{
    for (;;) {
        if (kind(p) == T_DOT) {
            advance(p);
            /* property name: identifier or keyword-as-name */
            js_node *n = mknode(p, NODE_MEMBER);
            if (!n) return NULL;
            n->a = base;
            n->flag = 0;     /* not computed */
            /* allow keywords as property names */
            if (kind(p) == T_IDENT) {
                n->str = cur(p)->str;
                advance(p);
            } else if (cur(p)->len > 0 && kind(p) != T_EOF) {
                /* keyword used as member name: intern its raw spelling */
                n->str = js_str_intern(p->vm, cur(p)->start, cur(p)->len);
                advance(p);
            } else {
                p_err(p, "expected property name after '.'");
                return NULL;
            }
            base = n;
        } else if (kind(p) == T_LBRACKET) {
            advance(p);
            js_node *idx = parse_expr(p);
            if (!expect(p, T_RBRACKET, "expected ']'")) return NULL;
            js_node *n = mknode(p, NODE_MEMBER);
            if (!n) return NULL;
            n->a = base;
            n->b_ = idx;
            n->flag = MEMBER_COMPUTED;
            base = n;
        } else if (kind(p) == T_LPAREN) {
            advance(p);
            js_node *n = mknode(p, NODE_CALL);
            if (!n) return NULL;
            n->a = base;
            while (kind(p) != T_RPAREN && kind(p) != T_EOF) {
                js_node *arg = parse_assign(p);
                if (!arg) return NULL;
                push_kid(p, n, arg);
                if (!accept(p, T_COMMA)) break;
            }
            if (!expect(p, T_RPAREN, "expected ')' in call")) return NULL;
            base = n;
        } else {
            break;
        }
    }
    return base;
}

/* detect arrow function from a parenthesized param list or single ident.
 * We do a lightweight lookahead by trying to parse params, then checking
 * for '=>'. To keep it simple we handle: IDENT => ...  and (a,b,...) => ...
 */
static js_node *parse_arrow_body(parser *p, js_node *params)
{
    js_node *n = mknode(p, NODE_ARROW);
    if (!n) return NULL;
    n->a = params;
    if (kind(p) == T_LBRACE) {
        n->b_ = parse_block(p);
        n->flag = 0;          /* block body */
    } else {
        n->b_ = parse_assign(p);
        n->flag = 1;          /* expression body */
    }
    return n;
}

/* try to parse "(params) =>" or "ident =>"; returns NULL (no consume) if not */
static js_node *try_parse_arrow(parser *p)
{
    /* single-identifier arrow:  x => ...  */
    if (kind(p) == T_IDENT) {
        /* peek next without losing position is hard with our 1-token lexer;
         * instead snapshot the lexer state. */
        js_lexer save = p->lx;
        int saverr = p->error;
        js_string *name = cur(p)->str;
        advance(p);
        if (kind(p) == T_ARROW) {
            advance(p);
            js_node *params = mknode(p, NODE_PARAMLIST);
            js_node *id = mknode(p, NODE_IDENT);
            if (!params || !id) return NULL;
            id->str = name;
            push_kid(p, params, id);
            return parse_arrow_body(p, params);
        }
        /* rollback */
        p->lx = save;
        p->error = saverr;
        return NULL;
    }

    if (kind(p) == T_LPAREN) {
        js_lexer save = p->lx;
        int saverr = p->error;
        advance(p);
        js_node *params = mknode(p, NODE_PARAMLIST);
        if (!params) return NULL;
        int ok = 1;
        while (kind(p) != T_RPAREN && kind(p) != T_EOF) {
            if (kind(p) != T_IDENT) { ok = 0; break; }
            js_node *id = mknode(p, NODE_IDENT);
            if (!id) return NULL;
            id->str = cur(p)->str;
            push_kid(p, params, id);
            advance(p);
            if (!accept(p, T_COMMA)) break;
        }
        if (ok && kind(p) == T_RPAREN) {
            advance(p);
            if (kind(p) == T_ARROW) {
                advance(p);
                return parse_arrow_body(p, params);
            }
        }
        /* not an arrow: rollback and let normal grouping parse it */
        p->lx = save;
        p->error = saverr;
        return NULL;
    }
    return NULL;
}

static js_node *parse_object_literal(parser *p)
{
    js_node *n = mknode(p, NODE_OBJECT);
    if (!n) return NULL;
    /* kids are flattened key,value pairs */
    while (kind(p) != T_RBRACE && kind(p) != T_EOF) {
        js_node *key = mknode(p, NODE_STRING);
        if (!key) return NULL;
        if (kind(p) == T_IDENT) {
            key->str = cur(p)->str; advance(p);
        } else if (kind(p) == T_STRING) {
            key->str = cur(p)->str; advance(p);
        } else if (kind(p) == T_NUMBER) {
            key->str = js_num_to_str(p->vm, cur(p)->num); advance(p);
        } else if (cur(p)->len > 0) {
            /* keyword as key */
            key->str = js_str_intern(p->vm, cur(p)->start, cur(p)->len);
            advance(p);
        } else {
            p_err(p, "expected property key"); return NULL;
        }

        js_node *val;
        if (kind(p) == T_COLON) {
            advance(p);
            val = parse_assign(p);
        } else {
            /* shorthand { x } -> { x: x } */
            val = mknode(p, NODE_IDENT);
            if (val) val->str = key->str;
        }
        if (!val) return NULL;
        push_kid(p, n, key);
        push_kid(p, n, val);
        if (!accept(p, T_COMMA)) break;
    }
    if (!expect(p, T_RBRACE, "expected '}' in object literal")) return NULL;
    return n;
}

static js_node *parse_array_literal(parser *p)
{
    js_node *n = mknode(p, NODE_ARRAY);
    if (!n) return NULL;
    while (kind(p) != T_RBRACKET && kind(p) != T_EOF) {
        if (kind(p) == T_COMMA) {   /* elision -> undefined */
            js_node *u = mknode(p, NODE_UNDEFINED);
            push_kid(p, n, u);
            advance(p);
            continue;
        }
        js_node *el = parse_assign(p);
        if (!el) return NULL;
        push_kid(p, n, el);
        if (!accept(p, T_COMMA)) break;
    }
    if (!expect(p, T_RBRACKET, "expected ']' in array literal")) return NULL;
    return n;
}

static js_node *parse_primary(parser *p)
{
    js_token *t = cur(p);
    switch (t->kind) {
    case T_NUMBER: {
        js_node *n = mknode(p, NODE_NUMBER);
        if (n) n->num = t->num;
        advance(p);
        return n;
    }
    case T_STRING: {
        js_node *n = mknode(p, NODE_STRING);
        if (n) n->str = t->str;
        advance(p);
        return n;
    }
    case T_TEMPLATE: {
        /* Treat template as plain string literal (no interpolation split).
         * Documented limitation. */
        js_node *n = mknode(p, NODE_STRING);
        if (n) n->str = t->str;
        advance(p);
        return n;
    }
    case T_TRUE: case T_FALSE: {
        js_node *n = mknode(p, NODE_BOOL);
        if (n) n->b = (t->kind == T_TRUE);
        advance(p);
        return n;
    }
    case T_NULL:      { advance(p); return mknode(p, NODE_NULL); }
    case T_UNDEFINED: { advance(p); return mknode(p, NODE_UNDEFINED); }
    case T_THIS:      { advance(p); return mknode(p, NODE_THIS); }
    case T_IDENT: {
        /* maybe arrow */
        js_node *arrow = try_parse_arrow(p);
        if (arrow) return arrow;
        if (p->error) return NULL;
        js_node *n = mknode(p, NODE_IDENT);
        if (n) n->str = t->str;
        advance(p);
        return n;
    }
    case T_LPAREN: {
        /* maybe arrow with paren params */
        js_node *arrow = try_parse_arrow(p);
        if (arrow) return arrow;
        if (p->error) return NULL;
        advance(p);  /* '(' */
        js_node *e = parse_expr(p);
        if (!expect(p, T_RPAREN, "expected ')'")) return NULL;
        return e;
    }
    case T_LBRACKET: { advance(p); return parse_array_literal(p); }
    case T_LBRACE:   { advance(p); return parse_object_literal(p); }
    case T_FUNCTION: { return parse_function(p, 0); }
    case T_NEW: {
        advance(p);
        js_node *callee = parse_primary(p);
        if (!callee) return NULL;
        /* allow member chain on callee before args: new a.b() */
        for (;;) {
            if (kind(p) == T_DOT) {
                advance(p);
                js_node *m = mknode(p, NODE_MEMBER);
                if (!m) return NULL;
                m->a = callee; m->flag = 0;
                m->str = (kind(p)==T_IDENT) ? cur(p)->str
                         : js_str_intern(p->vm, cur(p)->start, cur(p)->len);
                advance(p);
                callee = m;
            } else break;
        }
        js_node *n = mknode(p, NODE_NEW);
        if (!n) return NULL;
        n->a = callee;
        if (accept(p, T_LPAREN)) {
            while (kind(p) != T_RPAREN && kind(p) != T_EOF) {
                js_node *arg = parse_assign(p);
                if (!arg) return NULL;
                push_kid(p, n, arg);
                if (!accept(p, T_COMMA)) break;
            }
            if (!expect(p, T_RPAREN, "expected ')' after new args")) return NULL;
        }
        return n;
    }
    default:
        p_err(p, "unexpected token in expression");
        return NULL;
    }
}

/* expression with comma operator */
static js_node *parse_expr(parser *p)
{
    js_node *e = parse_assign(p);
    if (!e) return NULL;
    if (kind(p) == T_COMMA) {
        js_node *seq = mknode(p, NODE_SEQ);
        if (!seq) return NULL;
        push_kid(p, seq, e);
        while (accept(p, T_COMMA)) {
            js_node *next = parse_assign(p);
            if (!next) return NULL;
            push_kid(p, seq, next);
        }
        return seq;
    }
    return e;
}

/* ------------------------------------------------------------------ */
/*  Functions                                                          */
/* ------------------------------------------------------------------ */
static js_node *parse_params(parser *p)
{
    js_node *plist = mknode(p, NODE_PARAMLIST);
    if (!plist) return NULL;
    if (!expect(p, T_LPAREN, "expected '(' for parameters")) return NULL;
    while (kind(p) != T_RPAREN && kind(p) != T_EOF) {
        if (kind(p) != T_IDENT) { p_err(p, "expected parameter name"); return NULL; }
        js_node *id = mknode(p, NODE_IDENT);
        if (!id) return NULL;
        id->str = cur(p)->str;
        push_kid(p, plist, id);
        advance(p);
        if (!accept(p, T_COMMA)) break;
    }
    if (!expect(p, T_RPAREN, "expected ')' after parameters")) return NULL;
    return plist;
}

static js_node *parse_function(parser *p, int is_decl)
{
    advance(p);  /* 'function' */
    js_node *n = mknode(p, is_decl ? NODE_FUNCDECL : NODE_FUNCTION);
    if (!n) return NULL;
    if (kind(p) == T_IDENT) {
        n->str = cur(p)->str;
        advance(p);
    } else if (is_decl) {
        p_err(p, "function declaration requires a name");
        return NULL;
    }
    n->a = parse_params(p);
    if (!n->a) return NULL;
    n->b_ = parse_block(p);
    if (!n->b_) return NULL;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Statements                                                         */
/* ------------------------------------------------------------------ */
/* consume an optional statement terminator (semicolon or ASI) */
static void semicolon(parser *p)
{
    if (kind(p) == T_SEMI) { advance(p); return; }
    /* ASI-lite: allowed before }, EOF, or after a newline */
    if (kind(p) == T_RBRACE || kind(p) == T_EOF) return;
    if (cur(p)->nl_before) return;
    /* otherwise tolerate (best-effort) -- don't hard fail on missing ';' */
}

static js_node *parse_var(parser *p, int vk)
{
    /* one statement may declare multiple comma-separated declarators;
     * we wrap them in a BLOCK if more than one (keeps node model simple). */
    advance(p);  /* var/let/const */
    js_node *first = NULL;
    js_node *list = NULL;
    for (;;) {
        if (kind(p) != T_IDENT) { p_err(p, "expected variable name"); return NULL; }
        js_node *d = mknode(p, NODE_VARDECL);
        if (!d) return NULL;
        d->flag = vk;
        d->str = cur(p)->str;
        advance(p);
        if (accept(p, T_ASSIGN)) {
            d->a = parse_assign(p);
            if (!d->a) return NULL;
        }
        if (!first) { first = d; }
        else {
            if (!list) {
                list = mknode(p, NODE_BLOCK);
                if (!list) return NULL;
                list->flag = 1;     /* mark as "declaration group" (no scope) */
                push_kid(p, list, first);
            }
            push_kid(p, list, d);
        }
        if (!accept(p, T_COMMA)) break;
    }
    semicolon(p);
    return list ? list : first;
}

static js_node *parse_if(parser *p)
{
    advance(p);  /* if */
    if (!expect(p, T_LPAREN, "expected '(' after if")) return NULL;
    js_node *cond = parse_expr(p);
    if (!expect(p, T_RPAREN, "expected ')' after if condition")) return NULL;
    js_node *then = parse_stmt(p);
    js_node *els = NULL;
    if (accept(p, T_ELSE)) els = parse_stmt(p);
    js_node *n = mknode(p, NODE_IF);
    if (!n) return NULL;
    n->a = cond; n->b_ = then; n->c = els;
    return n;
}

static js_node *parse_while(parser *p)
{
    advance(p);
    if (!expect(p, T_LPAREN, "expected '(' after while")) return NULL;
    js_node *cond = parse_expr(p);
    if (!expect(p, T_RPAREN, "expected ')'")) return NULL;
    js_node *body = parse_stmt(p);
    js_node *n = mknode(p, NODE_WHILE);
    if (!n) return NULL;
    n->a = cond; n->b_ = body;
    return n;
}

static js_node *parse_do(parser *p)
{
    advance(p);
    js_node *body = parse_stmt(p);
    if (!expect(p, T_WHILE, "expected 'while' after do body")) return NULL;
    if (!expect(p, T_LPAREN, "expected '('")) return NULL;
    js_node *cond = parse_expr(p);
    if (!expect(p, T_RPAREN, "expected ')'")) return NULL;
    semicolon(p);
    js_node *n = mknode(p, NODE_DOWHILE);
    if (!n) return NULL;
    n->a = cond; n->b_ = body;
    return n;
}

static js_node *parse_for(parser *p)
{
    advance(p);  /* for */
    if (!expect(p, T_LPAREN, "expected '(' after for")) return NULL;

    /* parse the init clause, detecting for-in / for-of */
    int vk = -1;
    if (kind(p) == T_VAR) { vk = VK_VAR; }
    else if (kind(p) == T_LET) { vk = VK_LET; }
    else if (kind(p) == T_CONST) { vk = VK_CONST; }

    /* Look for "<decl-or-ident> in/of <expr>" */
    if (vk >= 0) {
        advance(p);  /* consume var/let/const */
        if (kind(p) != T_IDENT) { p_err(p, "expected name in for"); return NULL; }
        js_string *vname = cur(p)->str;
        advance(p);
        if (kind(p) == T_IN || kind(p) == T_OF) {
            int is_of = (kind(p) == T_OF);
            advance(p);
            js_node *iter = parse_assign(p);
            if (!expect(p, T_RPAREN, "expected ')' in for-in/of")) return NULL;
            js_node *body = parse_stmt(p);
            js_node *n = mknode(p, NODE_FORIN);
            if (!n) return NULL;
            n->str = vname;
            n->flag = (is_of ? FORIN_OF : 0) | (vk << 4);
            n->a = iter;
            n->b_ = body;
            return n;
        }
        /* classic for with var decl init */
        js_node *decl = mknode(p, NODE_VARDECL);
        if (!decl) return NULL;
        decl->flag = vk;
        decl->str = vname;
        if (accept(p, T_ASSIGN)) { decl->a = parse_assign(p); if (!decl->a) return NULL; }
        /* allow additional declarators */
        js_node *init = decl;
        if (kind(p) == T_COMMA) {
            js_node *grp = mknode(p, NODE_BLOCK);
            if (!grp) return NULL;
            grp->flag = 1;
            push_kid(p, grp, decl);
            while (accept(p, T_COMMA)) {
                js_node *d2 = mknode(p, NODE_VARDECL);
                if (!d2) return NULL;
                d2->flag = vk;
                if (kind(p) != T_IDENT) { p_err(p, "expected name"); return NULL; }
                d2->str = cur(p)->str; advance(p);
                if (accept(p, T_ASSIGN)) { d2->a = parse_assign(p); }
                push_kid(p, grp, d2);
            }
            init = grp;
        }
        if (!expect(p, T_SEMI, "expected ';' in for")) return NULL;
        js_node *cond = (kind(p)==T_SEMI) ? NULL : parse_expr(p);
        if (!expect(p, T_SEMI, "expected ';' in for")) return NULL;
        js_node *post = (kind(p)==T_RPAREN) ? NULL : parse_expr(p);
        if (!expect(p, T_RPAREN, "expected ')' in for")) return NULL;
        js_node *body = parse_stmt(p);
        js_node *n = mknode(p, NODE_FOR);
        if (!n) return NULL;
        n->a = init; n->b_ = cond; n->c = post; n->d = body;
        return n;
    }

    /* expression-initialized for, or for-in/of over a plain lvalue */
    js_node *init = NULL;
    if (kind(p) != T_SEMI) {
        init = parse_expr(p);
        if (!init) return NULL;
        /* (we don't support `for (x in obj)` without decl to keep it simple;
         *  could be added.) */
    }
    if (!expect(p, T_SEMI, "expected ';' in for")) return NULL;
    js_node *cond = (kind(p)==T_SEMI) ? NULL : parse_expr(p);
    if (!expect(p, T_SEMI, "expected ';' in for")) return NULL;
    js_node *post = (kind(p)==T_RPAREN) ? NULL : parse_expr(p);
    if (!expect(p, T_RPAREN, "expected ')' in for")) return NULL;
    js_node *body = parse_stmt(p);
    js_node *n = mknode(p, NODE_FOR);
    if (!n) return NULL;
    /* wrap expression init as expr-stmt for uniform handling */
    if (init) {
        js_node *es = mknode(p, NODE_EXPRSTMT);
        if (!es) return NULL;
        es->a = init;
        n->a = es;
    }
    n->b_ = cond; n->c = post; n->d = body;
    return n;
}

static js_node *parse_try(parser *p)
{
    advance(p);  /* try */
    js_node *n = mknode(p, NODE_TRY);
    if (!n) return NULL;
    n->a = parse_block(p);
    if (!n->a) return NULL;
    if (accept(p, T_CATCH)) {
        if (accept(p, T_LPAREN)) {
            if (kind(p) == T_IDENT) { n->str = cur(p)->str; advance(p); }
            if (!expect(p, T_RPAREN, "expected ')' after catch param")) return NULL;
        }
        n->b_ = parse_block(p);
        if (!n->b_) return NULL;
    }
    if (accept(p, T_FINALLY)) {
        n->c = parse_block(p);
        if (!n->c) return NULL;
    }
    return n;
}

static js_node *parse_block(parser *p)
{
    if (!expect(p, T_LBRACE, "expected '{'")) return NULL;
    js_node *n = mknode(p, NODE_BLOCK);
    if (!n) return NULL;
    while (kind(p) != T_RBRACE && kind(p) != T_EOF) {
        js_node *s = parse_stmt(p);
        if (!s) return NULL;
        push_kid(p, n, s);
        if (p->error) return NULL;
    }
    if (!expect(p, T_RBRACE, "expected '}'")) return NULL;
    return n;
}

static js_node *parse_stmt(parser *p)
{
    switch (kind(p)) {
    case T_LBRACE:   return parse_block(p);
    case T_VAR:      return parse_var(p, VK_VAR);
    case T_LET:      return parse_var(p, VK_LET);
    case T_CONST:    return parse_var(p, VK_CONST);
    case T_FUNCTION: return parse_function(p, 1);
    case T_IF:       return parse_if(p);
    case T_WHILE:    return parse_while(p);
    case T_DO:       return parse_do(p);
    case T_FOR:      return parse_for(p);
    case T_TRY:      return parse_try(p);
    case T_SEMI:     { advance(p); return mknode(p, NODE_EMPTY); }
    case T_RETURN: {
        advance(p);
        js_node *n = mknode(p, NODE_RETURN);
        if (!n) return NULL;
        if (kind(p) != T_SEMI && kind(p) != T_RBRACE && kind(p) != T_EOF &&
            !cur(p)->nl_before) {
            n->a = parse_expr(p);
            if (!n->a) return NULL;
        }
        semicolon(p);
        return n;
    }
    case T_BREAK:    { advance(p); semicolon(p); return mknode(p, NODE_BREAK); }
    case T_CONTINUE: { advance(p); semicolon(p); return mknode(p, NODE_CONTINUE); }
    case T_THROW: {
        advance(p);
        js_node *n = mknode(p, NODE_THROW);
        if (!n) return NULL;
        n->a = parse_expr(p);
        if (!n->a) return NULL;
        semicolon(p);
        return n;
    }
    default: {
        /* expression statement */
        js_node *e = parse_expr(p);
        if (!e) return NULL;
        js_node *n = mknode(p, NODE_EXPRSTMT);
        if (!n) return NULL;
        n->a = e;
        semicolon(p);
        return n;
    }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry                                                              */
/* ------------------------------------------------------------------ */
js_node *js_parse_program(js_vm *vm, const char *src, js_usize len)
{
    parser p;
    p.vm = vm;
    p.error = 0;
    g_parse_err[0] = 0;
    js_lex_init(&p.lx, vm, src, len);

    if (p.lx.error) { p_err(&p, "invalid token"); return NULL; }

    js_node *prog = mknode(&p, NODE_PROGRAM);
    if (!prog) return NULL;

    while (kind(&p) != T_EOF) {
        js_node *s = parse_stmt(&p);
        if (!s || p.error) {
            /* mirror to vm->errmsg */
            js_usize i = 0;
            while (g_parse_err[i] && i < JS_ERRMSG_CAP-1) { vm->errmsg[i]=g_parse_err[i]; i++; }
            vm->errmsg[i] = 0;
            return NULL;
        }
        push_kid(&p, prog, s);
    }
    return prog;
}

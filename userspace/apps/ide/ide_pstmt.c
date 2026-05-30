/*
 * ide_pstmt.c -- recursive-descent C parser: STATEMENTS.
 *
 * Implements parse_statement() and parse_compound() declared in ide_parser.h,
 * plus the local panic-mode recovery used when a statement is malformed.
 *
 * Freestanding: no libc, no malloc, no stdio. Static buffers + private str
 * helpers only. AST nodes are built ONLY through the ast_* arena API. Obeys the
 * cursor-ownership contract documented in ide_parser.h:
 *
 *   - Each parse_X consumes exactly its construct and leaves the cursor on the
 *     first token AFTER it.
 *   - parse_statement consumes its terminating ';' (expr/return/decl/...) OR the
 *     closing '}' (compound). parse_compound consumes '{' and the matching '}'.
 *   - On error: pdiag() + precover_to(";}") (+ eat a ';' if present); return a
 *     best-effort node (AST_NONE if nothing better), NEVER NULL, and ALWAYS make
 *     forward progress so callers never spin.
 *
 * parse_expression / parse_declaration / the ast_* arena entry points live in
 * other TUs and are resolved at link time; this file only declares/calls them.
 */
#include "ide_parser.h"

/* ------------------------------------------------------------------ *
 *  Private freestanding string helpers (no libc).
 * ------------------------------------------------------------------ */

/* Bounded copy of [s,len) into dst[cap], always NUL-terminated. */
static void s_copyn(char* dst, int cap, const char* s, int len) {
    if (cap <= 0) return;
    int n = len;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) dst[i] = s[i];
    dst[n] = '\0';
}

/* Copy a token's text into an AST name/type slot (NULL/short-safe). */
static void s_copy_tok(char* dst, int cap, Tok* t) {
    if (!t || !t->s || t->len <= 0) { if (cap > 0) dst[0] = '\0'; return; }
    s_copyn(dst, cap, t->s, t->len);
}

/* ------------------------------------------------------------------ *
 *  Depth guard: keep pathological / adversarial nesting from blowing the
 *  native C stack. Past the cap we stop recursing into nested statements
 *  and just skip to a safe stop token, returning a placeholder.
 * ------------------------------------------------------------------ */
#define STMT_MAX_DEPTH   256
static int g_stmt_depth = 0;

/* Iteration ceiling for a single compound body (defensive; the cursor's own
 * forward-progress guarantee already bounds this by the token count). */
#define COMPOUND_MAX_STMTS  (PARSE_MAX_TOKS)

/* ------------------------------------------------------------------ *
 *  Local helpers.
 * ------------------------------------------------------------------ */

/* Allocate a node; never returns NULL for our purposes (ast_new yields a real
 * node or, if the arena is exhausted, a sentinel we still treat as valid). */
static AstNode* mk(AstKind k) {
    AstNode* n = ast_new(k);
    return n;  /* may be 0 if arena exhausted; callers null-check before use */
}

/* Best-effort placeholder for a required production (never NULL semantics). */
static AstNode* mk_none(Parser* p, Tok* start) {
    AstNode* n = ast_new(AST_NONE);
    if (n) {
        Tok* end = pk(p);
        n->span = span_of(p, start ? start : end, end);
    }
    return n;
}

/* Set a node's span to cover [start .. last-consumed]. We use the token just
 * before the current cursor position as the inclusive end (the construct having
 * consumed up to but not including pk()). Falls back to pk() when nothing was
 * consumed. */
static void set_span_to_here(Parser* p, AstNode* n, Tok* start) {
    if (!n) return;
    Tok* end;
    if (p && p->toks && p->pos > 0 && p->pos <= p->ntoks)
        end = &p->toks[p->pos - 1];
    else
        end = pk(p);
    if (!start) start = end;
    n->span = span_of(p, start, end);
}

/* Panic-mode recovery for a broken statement: record a diagnostic, skip to the
 * next ';' or '}' (without consuming '}'), and swallow a ';' if we landed on
 * one. Always advances at least when there is something to advance over, so the
 * caller still makes forward progress (the caller also guards this). */
static void recover_stmt(Parser* p, const char* msg) {
    pdiag(p, msg);
    precover_to(p, ";}");
    if (at_punct(p, ";")) adv(p);
}

/* Does the upcoming token sequence begin a DECLARATION rather than an
 * expression?  Pragmatic "lexer hack":
 *   - current token is a TYPE keyword (TK_TYPE: int/char/struct/union/enum/
 *     static/const/unsigned/signed/long/... -- see lexer), OR the word 'typedef';
 *   - OR current token is an identifier that is a registered typedef name AND
 *     the following token is an identifier or '*' (e.g. "MyType x" / "MyType *p").
 * Storage-class / qualifier / tag keywords all lex as TK_TYPE, so the first arm
 * covers them. */
static int starts_declaration(Parser* p) {
    Tok* t = pk(p);
    if (!t) return 0;

    if (t->kind == TK_TYPE) return 1;
    if (tok_is(t, "typedef")) return 1;

    if (t->kind == TK_ID && is_typename(p, t->s, t->len)) {
        Tok* t2 = pk2(p);
        if (!t2) return 0;
        if (t2->kind == TK_ID) return 1;
        if (t2->kind == TK_PUNCT && tok_is(t2, "*")) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Forward decls (mutual recursion within this TU).
 * ------------------------------------------------------------------ */
static AstNode* parse_stmt_inner(Parser* p);

/* ------------------------------------------------------------------ *
 *  Compound statement:  '{' statement* '}'
 * ------------------------------------------------------------------ */
AstNode* parse_compound(Parser* p) {
    Tok* open = pk(p);
    AstNode* node = mk(AST_COMPOUND);

    if (!at_punct(p, "{")) {
        /* Not actually a block: diagnose, but still return a usable node. */
        pdiag(p, "expected '{'");
        if (node) set_span_to_here(p, node, open);
        else      node = mk_none(p, open);
        return node ? node : mk_none(p, open);
    }
    adv(p);  /* consume '{' */

    int iters = 0;
    while (!at_punct(p, "}") && !at(p, TK_EOF)) {
        if (++iters > COMPOUND_MAX_STMTS) {           /* defensive ceiling */
            pdiag(p, "compound statement too large");
            break;
        }
        int before = p->pos;
        AstNode* st = parse_statement(p);
        if (st && node) ast_add_child(node, st);
        if (p->pos == before) adv(p);                  /* forward progress */
    }

    Tok* close = pk(p);
    if (at_punct(p, "}")) {
        adv(p);                                        /* consume matching '}' */
    } else {
        pdiag(p, "expected '}'");                       /* unterminated block */
    }

    if (node) {
        Tok* end = at_punct(p, "}") ? close : close;   /* close is the '}' or EOF */
        if (p->pos > 0 && p->pos <= p->ntoks) end = &p->toks[p->pos - 1];
        node->span = span_of(p, open, end);
    } else {
        node = mk_none(p, open);
    }
    return node;
}

/* ------------------------------------------------------------------ *
 *  Statement dispatch.
 * ------------------------------------------------------------------ */
AstNode* parse_statement(Parser* p) {
    if (g_stmt_depth >= STMT_MAX_DEPTH) {
        /* Too deep: don't recurse further. Skip a unit of input to a safe
         * stop and return a placeholder, preserving forward progress. */
        Tok* start = pk(p);
        recover_stmt(p, "statement nesting too deep");
        return mk_none(p, start);
    }
    g_stmt_depth++;
    AstNode* r = parse_stmt_inner(p);
    g_stmt_depth--;
    if (!r) r = mk_none(p, pk(p));     /* never NULL */
    return r;
}

static AstNode* parse_stmt_inner(Parser* p) {
    Tok* start = pk(p);

    /* ---- '{' compound ---- */
    if (at_punct(p, "{"))
        return parse_compound(p);

    /* ---- empty statement ';' ---- */
    if (at_punct(p, ";")) {
        AstNode* n = mk(AST_EMPTY_STMT);
        adv(p);                                        /* consume ';' */
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- if / else ---- */
    if (at_kw(p, "if")) {
        AstNode* n = mk(AST_IF);
        adv(p);                                        /* 'if' */
        expect_punct(p, "(");
        AstNode* cond = parse_expression(p);
        expect_punct(p, ")");
        AstNode* then = parse_statement(p);
        if (n) {
            if (cond) ast_add_child(n, cond);
            if (then) ast_add_child(n, then);
        }
        if (at_kw(p, "else")) {
            adv(p);                                    /* 'else' */
            AstNode* els = parse_statement(p);
            if (n && els) ast_add_child(n, els);
        }
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- while ---- */
    if (at_kw(p, "while")) {
        AstNode* n = mk(AST_WHILE);
        adv(p);                                        /* 'while' */
        expect_punct(p, "(");
        AstNode* cond = parse_expression(p);
        expect_punct(p, ")");
        AstNode* body = parse_statement(p);
        if (n) {
            if (cond) ast_add_child(n, cond);
            if (body) ast_add_child(n, body);
            set_span_to_here(p, n, start);
        }
        return n ? n : mk_none(p, start);
    }

    /* ---- do { } while ( ) ; ---- */
    if (at_kw(p, "do")) {
        AstNode* n = mk(AST_DO);
        adv(p);                                        /* 'do' */
        AstNode* body = parse_statement(p);
        if (n && body) ast_add_child(n, body);
        if (at_kw(p, "while")) {
            adv(p);                                    /* 'while' */
            expect_punct(p, "(");
            AstNode* cond = parse_expression(p);
            expect_punct(p, ")");
            if (n && cond) ast_add_child(n, cond);
        } else {
            pdiag(p, "expected 'while' after do-body");
        }
        expect_punct(p, ";");                          /* terminating ';' */
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- for ( init? ; cond? ; iter? ) body ---- */
    if (at_kw(p, "for")) {
        AstNode* n = mk(AST_FOR);
        adv(p);                                        /* 'for' */
        expect_punct(p, "(");

        /* init: declaration (consumes its own ';') | expr-stmt (we eat ';') |
         * empty (just ';'). */
        if (at_punct(p, ";")) {
            adv(p);                                    /* empty init */
        } else if (starts_declaration(p)) {
            AstNode* init = parse_declaration(p);      /* consumes trailing ';' */
            if (n && init) ast_add_child(n, init);
        } else {
            AstNode* init = parse_expression(p);
            if (n && init) ast_add_child(n, init);
            expect_punct(p, ";");
        }

        /* cond: optional expr, then ';' */
        if (!at_punct(p, ";")) {
            AstNode* cond = parse_expression(p);
            if (n && cond) ast_add_child(n, cond);
        }
        expect_punct(p, ";");

        /* iter: optional expr, then ')' */
        if (!at_punct(p, ")")) {
            AstNode* iter = parse_expression(p);
            if (n && iter) ast_add_child(n, iter);
        }
        expect_punct(p, ")");

        AstNode* body = parse_statement(p);
        if (n && body) ast_add_child(n, body);
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- switch ( ) body ---- */
    if (at_kw(p, "switch")) {
        AstNode* n = mk(AST_SWITCH);
        adv(p);                                        /* 'switch' */
        expect_punct(p, "(");
        AstNode* sel = parse_expression(p);
        expect_punct(p, ")");
        AstNode* body = parse_statement(p);
        if (n) {
            if (sel)  ast_add_child(n, sel);
            if (body) ast_add_child(n, body);
            set_span_to_here(p, n, start);
        }
        return n ? n : mk_none(p, start);
    }

    /* ---- case constexpr : labeled-stmt ---- */
    if (at_kw(p, "case")) {
        AstNode* n = mk(AST_CASE);
        adv(p);                                        /* 'case' */
        AstNode* ce = parse_expression(p);             /* constant-expression */
        if (n && ce) ast_add_child(n, ce);
        expect_punct(p, ":");
        /* The statement a case labels (none if immediately followed by '}' /
         * another case/default / EOF). */
        if (!at_punct(p, "}") && !at(p, TK_EOF) &&
            !at_kw(p, "case") && !at_kw(p, "default")) {
            AstNode* st = parse_statement(p);
            if (n && st) ast_add_child(n, st);
        }
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- default : labeled-stmt ---- */
    if (at_kw(p, "default")) {
        AstNode* n = mk(AST_DEFAULT);
        adv(p);                                        /* 'default' */
        expect_punct(p, ":");
        if (!at_punct(p, "}") && !at(p, TK_EOF) &&
            !at_kw(p, "case") && !at_kw(p, "default")) {
            AstNode* st = parse_statement(p);
            if (n && st) ast_add_child(n, st);
        }
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- return expr? ; ---- */
    if (at_kw(p, "return")) {
        AstNode* n = mk(AST_RETURN);
        adv(p);                                        /* 'return' */
        if (!at_punct(p, ";") && !at(p, TK_EOF)) {
            AstNode* e = parse_expression(p);
            if (n && e) ast_add_child(n, e);
        }
        expect_punct(p, ";");
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- break ; ---- */
    if (at_kw(p, "break")) {
        AstNode* n = mk(AST_BREAK);
        adv(p);                                        /* 'break' */
        expect_punct(p, ";");
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- continue ; ---- */
    if (at_kw(p, "continue")) {
        AstNode* n = mk(AST_CONTINUE);
        adv(p);                                        /* 'continue' */
        expect_punct(p, ";");
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- goto label ; ---- */
    if (at_kw(p, "goto")) {
        AstNode* n = mk(AST_GOTO);
        adv(p);                                        /* 'goto' */
        if (at(p, TK_ID)) {
            Tok* lbl = pk(p);
            if (n) s_copy_tok(n->name, (int)sizeof(n->name), lbl);
            adv(p);                                    /* label ident */
        } else {
            pdiag(p, "expected label after 'goto'");
        }
        expect_punct(p, ";");
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- label:  ident ':' stmt   (NOT a 'case'/'default' -- handled above,
     *      and NOT a typedef'd type used as a declaration). The lookahead is
     *      "identifier directly followed by ':'", excluding the "?:" use which
     *      can't appear as the first token of a statement. ---- */
    if (at(p, TK_ID)) {
        Tok* t2 = pk2(p);
        if (t2 && t2->kind == TK_PUNCT && tok_is(t2, ":")) {
            AstNode* n = mk(AST_LABEL);
            Tok* lbl = pk(p);
            if (n) s_copy_tok(n->name, (int)sizeof(n->name), lbl);
            adv(p);                                    /* label ident */
            adv(p);                                    /* ':' */
            /* The labeled statement (none if at '}' / EOF). */
            if (!at_punct(p, "}") && !at(p, TK_EOF)) {
                AstNode* st = parse_statement(p);
                if (n && st) ast_add_child(n, st);
            }
            if (n) set_span_to_here(p, n, start);
            return n ? n : mk_none(p, start);
        }
    }

    /* ---- declaration statement ---- */
    if (starts_declaration(p)) {
        AstNode* n = mk(AST_DECL_STMT);
        AstNode* d = parse_declaration(p);             /* consumes its own ';' */
        if (n && d) ast_add_child(n, d);
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }

    /* ---- expression statement:  expr ';' ---- */
    {
        AstNode* n = mk(AST_EXPR_STMT);
        int before = p->pos;
        AstNode* e = parse_expression(p);
        if (n && e) ast_add_child(n, e);

        if (at_punct(p, ";")) {
            adv(p);                                    /* terminating ';' */
        } else if (p->pos == before) {
            /* parse_expression made no progress on a token that starts no
             * statement: malformed input. Recover and emit a placeholder so
             * the caller advances. */
            recover_stmt(p, "expected statement");
            return mk_none(p, start);
        } else {
            /* Expression parsed but no ';' followed: diagnose and recover to a
             * statement boundary. */
            recover_stmt(p, "expected ';' after expression");
        }
        if (n) set_span_to_here(p, n, start);
        return n ? n : mk_none(p, start);
    }
}

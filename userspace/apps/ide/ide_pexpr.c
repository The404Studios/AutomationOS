/*
 * ide_pexpr.c -- expression layer of the recursive-descent C parser.
 *
 * Implements the two entry points declared in ide_parser.h:
 *   parse_assignment(p)  -- an assignment-expression (NO comma operator).
 *   parse_expression(p)  -- a full expression including the comma operator.
 *
 * Grammar is assembled bottom-up: primary -> postfix -> unary -> the binary
 * precedence-climbing ladder -> conditional (?:) -> assignment, then comma on
 * top. AST nodes are produced ONLY via ast_new/ast_add_child. Freestanding:
 * no libc, no malloc, no stdio; all helpers are static and prefixed px_.
 *
 * Cursor contract (see ide_parser.h): each production consumes exactly its
 * tokens and leaves the cursor on the first token after it; we never consume a
 * trailing ';'. Every loop makes forward progress; on an unexpected token we
 * pdiag() and return a best-effort node (AST_NONE placeholder) -- never NULL,
 * never an infinite loop.
 */
#include "ide_parser.h"

/* Hard caps so a malformed source can never make us run away. */
#define PX_MAX_ARGS    64
#define PX_MAX_OPERS   64

/* ----------------------------------------------------------------------- */
/* tiny local string helpers (no libc)                                     */
/* ----------------------------------------------------------------------- */

/* length of a NUL-terminated C string */
static int px_slen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* copy literal C string s into d (cap bytes incl. NUL), truncating safely */
static void px_strcpy(char* d, int cap, const char* s) {
    int i = 0;
    if (cap <= 0) return;
    if (s) {
        while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    }
    d[i] = 0;
}

/* copy a token's text into d (cap bytes incl. NUL), truncating to cap-1 */
static void px_tokcpy(char* d, int cap, Tok* t) {
    int i = 0, n;
    if (cap <= 0) return;
    if (!t || !t->s) { d[0] = 0; return; }
    n = t->len;
    if (n > cap - 1) n = cap - 1;
    for (; i < n; i++) d[i] = t->s[i];
    d[i] = 0;
}

/* ----------------------------------------------------------------------- */
/* The last token this parser actually consumed (the one before the cursor).
 * Used as the inclusive end token of a span. Falls back to the current token
 * when nothing has been consumed yet. */
static Tok* px_prev(Parser* p) {
    if (p->pos > 0 && p->toks) return &p->toks[p->pos - 1];
    return pk(p);
}

/* Set n->span to cover [start .. last-consumed], guarding NULLs. */
static void px_set_span(Parser* p, AstNode* n, Tok* start) {
    if (!n) return;
    if (!start) start = px_prev(p);
    n->span = span_of(p, start, px_prev(p));
}

/* ----------------------------------------------------------------------- */
/* forward decls (mutually recursive within this file)                     */
/* ----------------------------------------------------------------------- */
static AstNode* px_assignment(Parser* p);
static AstNode* px_conditional(Parser* p);
static AstNode* px_binary(Parser* p, int min_prec);
static AstNode* px_unary(Parser* p);
static AstNode* px_postfix(Parser* p);
static AstNode* px_primary(Parser* p);

/* Make a best-effort placeholder spanning the current token and advance once
 * so callers can always rely on forward progress. */
static AstNode* px_error_node(Parser* p, const char* msg) {
    Tok* t = pk(p);
    AstNode* n = ast_new(AST_NONE);
    pdiag(p, msg);
    if (n) n->span = span_of(p, t, t);
    if (!at(p, TK_EOF)) adv(p);   /* forward progress */
    return n;
}

/* ----------------------------------------------------------------------- */
/* primary                                                                 */
/* ----------------------------------------------------------------------- */
/*  TK_NUM/TK_STR/TK_CHAR -> AST_LITERAL (name = token text, <=63)
 *  TK_ID                  -> AST_IDENT   (name = ident)
 *  '(' expr ')'           -> grouped (parse_expression, expect ')')
 *  '{' assign,... '}'     -> AST_INIT_LIST
 */
static AstNode* px_primary(Parser* p) {
    Tok* t = pk(p);

    if (at(p, TK_NUM) || at(p, TK_STR) || at(p, TK_CHAR)) {
        AstNode* n = ast_new(AST_LITERAL);
        adv(p);
        if (!n) return px_error_node(p, "out of AST nodes");
        px_tokcpy(n->name, sizeof n->name, t);   /* truncates to 63 */
        n->span = span_of(p, t, t);
        return n;
    }

    if (at(p, TK_ID)) {
        AstNode* n = ast_new(AST_IDENT);
        adv(p);
        if (!n) return px_error_node(p, "out of AST nodes");
        px_tokcpy(n->name, sizeof n->name, t);
        n->span = span_of(p, t, t);
        return n;
    }

    if (at_punct(p, "(")) {
        AstNode* inner;
        Tok* open = pk(p);
        adv(p);                       /* '(' */
        inner = parse_expression(p);
        {
            Tok* close = pk(p);
            if (!at_punct(p, ")")) {
                pdiag(p, "expected ')' to close expression");
            } else {
                adv(p);               /* ')' */
            }
            if (inner) inner->span = span_of(p, open, close);
        }
        if (!inner) return px_error_node(p, "empty parenthesized expression");
        return inner;
    }

    if (at_punct(p, "{")) {
        Tok* open = pk(p);
        AstNode* lst = ast_new(AST_INIT_LIST);
        int count = 0;
        adv(p);                       /* '{' */
        if (!lst) return px_error_node(p, "out of AST nodes");
        if (!at_punct(p, "}")) {
            for (;;) {
                Tok* before = pk(p);
                if (at(p, TK_EOF) || at_punct(p, "}")) break;
                if (count < PX_MAX_OPERS) {
                    AstNode* e = px_assignment(p);
                    if (e) ast_add_child(lst, e);
                } else {
                    /* over cap: drain one element's worth of tokens */
                    (void)px_assignment(p);
                }
                count++;
                if (!eat_punct(p, ",")) break;
                /* trailing comma before '}' is allowed */
                if (at_punct(p, "}")) break;
                if (pk(p) == before) { adv(p); }   /* guarantee progress */
            }
        }
        {
            Tok* close = pk(p);
            if (!at_punct(p, "}")) {
                pdiag(p, "expected '}' to close initializer list");
                lst->span = span_of(p, open, open);
            } else {
                adv(p);               /* '}' */
                lst->span = span_of(p, open, close);
            }
        }
        return lst;
    }

    return px_error_node(p, "expected an expression");
}

/* ----------------------------------------------------------------------- */
/* postfix                                                                  */
/* ----------------------------------------------------------------------- */
/*  '(' args ')'  -> AST_CALL    (child0 = callee; name copied if callee IDENT)
 *  '[' expr ']'  -> AST_INDEX   (children: base, index)
 *  '.'/'->' id   -> AST_MEMBER  (name = member, type_str = "." / "->", child=base)
 *  '++'/'--'     -> AST_UNARY   (name = "post++"/"post--", child = base)
 */
static AstNode* px_postfix(Parser* p) {
    Tok* start = pk(p);
    AstNode* base = px_primary(p);

    for (;;) {
        if (at_punct(p, "(")) {                       /* call */
            AstNode* call = ast_new(AST_CALL);
            int argc = 0;
            adv(p);                                   /* '(' */
            if (!call) return base ? base : px_error_node(p, "out of AST nodes");
            ast_add_child(call, base);                /* child0 = callee */
            if (base && base->kind == AST_IDENT) {
                px_strcpy(call->name, sizeof call->name, base->name);
            }
            if (!at_punct(p, ")")) {
                for (;;) {
                    Tok* before = pk(p);
                    if (at(p, TK_EOF) || at_punct(p, ")")) break;
                    if (argc < PX_MAX_ARGS) {
                        AstNode* a = px_assignment(p);
                        if (a) ast_add_child(call, a);
                    } else {
                        (void)px_assignment(p);
                    }
                    argc++;
                    if (!eat_punct(p, ",")) break;
                    if (pk(p) == before) { adv(p); }  /* progress */
                }
            }
            if (!at_punct(p, ")")) {
                pdiag(p, "expected ')' to close call arguments");
            } else {
                adv(p);                               /* ')' */
            }
            px_set_span(p, call, start);              /* callee start .. ')' */
            base = call;
            continue;
        }

        if (at_punct(p, "[")) {                        /* index */
            AstNode* idx = ast_new(AST_INDEX);
            AstNode* inner;
            adv(p);                                    /* '[' */
            if (!idx) return base ? base : px_error_node(p, "out of AST nodes");
            ast_add_child(idx, base);                  /* base */
            inner = parse_expression(p);
            if (inner) ast_add_child(idx, inner);      /* index */
            if (!at_punct(p, "]")) {
                pdiag(p, "expected ']' to close subscript");
            } else {
                adv(p);                                /* ']' */
            }
            px_set_span(p, idx, start);
            base = idx;
            continue;
        }

        if (at_punct(p, ".") || at_punct(p, "->")) {   /* member */
            AstNode* mem = ast_new(AST_MEMBER);
            Tok* op = pk(p);
            Tok* id;
            adv(p);                                    /* '.' or '->' */
            if (!mem) return base ? base : px_error_node(p, "out of AST nodes");
            px_tokcpy(mem->type_str, sizeof mem->type_str, op);  /* "." / "->" */
            ast_add_child(mem, base);
            id = pk(p);
            if (at(p, TK_ID)) {
                px_tokcpy(mem->name, sizeof mem->name, id);
                adv(p);                                /* member ident */
            } else {
                pdiag(p, "expected member name after '.' or '->'");
            }
            px_set_span(p, mem, start);
            base = mem;
            continue;
        }

        if (at_punct(p, "++") || at_punct(p, "--")) {  /* post-inc/dec */
            AstNode* un = ast_new(AST_UNARY);
            Tok* op = pk(p);
            adv(p);
            if (!un) return base ? base : px_error_node(p, "out of AST nodes");
            if (op->len >= 2 && op->s && op->s[0] == '+')
                px_strcpy(un->name, sizeof un->name, "post++");
            else
                px_strcpy(un->name, sizeof un->name, "post--");
            ast_add_child(un, base);
            px_set_span(p, un, start);
            base = un;
            continue;
        }

        break;
    }

    if (!base) return px_error_node(p, "expected a postfix expression");
    return base;
}

/* ----------------------------------------------------------------------- */
/* unary / cast / sizeof                                                    */
/* ----------------------------------------------------------------------- */

/* prefix unary operator text -> 1 if it is one of ! ~ - + * & ++ -- */
static int px_is_prefix_unop(Tok* t) {
    if (!t || t->kind != TK_PUNCT) return 0;
    return    tok_is(t, "!")  || tok_is(t, "~") || tok_is(t, "-")
           || tok_is(t, "+")  || tok_is(t, "*") || tok_is(t, "&")
           || tok_is(t, "++") || tok_is(t, "--");
}

/* Consume a balanced "( typename )" starting at the current '(' and render the
 * type text into out (cap). Returns 1 on success (cursor left after ')'), 0 if
 * this is NOT a cast (cursor unchanged). We only treat it as a cast when the
 * token right after '(' is a typename per is_typename (the lexer hack). */
static int px_try_cast_type(Parser* p, char* out, int cap, Tok** open_out, Tok** close_out) {
    Tok* nxt = pk2(p);
    int depth;
    int started = 0;

    out[0] = 0;
    if (!at_punct(p, "(")) return 0;
    if (!nxt || !nxt->s) return 0;
    /* cast only if the thing after '(' begins a typename */
    if (!is_typename(p, nxt->s, nxt->len)) return 0;

    *open_out = pk(p);
    adv(p);                          /* '(' */

    /* Render tokens until the matching ')'. Accept type keywords, identifiers
     * (typedef'd names), qualifiers, '*', and '[' ']' array bits. */
    depth = 1;
    for (;;) {
        Tok* t = pk(p);
        if (at(p, TK_EOF)) break;
        if (at_punct(p, "(")) { depth++; }
        else if (at_punct(p, ")")) {
            depth--;
            if (depth == 0) break;
        }
        /* append token text with a single separating space */
        {
            int dl = px_slen(out);
            int i = 0;
            if (dl > 0 && dl < cap - 1) out[dl++] = ' ';
            while (t->s && i < t->len && dl < cap - 1) out[dl++] = t->s[i++];
            out[dl] = 0;
            started = 1;
        }
        adv(p);
    }
    *close_out = pk(p);
    if (at_punct(p, ")")) adv(p);    /* matching ')' */
    (void)started;
    return 1;                        /* committed: '(' typename ... consumed */
}

static AstNode* px_unary(Parser* p) {
    Tok* t = pk(p);

    /* sizeof  (either "sizeof expr" or "sizeof ( typename )") */
    if (at_kw(p, "sizeof")) {
        AstNode* so = ast_new(AST_SIZEOF);
        Tok* kw = pk(p);
        adv(p);                       /* sizeof */
        if (!so) return px_error_node(p, "out of AST nodes");
        /* sizeof ( typename ) */
        if (at_punct(p, "(")) {
            Tok* nxt = pk2(p);
            if (nxt && nxt->s && is_typename(p, nxt->s, nxt->len)) {
                char ty[96]; Tok* o = 0; Tok* c = 0;
                if (px_try_cast_type(p, ty, sizeof ty, &o, &c)) {
                    px_strcpy(so->type_str, sizeof so->type_str, ty);
                    px_set_span(p, so, kw);
                    return so;
                }
            }
        }
        /* otherwise sizeof applies to a unary expression */
        {
            AstNode* operand = px_unary(p);
            if (operand) ast_add_child(so, operand);
            px_set_span(p, so, kw);
            return so;
        }
    }

    /* cast: '(' typename ')' unary  -- only when the lookahead is a typename */
    if (at_punct(p, "(")) {
        Tok* nxt = pk2(p);
        if (nxt && nxt->s && is_typename(p, nxt->s, nxt->len)) {
            char ty[96]; Tok* open = 0; Tok* close = 0;
            int saved = p->pos;
            if (px_try_cast_type(p, ty, sizeof ty, &open, &close)) {
                AstNode* cast = ast_new(AST_CAST);
                AstNode* operand;
                if (!cast) { p->pos = saved; return px_postfix(p); }
                px_strcpy(cast->type_str, sizeof cast->type_str, ty);
                operand = px_unary(p);
                if (operand) ast_add_child(cast, operand);
                px_set_span(p, cast, open);          /* '(' .. operand end */
                return cast;
            }
            /* not a cast after all: fall through to grouped expr */
        }
        /* not a typename -> grouped expression handled by postfix/primary */
    }

    /* prefix unary operators */
    if (px_is_prefix_unop(t)) {
        AstNode* un = ast_new(AST_UNARY);
        AstNode* operand;
        Tok* op = pk(p);
        adv(p);                       /* operator */
        if (!un) return px_error_node(p, "out of AST nodes");
        px_tokcpy(un->name, sizeof un->name, op);
        operand = px_unary(p);
        if (operand) ast_add_child(un, operand);
        px_set_span(p, un, op);
        return un;
    }

    return px_postfix(p);
}

/* ----------------------------------------------------------------------- */
/* binary precedence climbing                                               */
/* ----------------------------------------------------------------------- */

/* Binary operator precedence (higher binds tighter). 0 == not a binary op.
 * Mirrors C: * / %  >  + -  >  << >>  >  relational  >  == !=  >  &  >  ^
 * >  |  >  &&  >  || . */
static int px_binop_prec(Tok* t) {
    if (!t || t->kind != TK_PUNCT) return 0;
    if (tok_is(t, "*") || tok_is(t, "/") || tok_is(t, "%")) return 11;
    if (tok_is(t, "+") || tok_is(t, "-"))                   return 10;
    if (tok_is(t, "<<") || tok_is(t, ">>"))                 return 9;
    if (tok_is(t, "<") || tok_is(t, "<=") ||
        tok_is(t, ">") || tok_is(t, ">="))                  return 8;
    if (tok_is(t, "==") || tok_is(t, "!="))                 return 7;
    if (tok_is(t, "&"))                                     return 6;
    if (tok_is(t, "^"))                                     return 5;
    if (tok_is(t, "|"))                                     return 4;
    if (tok_is(t, "&&"))                                    return 3;
    if (tok_is(t, "||"))                                    return 2;
    return 0;
}

/* Precedence climbing. All these binary operators are left-associative, so a
 * right operand is parsed with min_prec = prec+1. */
static AstNode* px_binary(Parser* p, int min_prec) {
    Tok* start = pk(p);
    AstNode* lhs = px_unary(p);
    int guard = 0;

    for (;;) {
        Tok* op = pk(p);
        int prec = px_binop_prec(op);
        AstNode* bin;
        AstNode* rhs;

        if (prec == 0 || prec < min_prec) break;
        if (++guard > (PX_MAX_OPERS * 64)) break;   /* paranoia: bounded */

        adv(p);                                     /* operator */
        rhs = px_binary(p, prec + 1);

        bin = ast_new(AST_BINARY);
        if (!bin) { return lhs; }
        px_tokcpy(bin->name, sizeof bin->name, op);
        ast_add_child(bin, lhs);
        if (rhs) ast_add_child(bin, rhs);
        px_set_span(p, bin, start);                 /* lhs start .. rhs end */
        lhs = bin;
    }

    if (!lhs) return px_error_node(p, "expected an expression");
    return lhs;
}

/* ----------------------------------------------------------------------- */
/* conditional  ( cond '?' expr ':' assignment )                           */
/* ----------------------------------------------------------------------- */
static AstNode* px_conditional(Parser* p) {
    Tok* start = pk(p);
    AstNode* cond = px_binary(p, 1);

    if (at_punct(p, "?")) {
        AstNode* tern = ast_new(AST_TERNARY);
        AstNode* then_e;
        AstNode* else_e;
        adv(p);                                     /* '?' */
        if (!tern) return cond ? cond : px_error_node(p, "out of AST nodes");
        ast_add_child(tern, cond);
        then_e = parse_expression(p);               /* between ? and : */
        if (then_e) ast_add_child(tern, then_e);
        if (at_punct(p, ":")) {
            adv(p);                                 /* ':' */
        } else {
            pdiag(p, "expected ':' in conditional expression");
        }
        else_e = px_assignment(p);                  /* right-assoc tail */
        if (else_e) ast_add_child(tern, else_e);
        px_set_span(p, tern, start);                /* cond start .. else end */
        return tern;
    }

    if (!cond) return px_error_node(p, "expected an expression");
    return cond;
}

/* ----------------------------------------------------------------------- */
/* assignment  ( right-associative )                                        */
/* ----------------------------------------------------------------------- */

/* 1 if t is one of the assignment operators. */
static int px_is_assign_op(Tok* t) {
    if (!t || t->kind != TK_PUNCT) return 0;
    return    tok_is(t, "=")   || tok_is(t, "+=") || tok_is(t, "-=")
           || tok_is(t, "*=")  || tok_is(t, "/=") || tok_is(t, "%=")
           || tok_is(t, "&=")  || tok_is(t, "|=") || tok_is(t, "^=")
           || tok_is(t, "<<=") || tok_is(t, ">>=");
}

static AstNode* px_assignment(Parser* p) {
    Tok* start = pk(p);
    AstNode* lhs = px_conditional(p);

    if (px_is_assign_op(pk(p))) {
        AstNode* as = ast_new(AST_ASSIGN);
        AstNode* rhs;
        Tok* op = pk(p);
        adv(p);                                     /* assignment operator */
        if (!as) return lhs ? lhs : px_error_node(p, "out of AST nodes");
        px_tokcpy(as->name, sizeof as->name, op);
        ast_add_child(as, lhs);
        rhs = px_assignment(p);                     /* right-associative */
        if (rhs) ast_add_child(as, rhs);
        px_set_span(p, as, start);                  /* lhs start .. rhs end */
        return as;
    }

    if (!lhs) return px_error_node(p, "expected an expression");
    return lhs;
}

/* ----------------------------------------------------------------------- */
/* public entry points                                                      */
/* ----------------------------------------------------------------------- */

AstNode* parse_assignment(Parser* p) {
    return px_assignment(p);
}

AstNode* parse_expression(Parser* p) {
    Tok* start = pk(p);
    AstNode* first = px_assignment(p);

    if (!at_punct(p, ",")) {
        if (!first) return px_error_node(p, "expected an expression");
        return first;
    }

    /* comma operator: gather a sequence under one AST_COMMA */
    {
        AstNode* comma = ast_new(AST_COMMA);
        int count = 0;
        if (!comma) return first ? first : px_error_node(p, "out of AST nodes");
        if (first) ast_add_child(comma, first);
        count = 1;
        while (at_punct(p, ",")) {
            AstNode* nxt;
            Tok* before;
            adv(p);                                 /* ',' */
            before = pk(p);
            if (count >= PX_MAX_OPERS) {
                (void)px_assignment(p);
            } else {
                nxt = px_assignment(p);
                if (nxt) ast_add_child(comma, nxt);
            }
            count++;
            if (pk(p) == before) { adv(p); }        /* forward progress */
        }
        px_set_span(p, comma, start);
        return comma;
    }
}

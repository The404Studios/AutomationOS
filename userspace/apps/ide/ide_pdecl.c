/*
 * ide_pdecl.c -- recursive-descent C parser: the DECLARATION layer.
 *
 * Handles the hardest grammar: type-specifier runs + declarators, pointers,
 * arrays, typedefs, struct/union/enum record definitions, function prototypes,
 * and full function definitions (delegating bodies to parse_compound).
 *
 * Freestanding: no libc, no malloc, no stdio. Private static string helpers,
 * static scratch buffers, AST nodes only via ast_*. Obeys the cursor-ownership
 * contract in ide_parser.h: each parse_X consumes exactly its construct and
 * leaves the cursor on the first token after it; declarations consume through
 * their terminating ';' (functions through '}'); on error we pdiag + recover
 * and always make forward progress, never returning NULL.
 */
#include "ide_parser.h"

/* Caps from the header contract. */
#define TYPE_CAP   96
#define NAME_CAP   64
#define MAX_PARAMS 32
#define MAX_FIELDS 32

/* ------------------------------------------------------------------ *
 *  Private freestanding string helpers (no libc).
 * ------------------------------------------------------------------ */

static int d_strlen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Append [s,len) to dst (cap-bounded, always NUL-terminated). No-op if full. */
static void d_append(char* dst, int cap, const char* s, int len) {
    if (!dst || cap <= 0 || !s) return;
    int n = 0;
    while (n < cap - 1 && dst[n]) n++;          /* find current end */
    int i = 0;
    while (i < len && n < cap - 1) dst[n++] = s[i++];
    dst[n] = '\0';
}

/* Append a NUL-terminated literal. */
static void d_append_str(char* dst, int cap, const char* s) {
    d_append(dst, cap, s, d_strlen(s));
}

/* Append a token's text. */
static void d_append_tok(char* dst, int cap, Tok* t) {
    if (t && t->s && t->len > 0) d_append(dst, cap, t->s, t->len);
}

/* Bounded copy of [s,len) -> dst. */
static void d_copyn(char* dst, int cap, const char* s, int len) {
    if (!dst || cap <= 0) return;
    int n = len;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) dst[i] = s ? s[i] : '\0';
    dst[n] = '\0';
}

static void d_setname(AstNode* nd, const char* s, int len) {
    if (nd) d_copyn(nd->name, (int)sizeof(nd->name), s, len);
}

static void d_settype(AstNode* nd, const char* s) {
    if (nd) d_copyn(nd->type_str, (int)sizeof(nd->type_str), s, d_strlen(s));
}

/* ------------------------------------------------------------------ *
 *  Type-specifier classification.
 *
 *  A type-specifier/qualifier/storage-class token is one of:
 *   - a registered typename (is_typename -> builtin or typedef/tag name), or
 *   - one of these keyword texts that the builtin seed list does NOT cover.
 *  We must accept the latter explicitly because parser_init only seeds the
 *  common builtins (not long/short/static/extern/struct/union/enum/...).
 * ------------------------------------------------------------------ */
static const char* const k_spec_words[] = {
    "void", "char", "short", "int", "long", "float", "double",
    "unsigned", "signed", "const", "volatile", "static", "extern",
    "inline", "register", "auto", "restrict", "_Bool", "bool",
    "struct", "union", "enum", 0
};

static int is_spec_word(Tok* t) {
    if (!t || !t->s || t->len <= 0) return 0;
    for (int i = 0; k_spec_words[i]; i++)
        if (tok_is(t, k_spec_words[i])) return 1;
    return 0;
}

/* True if the current token can start / continue a type-specifier run. */
static int at_type_spec(Parser* p) {
    Tok* t = pk(p);
    if (t->kind == TK_EOF) return 0;
    if (is_spec_word(t)) return 1;
    if ((t->kind == TK_TYPE || t->kind == TK_ID) &&
        is_typename(p, t->s, t->len))
        return 1;
    return 0;
}

/* True if current token is struct/union/enum. */
static int at_record_kw(Parser* p) {
    return at_kw(p, "struct") || at_kw(p, "union") || at_kw(p, "enum");
}

/* ------------------------------------------------------------------ *
 *  Forward decls for the record-body parser used by both
 *  parse_type_and_declarator (as a type) and parse_declaration (as a def).
 * ------------------------------------------------------------------ */
static void parse_record_body(Parser* p, AstNode* rec, int is_enum);

/* ------------------------------------------------------------------ *
 *  parse_type_and_declarator
 *
 *  Parse a leading run of type specifiers/qualifiers (including a
 *  struct/union/enum tag, possibly with an inline { ... } body), then a
 *  declarator: leading '*' (with optional 'const'), an identifier, and an
 *  array suffix '[ ... ]'. STOP on a '(' that follows the name -- the caller
 *  (parse_declaration) owns the function-declarator suffix.
 *
 *  Renders the type into type_out (cap TYPE_CAP) and the name into name_out
 *  (cap NAME_CAP). Returns AST_VAR_DECL with name/type_str set, or AST_NONE.
 * ------------------------------------------------------------------ */
AstNode* parse_type_and_declarator(Parser* p, char* type_out, char* name_out) {
    if (type_out) type_out[0] = '\0';
    if (name_out) name_out[0] = '\0';

    char ty[TYPE_CAP];
    char nm[NAME_CAP];
    ty[0] = '\0';
    nm[0] = '\0';

    Tok* first = pk(p);
    Tok* last  = first;

    /* ---- 1. type-specifier run --------------------------------------- */
    int got_spec = 0;
    while (!at(p, TK_EOF)) {
        if (at_record_kw(p)) {
            /* struct/union/enum [tag] [ { body } ] used as a TYPE. */
            Tok* kw = adv(p);
            if (ty[0]) d_append_str(ty, TYPE_CAP, " ");
            d_append_tok(ty, TYPE_CAP, kw);
            last = kw;
            int is_enum = tok_is(kw, "enum");
            /* optional tag */
            if (at(p, TK_ID) || at(p, TK_TYPE)) {
                Tok* tag = adv(p);
                d_append_str(ty, TYPE_CAP, " ");
                d_append_tok(ty, TYPE_CAP, tag);
                last = tag;
            }
            /* optional inline body (skip/record it so the cursor advances) */
            if (at_punct(p, "{")) {
                AstNode* rec = ast_new(AST_RECORD);
                parse_record_body(p, rec, is_enum);   /* consumes {...} */
                last = pk(p);
                if (p->ntoks > 0 && p->pos > 0) last = &p->toks[p->pos - 1];
            }
            got_spec = 1;
            continue;
        }
        if (at_type_spec(p)) {
            Tok* t = adv(p);
            if (ty[0]) d_append_str(ty, TYPE_CAP, " ");
            d_append_tok(ty, TYPE_CAP, t);
            last = t;
            got_spec = 1;
            continue;
        }
        /* Heuristic: an unknown identifier that BEGINS a declaration is a type
         * name when immediately followed by another identifier or '*'. This
         * covers typedefs from headers we never parsed (e.g. `enemy_t g_enemies`
         * where enemy_t is defined in an #include'd game.h). Consume exactly one
         * as the leading specifier, and remember it so later uses are known. */
        if (!got_spec && at(p, TK_ID)) {
            Tok* t2 = pk2(p);
            if (t2 && (t2->kind == TK_ID ||
                       (t2->kind == TK_PUNCT && tok_is(t2, "*")))) {
                Tok* t = adv(p);
                add_typename(p, t->s, t->len);
                d_append_tok(ty, TYPE_CAP, t);
                last = t;
                got_spec = 1;
                continue;
            }
        }
        break;
    }

    if (!got_spec) {
        /* No type at all -- not a declaration we can parse. */
        AstNode* none = ast_new(AST_NONE);
        if (type_out) d_copyn(type_out, TYPE_CAP, ty, d_strlen(ty));
        if (name_out) d_copyn(name_out, NAME_CAP, nm, d_strlen(nm));
        if (none) none->span = span_of(p, first, last);
        return none;
    }

    /* ---- 2. pointer(s) ----------------------------------------------- */
    while (at_punct(p, "*")) {
        Tok* star = adv(p);
        d_append_str(ty, TYPE_CAP, "*");
        last = star;
        /* pointer qualifiers: const/volatile/restrict after the '*' */
        while (at_kw(p, "const") || at_kw(p, "volatile") || at_kw(p, "restrict")) {
            Tok* q = adv(p);
            d_append_str(ty, TYPE_CAP, " ");
            d_append_tok(ty, TYPE_CAP, q);
            last = q;
        }
    }

    /* ---- 3. identifier (may be absent: abstract declarator) ----------
     * Take an ID, or a TYPE-tagged token that is no longer acting as a
     * specifier (defensive: shouldn't normally happen after the run above). */
    if (at(p, TK_ID) ||
        (at(p, TK_TYPE) && !at_record_kw(p) && !at_type_spec(p))) {
        Tok* id = adv(p);
        d_copyn(nm, NAME_CAP, id->s, id->len);
        last = id;
    }

    /* ---- 4. array suffix/suffixes '[ ... ]' -------------------------- */
    while (at_punct(p, "[")) {
        adv(p);                              /* '[' */
        if (!at_punct(p, "]")) {
            /* size expression: parse it (best-effort); value discarded. */
            int before = p->pos;
            parse_assignment(p);
            if (p->pos == before) {          /* no progress -> skip defensively */
                precover_to(p, "];");
            }
        }
        if (at_punct(p, "]")) {
            last = adv(p);
        } else {
            pdiag(p, "expected ']'");
            precover_to(p, "];");
            if (at_punct(p, "]")) last = adv(p);
        }
        d_append_str(ty, TYPE_CAP, "[]");
    }

    /* ---- emit -------------------------------------------------------- */
    AstNode* nd = ast_new(AST_VAR_DECL);
    if (nd) {
        d_setname(nd, nm, d_strlen(nm));
        d_settype(nd, ty);
        nd->span = span_of(p, first, last);
    }
    if (type_out) d_copyn(type_out, TYPE_CAP, ty, d_strlen(ty));
    if (name_out) d_copyn(name_out, NAME_CAP, nm, d_strlen(nm));
    return nd ? nd : ast_new(AST_NONE);
}

/* ------------------------------------------------------------------ *
 *  Record body parser.
 *
 *  Consumes '{' ... '}'. For struct/union: each entry is a
 *  parse_type_and_declarator -> AST_FIELD (consume trailing ';'). For enum:
 *  each entry is an identifier with optional '= const-expr' -> AST_ENUM_CONST.
 *  Bounded by MAX_FIELDS. Always terminates (forward progress guaranteed).
 * ------------------------------------------------------------------ */
static void parse_record_body(Parser* p, AstNode* rec, int is_enum) {
    if (!at_punct(p, "{")) return;
    Tok* open = adv(p);                       /* '{' */
    Tok* last = open;
    int count = 0;

    while (!at(p, TK_EOF) && !at_punct(p, "}")) {
        int before = p->pos;

        if (is_enum) {
            if (at(p, TK_ID) || at(p, TK_TYPE)) {
                Tok* id = adv(p);
                if (count < MAX_FIELDS) {
                    AstNode* ec = ast_new(AST_ENUM_CONST);
                    if (ec) {
                        d_setname(ec, id->s, id->len);
                        Tok* eend = id;
                        if (eat_punct(p, "=")) {
                            /* optional initializer: const-expr */
                            int b2 = p->pos;
                            parse_assignment(p);
                            if (p->pos == b2) precover_to(p, ",}");
                            if (p->ntoks > 0 && p->pos > 0)
                                eend = &p->toks[p->pos - 1];
                        }
                        ec->span = span_of(p, id, eend);
                        if (rec) ast_add_child(rec, ec);
                    }
                    count++;
                } else if (eat_punct(p, "=")) {
                    parse_assignment(p);
                }
                last = (p->ntoks > 0 && p->pos > 0) ? &p->toks[p->pos - 1] : id;
            } else {
                pdiag(p, "expected enumerator");
                precover_to(p, ",}");
            }
            eat_punct(p, ",");                /* separator (trailing OK) */
        } else {
            /* struct/union field: type + declarator, ';' terminated */
            char fty[TYPE_CAP];
            char fnm[NAME_CAP];
            AstNode* fld = parse_type_and_declarator(p, fty, fnm);
            if (fld && fld->kind != AST_NONE && count < MAX_FIELDS) {
                fld->kind = AST_FIELD;        /* reclassify the node */
                if (rec) ast_add_child(rec, fld);
                count++;
            }
            /* additional comma-declarators in one field line: int a, b; */
            while (eat_punct(p, ",")) {
                char fty2[TYPE_CAP];
                char fnm2[NAME_CAP];
                AstNode* f2 = parse_type_and_declarator(p, fty2, fnm2);
                if (f2 && f2->kind != AST_NONE && count < MAX_FIELDS) {
                    f2->kind = AST_FIELD;
                    if (rec) ast_add_child(rec, f2);
                    count++;
                }
            }
            if (!eat_punct(p, ";")) {
                pdiag(p, "expected ';' in record");
                precover_to(p, ";}");
                eat_punct(p, ";");
            }
            last = (p->ntoks > 0 && p->pos > 0) ? &p->toks[p->pos - 1] : open;
        }

        if (p->pos == before) adv(p);          /* forward progress */
    }

    if (at_punct(p, "}")) last = adv(p);        /* consume '}' */
    else { pdiag(p, "expected '}'"); precover_to(p, "}"); if (at_punct(p, "}")) last = adv(p); }

    if (rec) rec->span = span_of(p, open, last);
}

/* ------------------------------------------------------------------ *
 *  Parameter list parser.
 *
 *  Cursor must be on '('. Consumes '(' ... ')'. Each param is a
 *  parse_type_and_declarator -> AST_PARAM child of `fn`. Handles empty list,
 *  a single 'void', and a trailing '...'. Bounded by MAX_PARAMS.
 * ------------------------------------------------------------------ */
static void parse_param_list(Parser* p, AstNode* fn) {
    if (!eat_punct(p, "(")) return;
    int count = 0;

    /* (void) -> no params */
    if (at_kw(p, "void") && tok_is(pk2(p), ")")) {
        adv(p);                                 /* void */
        eat_punct(p, ")");
        return;
    }

    while (!at(p, TK_EOF) && !at_punct(p, ")")) {
        int before = p->pos;

        if (at_punct(p, "...")) {
            Tok* el = adv(p);
            if (count < MAX_PARAMS) {
                AstNode* pm = ast_new(AST_PARAM);
                if (pm) {
                    d_settype(pm, "...");
                    d_setname(pm, "", 0);
                    pm->span = span_of(p, el, el);
                    if (fn) ast_add_child(fn, pm);
                }
                count++;
            }
        } else {
            char pty[TYPE_CAP];
            char pnm[NAME_CAP];
            AstNode* pm = parse_type_and_declarator(p, pty, pnm);
            if (pm && pm->kind != AST_NONE) {
                pm->kind = AST_PARAM;
                if (count < MAX_PARAMS && fn) ast_add_child(fn, pm);
                count++;
            } else if (pm && pm->kind == AST_NONE) {
                /* couldn't parse a param: bail to ',' or ')' */
                precover_to(p, ",)");
            }
        }

        if (!at_punct(p, ")")) {
            if (!eat_punct(p, ",")) {
                /* not a comma and not ')': recover to one of them */
                precover_to(p, ",)");
                eat_punct(p, ",");
            }
        }

        if (p->pos == before) adv(p);           /* forward progress */
    }

    if (!eat_punct(p, ")")) {
        pdiag(p, "expected ')'");
        precover_to(p, ");{");
        eat_punct(p, ")");
    }
}

/* ------------------------------------------------------------------ *
 *  parse_declaration
 *
 *  A full declaration at TU or block scope. Always consumes through the
 *  terminating ';' (functions through their body '}'). Never returns NULL;
 *  on error returns a best-effort node after pdiag + recovery.
 * ------------------------------------------------------------------ */
AstNode* parse_declaration(Parser* p) {
    Tok* first = pk(p);

    /* ---- 1. typedef -------------------------------------------------- */
    if (at_kw(p, "typedef")) {
        adv(p);                                 /* 'typedef' */
        char ty[TYPE_CAP];
        char nm[NAME_CAP];
        parse_type_and_declarator(p, ty, nm);
        if (nm[0]) add_typename(p, nm, d_strlen(nm));

        AstNode* td = ast_new(AST_TYPEDEF);
        if (td) {
            d_setname(td, nm, d_strlen(nm));
            d_settype(td, ty);
        }
        Tok* semi = pk(p);
        if (!eat_punct(p, ";")) {
            pdiag(p, "expected ';' after typedef");
            precover_to(p, ";}");
            if (at_punct(p, ";")) { semi = pk(p); adv(p); }
        } else {
            if (p->ntoks > 0 && p->pos > 0) semi = &p->toks[p->pos - 1];
        }
        if (td) td->span = span_of(p, first, semi);
        return td ? td : ast_new(AST_NONE);
    }

    /* ---- 2. bare struct/union/enum record DEFINITION ----------------- *
     *  Pattern: struct|union|enum [tag] { ... } ;   (no declarator).
     *  If a declarator follows the '}', it's a normal decl of that type;
     *  we fall through to step 3 by NOT consuming here in that case.
     * ------------------------------------------------------------------ */
    if (at_record_kw(p)) {
        /* Look ahead: is this a "<kw> [tag] {" record body (a definition)? */
        Tok* kw = pk(p);
        int is_enum = tok_is(kw, "enum");
        Tok* la = pk2(p);
        int body_follows = 0;
        if (tok_is(la, "{")) {
            body_follows = 1;                   /* struct { ... }      */
        } else if ((la->kind == TK_ID || la->kind == TK_TYPE)) {
            /* struct Tag { ... } : peek past the tag for '{' */
            int save = p->pos;
            adv(p);                             /* kw */
            adv(p);                             /* tag */
            if (at_punct(p, "{")) body_follows = 1;
            p->pos = save;                      /* rewind */
        }

        if (body_follows) {
            adv(p);                             /* kw */
            AstNode* rec = ast_new(AST_RECORD);
            Tok* tag = 0;
            if (at(p, TK_ID) || at(p, TK_TYPE)) {
                tag = adv(p);
                if (rec) d_setname(rec, tag->s, tag->len);
                add_typename(p, tag->s, tag->len);   /* tag is now a typename */
            }
            parse_record_body(p, rec, is_enum);  /* consumes { ... } */

            /* A declarator may still follow: struct Foo {..} bar;  In that
             * case treat the record as the type of a normal var decl. */
            if (!at_punct(p, ";") && (at(p, TK_ID) || at_punct(p, "*"))) {
                /* render type = "<kw> <tag>" and parse the declarator tail */
                char ty[TYPE_CAP];
                char nm[NAME_CAP];
                ty[0] = '\0';
                d_append_tok(ty, TYPE_CAP, kw);
                if (tag) { d_append_str(ty, TYPE_CAP, " "); d_append_tok(ty, TYPE_CAP, tag); }
                /* declarator: pointers + name + array */
                while (at_punct(p, "*")) { adv(p); d_append_str(ty, TYPE_CAP, "*"); }
                nm[0] = '\0';
                if (at(p, TK_ID)) { Tok* id = adv(p); d_copyn(nm, NAME_CAP, id->s, id->len); }
                while (at_punct(p, "[")) {
                    adv(p);
                    if (!at_punct(p, "]")) { int b = p->pos; parse_assignment(p); if (p->pos == b) precover_to(p, "];"); }
                    eat_punct(p, "]");
                    d_append_str(ty, TYPE_CAP, "[]");
                }
                AstNode* vd = ast_new(AST_VAR_DECL);
                if (vd) { d_setname(vd, nm, d_strlen(nm)); d_settype(vd, ty); }
                Tok* semi = pk(p);
                if (!eat_punct(p, ";")) { pdiag(p, "expected ';'"); precover_to(p, ";}"); eat_punct(p, ";"); }
                if (p->ntoks > 0 && p->pos > 0) semi = &p->toks[p->pos - 1];
                if (vd) vd->span = span_of(p, first, semi);
                return vd ? vd : ast_new(AST_NONE);
            }

            /* plain record definition: expect trailing ';' */
            Tok* semi = pk(p);
            if (!eat_punct(p, ";")) {
                pdiag(p, "expected ';' after record");
                precover_to(p, ";}");
                if (at_punct(p, ";")) adv(p);
            }
            if (p->ntoks > 0 && p->pos > 0) semi = &p->toks[p->pos - 1];
            if (rec) rec->span = span_of(p, first, semi);
            return rec ? rec : ast_new(AST_NONE);
        }
        /* else: struct/union/enum used as a type -> fall through to step 3 */
    }

    /* ---- 3. normal declaration: type + declarator -------------------- */
    char ty[TYPE_CAP];
    char nm[NAME_CAP];
    AstNode* head = parse_type_and_declarator(p, ty, nm);

    if (head && head->kind == AST_NONE) {
        /* Not a declaration we could parse: diagnose + recover. */
        pdiag(p, "expected a declaration");
        precover_to(p, ";}");
        if (at_punct(p, ";")) adv(p);
        return head;                            /* AST_NONE placeholder */
    }

    /* ---- 3a. function: '(' params ')' -------------------------------- */
    if (at_punct(p, "(")) {
        AstNode* fn = ast_new(AST_FUNC_PROTO);  /* may upgrade to FUNC_DEF */
        if (fn) {
            d_setname(fn, nm, d_strlen(nm));
            d_settype(fn, ty);                  /* return type */
        }
        parse_param_list(p, fn);                /* consumes ( ... ) */

        if (at_punct(p, "{")) {
            /* FUNCTION DEFINITION: body via parse_compound. */
            if (fn) fn->kind = AST_FUNC_DEF;
            AstNode* body = parse_compound(p);  /* consumes { ... } */
            if (fn && body) ast_add_child(fn, body);
            Tok* endt = pk(p);
            if (p->ntoks > 0 && p->pos > 0) endt = &p->toks[p->pos - 1];
            if (fn) fn->span = span_of(p, first, endt);
            return fn ? fn : ast_new(AST_NONE);
        }

        /* PROTOTYPE: expect ';' */
        Tok* semi = pk(p);
        if (!eat_punct(p, ";")) {
            pdiag(p, "expected ';' after prototype");
            precover_to(p, ";}");
            if (at_punct(p, ";")) adv(p);
        }
        if (p->ntoks > 0 && p->pos > 0) semi = &p->toks[p->pos - 1];
        if (fn) fn->span = span_of(p, first, semi);
        return fn ? fn : ast_new(AST_NONE);
    }

    /* ---- 3b. variable / global -------------------------------------- */
    /* head is already AST_VAR_DECL with name/type. Optional initializer. */
    if (eat_punct(p, "=")) {
        int before = p->pos;
        AstNode* init = parse_assignment(p);
        if (init && head) ast_add_child(head, init);
        if (p->pos == before) precover_to(p, ",;}");
    }

    /* Additional comma-declarators: int a, b = 1, *c;  We richly model the
     * first (already in `head`) and skip the rest up to ';' for simplicity,
     * per the header's documented simplest strategy. */
    while (eat_punct(p, ",")) {
        char ty2[TYPE_CAP];
        char nm2[NAME_CAP];
        /* a comma-declarator shares the base specifiers; here we just parse a
         * pointer/name/array tail and discard, ensuring forward progress. */
        int before = p->pos;
        while (at_punct(p, "*")) adv(p);
        if (at(p, TK_ID)) adv(p);
        while (at_punct(p, "[")) {
            adv(p);
            if (!at_punct(p, "]")) { int b = p->pos; parse_assignment(p); if (p->pos == b) precover_to(p, "];"); }
            eat_punct(p, "]");
        }
        if (eat_punct(p, "=")) parse_assignment(p);
        (void)ty2; (void)nm2;
        if (p->pos == before) { precover_to(p, ",;}"); if (!at_punct(p, ",")) break; }
    }

    Tok* semi = pk(p);
    if (!eat_punct(p, ";")) {
        pdiag(p, "expected ';' after declaration");
        precover_to(p, ";}");
        if (at_punct(p, ";")) adv(p);
    }
    if (p->ntoks > 0 && p->pos > 0) semi = &p->toks[p->pos - 1];
    if (head) head->span = span_of(p, first, semi);
    return head ? head : ast_new(AST_NONE);
}

/*
 * ide_pcore.c -- recursive-descent C parser CORE.
 *
 * Implements the lifecycle (parser_init / parse_translation_unit), the shared
 * token cursor, and the typedef/type-name table declared in ide_parser.h.
 *
 * Freestanding: no libc, no malloc, no stdio. Static buffers + private str
 * helpers only. Obeys the cursor-ownership contract documented in the header.
 *
 * parse_declaration() and the ast_* arena entry points live in other TUs and
 * are resolved at link time; this file only declares/calls them.
 */
#include "ide_parser.h"

/* ------------------------------------------------------------------ *
 *  Private freestanding string helpers (no libc).
 * ------------------------------------------------------------------ */

static int p_strlen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int p_memcmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

/* Bounded copy: copy up to cap-1 bytes from [s,len), always NUL-terminate. */
static void p_copyn(char* dst, int cap, const char* s, int len) {
    if (cap <= 0) return;
    int n = len;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) dst[i] = s[i];
    dst[n] = '\0';
}

/* Bounded NUL-terminated string copy into dst[cap]. */
static void p_strcpy(char* dst, int cap, const char* s) {
    p_copyn(dst, cap, s, p_strlen(s));
}

/* 1 if char c appears in NUL-terminated set. */
static int p_inset(char c, const char* set) {
    if (!set) return 0;
    for (int i = 0; set[i]; i++)
        if (set[i] == c) return 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Fallback EOF token: keeps pk()/pk2() from ever returning NULL or
 *  reading out of range. kind TK_EOF, zero length, points at nothing.
 * ------------------------------------------------------------------ */
static Tok g_eof = { TK_EOF, "", 0, 0, 0 };

/* ------------------------------------------------------------------ *
 *  Cursor helpers.
 * ------------------------------------------------------------------ */

Tok* pk(Parser* p) {
    if (!p) return &g_eof;
    if (p->pos >= 0 && p->pos < p->ntoks) return &p->toks[p->pos];
    return &g_eof;
}

Tok* pk2(Parser* p) {
    if (!p) return &g_eof;
    int i = p->pos + 1;
    if (i >= 0 && i < p->ntoks) return &p->toks[i];
    return &g_eof;
}

int at(Parser* p, TokKind k) {
    return pk(p)->kind == k;
}

int tok_is(Tok* t, const char* s) {
    if (!t || !s) return 0;
    int n = p_strlen(s);
    if (n != t->len) return 0;
    if (n == 0) return 1;
    if (!t->s) return 0;
    return p_memcmp(t->s, s, n) == 0;
}

int at_punct(Parser* p, const char* s) {
    Tok* t = pk(p);
    return t->kind == TK_PUNCT && tok_is(t, s);
}

/* Text equality regardless of whether the lexer tagged it KW/TYPE/ID. */
int at_kw(Parser* p, const char* s) {
    return tok_is(pk(p), s);
}

Tok* adv(Parser* p) {
    Tok* t = pk(p);
    if (p && p->pos < p->ntoks) p->pos++;
    return t;
}

int eat_punct(Parser* p, const char* s) {
    if (at_punct(p, s)) {
        adv(p);
        return 1;
    }
    return 0;
}

void pdiag(Parser* p, const char* msg) {
    if (!p) return;
    if (p->ndiags >= PARSE_MAX_DIAGS) return;
    Tok* t = pk(p);
    Diag* d = &p->diags[p->ndiags++];
    d->line = t->line;
    d->col  = t->col;
    /* byte offset of the offending token into the source buffer */
    if (t->s && t != &g_eof) d->off = (int)(t->s - p->src);
    else                     d->off = p->src_len;
    p_strcpy(d->msg, (int)sizeof(d->msg), msg ? msg : "");
}

void expect_punct(Parser* p, const char* s) {
    if (at_punct(p, s)) {
        adv(p);
        return;
    }
    /* Build "expected '<s>'" into a bounded local buffer, then record it. */
    char buf[96];
    const char* pre = "expected '";
    int i = 0;
    for (int k = 0; pre[k] && i < (int)sizeof(buf) - 1; k++) buf[i++] = pre[k];
    if (s) {
        for (int k = 0; s[k] && i < (int)sizeof(buf) - 2; k++) buf[i++] = s[k];
    }
    if (i < (int)sizeof(buf) - 1) buf[i++] = '\'';
    buf[i] = '\0';
    pdiag(p, buf);
}

/* Skip tokens until one whose FIRST char is in stop_chars, or EOF.
 * Does NOT consume the stop token. Always terminates (advances on each
 * non-stop token; EOF and stop both break). */
void precover_to(Parser* p, const char* stop_chars) {
    if (!p) return;
    while (!at(p, TK_EOF)) {
        Tok* t = pk(p);
        char c = (t->len > 0 && t->s) ? t->s[0] : '\0';
        if (c != '\0' && p_inset(c, stop_chars)) break;
        adv(p);
    }
}

/* ------------------------------------------------------------------ *
 *  Type-name table ("lexer hack" support).
 * ------------------------------------------------------------------ */

int is_typename(Parser* p, const char* s, int len) {
    if (!p || !s || len <= 0) return 0;
    for (int i = 0; i < p->ntypes; i++) {
        const char* e = p->types[i];
        int el = p_strlen(e);
        if (el == len && p_memcmp(e, s, len) == 0) return 1;
    }
    return 0;
}

void add_typename(Parser* p, const char* s, int len) {
    if (!p || !s || len <= 0) return;
    if (len > 63) return;                 /* slot is char[64] */
    if (is_typename(p, s, len)) return;   /* dedupe */
    if (p->ntypes >= PARSE_MAX_TYPES) return;
    char* dst = p->types[p->ntypes++];
    p_copyn(dst, 64, s, len);
}

/* ------------------------------------------------------------------ *
 *  Span construction.
 * ------------------------------------------------------------------ */

Span span_of(Parser* p, Tok* a, Tok* b) {
    Span sp;
    sp.start_off = 0; sp.end_off = 0;
    sp.start_line = 0; sp.start_col = 0;
    sp.end_line = 0; sp.end_col = 0;

    /* NULL-safe: fall back so we never deref a bad pointer. */
    if (!a) a = b;
    if (!b) b = a;
    if (!a) return sp;            /* both NULL -> zeroed span */

    const char* base = p ? p->src : (const char*)0;

    sp.start_off  = (a->s && base) ? (int)(a->s - base) : 0;
    sp.start_line = a->line;
    sp.start_col  = a->col;

    sp.end_off  = (b->s && base) ? (int)(b->s - base) + b->len : sp.start_off;
    sp.end_line = b->line;
    sp.end_col  = b->col + b->len;
    return sp;
}

/* ------------------------------------------------------------------ *
 *  Lifecycle.
 * ------------------------------------------------------------------ */

/* Builtin type-name seeds. */
static const char* const k_builtin_types[] = {
    "void", "char", "short", "int", "long", "float", "double",
    "unsigned", "signed", "const", "volatile", "_Bool", "bool",
    "size_t", "ssize_t", "uintptr_t", "intptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "FILE", "va_list"
};

void parser_init(Parser* p, const char* src, int src_len, Tok* toks, int max_toks) {
    if (!p) return;
    p->src     = src;
    p->src_len = src_len;
    p->toks    = toks;
    p->ntoks   = lex_tokenize(src, src_len, toks, max_toks);
    if (p->ntoks < 0) p->ntoks = 0;
    /* Strip comment + preprocessor tokens so the grammar never trips over them.
     * Spans still index the original source, so comments survive in the text for
     * round-trip edits; the parser just doesn't see them. */
    {
        int w = 0;
        for (int r = 0; r < p->ntoks; r++) {
            if (toks[r].kind == TK_COMMENT || toks[r].kind == TK_PREPROC)
                continue;
            if (w != r) toks[w] = toks[r];
            w++;
        }
        if (w < max_toks) {
            toks[w].kind = TK_EOF;
            toks[w].s    = src + (src_len > 0 ? src_len : 0);
            toks[w].len  = 0;
            toks[w].line = 0;
            toks[w].col  = 0;
        }
        p->ntoks = w;
    }
    p->pos     = 0;
    p->ndiags  = 0;
    p->ntypes  = 0;

    int nbuiltin = (int)(sizeof(k_builtin_types) / sizeof(k_builtin_types[0]));
    for (int i = 0; i < nbuiltin; i++)
        add_typename(p, k_builtin_types[i], p_strlen(k_builtin_types[i]));
}

AstNode* parse_translation_unit(Parser* p) {
    ast_reset();
    AstNode* tu = ast_new(AST_TU);
    ast_set_root(tu);
    if (!p) return tu;

    Tok* first = pk(p);
    Tok* last  = first;

    while (!at(p, TK_EOF)) {
        if (eat_punct(p, ";")) continue;        /* stray top-level semicolons */

        int before = p->pos;
        AstNode* d = parse_declaration(p);
        /* A declaration may yield a ->next sibling chain (comma-declarators:
         * int a, b;) -- add every node so no declarator is dropped. */
        while (d) { AstNode* nx = d->next; if (tu) ast_add_child(tu, d); d = nx; }

        if (p->pos == before) adv(p);           /* guarantee forward progress */
    }

    /* End token = last real token (one before current pos), else first. */
    if (p->ntoks > 0) {
        int li = p->ntoks - 1;
        last = &p->toks[li];
    }
    if (tu) tu->span = span_of(p, first, last);
    return tu;
}

/*
 * ide_complete.c -- autocomplete candidate engine (see ide_complete.h).
 */
#include "ide.h"
#include "ide_complete.h"
#include "ide_library.h"
#include "ide_sys.h"

/* Editor insertion entry points (implemented in ide_editor.c). */
void ide_editor_apply_completion(struct Ide* a, const char* text,
                                 int is_snippet, int prefix_len);

/* Parallel metadata for the editor's ac_matches[] (same index space). */
static int g_kind[AC_MAX_MATCHES];
static int g_snip[AC_MAX_MATCHES];   /* library index for CK_SNIPPET, else -1 */

/* C keywords + types offered as completions (kept compact; the parser/lexer has
 * its own authoritative lists, but a local copy avoids a cross-module dep). */
static const char* const g_kw[] = {
    "int", "char", "void", "short", "long", "unsigned", "signed",
    "float", "double", "const", "static", "struct", "union", "enum",
    "typedef", "return", "if", "else", "for", "while", "do", "switch",
    "case", "break", "continue", "default", "goto", "sizeof", "extern",
    "volatile", "inline", "bool", "true", "false",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "size_t",
    0
};

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* case-insensitive: does `word` start with prefix[0..plen)? */
static int ci_prefix(const char* word, const char* pf, int plen) {
    for (int i = 0; i < plen; i++) {
        char w = word[i];
        if (!w) return 0;
        if (lc(w) != lc(pf[i])) return 0;
    }
    return 1;
}

/* Add one candidate if it matches the prefix and isn't a dup. */
static void try_add(Editor* e, const char* text, int kind, int snip,
                    const char* pf, int plen) {
    if (e->ac_count >= AC_MAX_MATCHES) return;
    if (!text || !text[0]) return;
    if (!ci_prefix(text, pf, plen)) return;
    int tl = ide_strlen(text);
    if (kind != CK_SNIPPET && tl == plen) return;   /* already fully typed */
    for (int j = 0; j < e->ac_count; j++)
        if (ide_streq(e->ac_matches[j], text)) return;   /* dedup */
    int k = 0;
    while (k < AC_WORD_CAP - 1 && text[k]) { e->ac_matches[e->ac_count][k] = text[k]; k++; }
    e->ac_matches[e->ac_count][k] = 0;
    g_kind[e->ac_count] = kind;
    g_snip[e->ac_count] = snip;
    e->ac_count++;
}

void complete_refresh(struct Ide* a) {
    Editor* e = &a->editor;
    Model*  m = &a->model;
    e->ac_count = 0;
    int plen = e->ac_prefix_len;
    int minpfx = (g_ac_minpfx < 1 ? 1 : g_ac_minpfx);   /* Settings knob */
    if (plen < minpfx) { e->ac_active = 0; return; }
    const char* pf = e->ac_prefix;

    /* 1. parameters of the focused function (most local / relevant). */
    if (m->focus >= 0 && m->focus < m->nfuncs) {
        Func* f = &m->funcs[m->focus];
        for (int i = 0; i < f->nparams && i < M_MAXPARAMS; i++)
            try_add(e, f->params[i].name, CK_PARAM, -1, pf, plen);
    }
    /* 2. file symbols. */
    for (int i = 0; i < m->nfuncs   && i < M_MAXFUNCS;   i++) try_add(e, m->funcs[i].name,   CK_FUNC,   -1, pf, plen);
    for (int i = 0; i < m->nglobals && i < M_MAXGLOBALS; i++) try_add(e, m->globals[i].name, CK_GLOBAL, -1, pf, plen);
    for (int i = 0; i < m->nprotos  && i < M_MAXPROTOS;  i++) try_add(e, m->protos[i].name,  CK_FUNC,   -1, pf, plen);
    for (int i = 0; i < m->nrecords && i < M_MAXRECORDS; i++) try_add(e, m->records[i].name, CK_TYPE,   -1, pf, plen);
    for (int i = 0; i < m->nmacros  && i < M_MAXMACROS;  i++) try_add(e, m->macros[i].name,  CK_MACRO,  -1, pf, plen);
    /* 3. C keywords / types. */
    for (int i = 0; g_kw[i]; i++) try_add(e, g_kw[i], CK_KEYWORD, -1, pf, plen);
    /* 4. library complex triggers (offer the snippet body on accept). */
    {
        int n = lib_count();
        for (int i = 0; i < n; i++) {
            const Snippet* s = lib_get(i);
            if (s) try_add(e, s->trigger, CK_SNIPPET, i, pf, plen);
        }
    }

    /* Clamp to the visible-rows Settings knob (AC_MAX_MATCHES is the array cap;
     * g_ac_visible just limits how many of the top matches we keep/show). */
    {
        int vis = g_ac_visible;
        if (vis < 1) vis = 1;
        if (vis > AC_MAX_MATCHES) vis = AC_MAX_MATCHES;
        if (e->ac_count > vis) e->ac_count = vis;
    }

    e->ac_active = (e->ac_count > 0);
    if (e->ac_sel >= e->ac_count) e->ac_sel = 0;
}

int complete_kind(int i) {
    if (i < 0 || i >= AC_MAX_MATCHES) return CK_KEYWORD;
    return g_kind[i];
}

const char* complete_preview(int i) {
    if (i < 0 || i >= AC_MAX_MATCHES) return 0;
    if (g_kind[i] != CK_SNIPPET) return 0;
    const Snippet* s = lib_get(g_snip[i]);
    return s ? s->body : 0;
}

void complete_accept(struct Ide* a) {
    Editor* e = &a->editor;
    if (!e->ac_active || e->ac_count == 0) return;
    if (e->ac_sel < 0 || e->ac_sel >= e->ac_count) return;   /* bounds guard */
    int kind = g_kind[e->ac_sel];
    if (kind == CK_SNIPPET) {
        const Snippet* s = lib_get(g_snip[e->ac_sel]);
        if (s) ide_editor_apply_completion(a, s->body, 1, e->ac_prefix_len);
    } else {
        ide_editor_apply_completion(a, e->ac_matches[e->ac_sel], 0, e->ac_prefix_len);
    }
    e->ac_active = 0;
    e->ac_count = 0;
}

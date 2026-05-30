/*
 * ide_parse.c -- AST-based semantic model builder for the IDE.
 *
 * Implements model_parse(): tokenize one C source buffer, parse it into a real
 * C AST (ide_pcore/pdecl/pstmt/pexpr.c), then WALK that tree to fill the Model:
 *   - top-level AST_VAR_DECL  -> Global
 *   - top-level AST_FUNC_DEF  -> Func (+ params, calls, global reads/writes)
 *
 * This replaces the old regex/token-heuristic model_parse with an accurate
 * tree walk. Freestanding: no libc, no malloc, no stdio. Static buffers only.
 * All helpers are static and prefixed mp_.
 */
#include "ide_model.h"
#include "ide_ast.h"
#include "ide_parser.h"
#include "ide_lex.h"
#include "ide_sys.h"

/* ---- static, bounded parse state (no malloc) ---- */
static Tok    toks[PARSE_MAX_TOKS];
static Parser P;

/* recursion guard so a pathological / corrupt tree can't blow the stack */
#define MP_MAX_DEPTH 256

/* ----------------------------------------------------------------------- */
/* per-function "seen" sets: ensures a Global's reader/writer fan-in count  */
/* is bumped at most ONCE per function.                                     */
/* ----------------------------------------------------------------------- */
typedef struct {
    char r[M_MAXREFS][M_NAME]; int nr;
    char w[M_MAXREFS][M_NAME]; int nw;
} SeenSet;

/* ----------------------------------------------------------------------- */
/* small list helpers                                                      */
/* ----------------------------------------------------------------------- */

/* does an M_NAME-name list already contain name?  (dedupe) */
static int mp_list_has(char list[][M_NAME], int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (ide_streq(list[i], name)) return 1;
    return 0;
}

/* append name to a capped M_NAME list if absent; returns 1 if newly added */
static int mp_list_add(char list[][M_NAME], int* n, int cap, const char* name) {
    if (!name || !name[0]) return 0;
    if (mp_list_has(list, *n, name)) return 0;
    if (*n >= cap) return 0;
    ide_strlcpy(list[*n], name, M_NAME);
    (*n)++;
    return 1;
}

static int mp_global_index(Model* m, const char* name) {
    for (int i = 0; i < m->nglobals; i++)
        if (ide_streq(m->globals[i].name, name)) return i;
    return -1;
}

/* ----------------------------------------------------------------------- */
/* the read/write classifier: a recursive AST walk that marks IDENT nodes   */
/* as global reads or writes depending on lvalue context.                   */
/* ----------------------------------------------------------------------- */
static void mp_classify(Model* m, Func* f, SeenSet* seen,
                        AstNode* n, int writing, int depth) {
    if (!n || depth > MP_MAX_DEPTH) return;

    switch (n->kind) {
    case AST_ASSIGN: {
        /* a = b / a += b : first child is the lvalue (written), rest read. */
        AstNode* c = n->first_child;
        if (c) {
            mp_classify(m, f, seen, c, 1, depth + 1);
            for (AstNode* k = c->next; k; k = k->next)
                mp_classify(m, f, seen, k, 0, depth + 1);
        }
        return;
    }
    case AST_UNARY: {
        /* ++/-- (pre or post) write their operand; everything else reads. */
        int isinc = ide_streq(n->name, "++")     || ide_streq(n->name, "--") ||
                    ide_streq(n->name, "post++") || ide_streq(n->name, "post--");
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, isinc ? writing : 0, depth + 1);
        return;
    }
    case AST_INDEX:
    case AST_MEMBER: {
        /* base keeps the writing context; any index/remaining are reads.   */
        AstNode* c = n->first_child;
        if (c) {
            mp_classify(m, f, seen, c, writing, depth + 1);
            for (AstNode* k = c->next; k; k = k->next)
                mp_classify(m, f, seen, k, 0, depth + 1);
        }
        return;
    }
    case AST_IDENT: {
        /* a known global reference: record read or write on the func, and
         * bump the global's fan-in at most once per function. */
        const char* nm = n->name;
        int gx = mp_global_index(m, nm);
        if (gx >= 0) {
            if (writing) {
                mp_list_add(f->writes, &f->nwrites, M_MAXREFS, nm);
                if (mp_list_add(seen->w, &seen->nw, M_MAXREFS, nm))
                    m->globals[gx].nwriters++;
            } else {
                mp_list_add(f->reads, &f->nreads, M_MAXREFS, nm);
                if (mp_list_add(seen->r, &seen->nr, M_MAXREFS, nm))
                    m->globals[gx].nreaders++;
            }
        }
        /* an identifier may still have children (rare); treat as reads. */
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, 0, depth + 1);
        return;
    }
    default:
        /* generic node: descend, no write context propagated. */
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, 0, depth + 1);
        return;
    }
}

/* ----------------------------------------------------------------------- */
/* deep walk of a function body to collect CALLS (the classifier above      */
/* handles reads/writes separately so the two concerns stay clean).         */
/* ----------------------------------------------------------------------- */
static void mp_collect_calls(Func* f, AstNode* n, int depth) {
    if (!n || depth > MP_MAX_DEPTH) return;

    if (n->kind == AST_CALL) {
        const char* nm = n->name;
        /* fall back to a simple callee identifier if name is empty */
        if ((!nm || !nm[0]) && n->first_child &&
            n->first_child->kind == AST_IDENT)
            nm = n->first_child->name;
        if (nm && nm[0])
            mp_list_add(f->calls, &f->ncalls, M_MAXCALLS, nm);
    }

    for (AstNode* c = n->first_child; c; c = c->next)
        mp_collect_calls(f, c, depth + 1);
}

/* ----------------------------------------------------------------------- */
/* PASS 1: top-level globals                                               */
/* ----------------------------------------------------------------------- */
static void mp_pass_globals(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind != AST_VAR_DECL) continue;
        if (m->nglobals >= M_MAXGLOBALS) break;
        if (!c->name[0]) continue;
        if (mp_global_index(m, c->name) >= 0) continue;   /* dedupe by name */

        Global* g = &m->globals[m->nglobals];
        ide_strlcpy(g->name, c->name, M_NAME);
        ide_strlcpy(g->type, c->type_str, M_TYPE);
        ide_strlcpy(g->file, m->cur_file, M_NAME);
        g->nreaders = 0;
        g->nwriters = 0;
        m->nglobals++;
    }
}

/* ----------------------------------------------------------------------- */
/* PASS 2: top-level function definitions                                  */
/* ----------------------------------------------------------------------- */
static void mp_pass_funcs(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind != AST_FUNC_DEF) continue;
        if (m->nfuncs >= M_MAXFUNCS) break;

        Func* f = &m->funcs[m->nfuncs];

        /* header */
        ide_strlcpy(f->name, c->name, M_NAME);
        ide_strlcpy(f->ret,  c->type_str, M_TYPE);
        ide_strlcpy(f->file, m->cur_file, M_NAME);
        f->line_start = c->span.start_line;
        f->line_end   = c->span.end_line;

        f->nparams = 0;
        f->ncalls  = 0;
        f->nreads  = 0;
        f->nwrites = 0;
        f->nports  = 0;
        f->closed  = 0;

        /* params + locate the body among the children */
        AstNode* body = 0;
        for (AstNode* k = c->first_child; k; k = k->next) {
            if (k->kind == AST_PARAM) {
                if (f->nparams < M_MAXPARAMS) {
                    Param* p = &f->params[f->nparams];
                    ide_strlcpy(p->type, k->type_str, M_TYPE);
                    ide_strlcpy(p->name, k->name, M_NAME);
                    f->nparams++;
                }
            } else if (k->kind == AST_COMPOUND) {
                body = k;            /* the function body */
            }
        }

        /* deep walk the body: calls, then reads/writes of globals */
        if (body) {
            SeenSet seen;
            seen.nr = 0;
            seen.nw = 0;
            mp_collect_calls(f, body, 0);
            mp_classify(m, f, &seen, body, 0, 0);
        }

        m->nfuncs++;
    }
}

/* ----------------------------------------------------------------------- */
/* entry point                                                             */
/* ----------------------------------------------------------------------- */
void model_parse(Model* m, const char* src, int len, const char* filename) {
    if (!m) return;

    /* 1. reset model */
    m->nfuncs   = 0;
    m->nglobals = 0;
    m->nconns   = 0;
    m->nrisks   = 0;
    m->nactions = 0;
    m->nflow    = 0;
    m->focus    = -1;
    m->coherence = 0;
    m->analyzed  = 0;
    m->lexed     = 0;
    m->parsed    = 0;

    /* line count = number of '\n' + 1 (empty buffer -> 1 line) */
    int lines = 1;
    if (src && len > 0) {
        for (int i = 0; i < len; i++)
            if (src[i] == '\n') lines++;
    }
    m->total_lines = lines;

    /* basename of filename after the last '/' */
    const char* base = filename ? filename : "";
    if (filename) {
        for (const char* p = filename; *p; p++)
            if (*p == '/') base = p + 1;
    }
    ide_strlcpy(m->cur_file, base, M_NAME);

    /* nothing to parse: still produce a consistent (empty) model */
    if (!src || len <= 0) {
        m->lexed  = 1;
        m->parsed = 1;
        return;
    }

    /* 2. parse to an AST */
    parser_init(&P, src, len, toks, PARSE_MAX_TOKS);
    m->lexed = 1;
    AstNode* root = parse_translation_unit(&P);

    /* 3 & 4: walk the tree (robust if root is NULL or the arena overflowed) */
    if (root) {
        mp_pass_globals(m, root);   /* PASS 1: globals first (so func walk
                                       can recognise global references)     */
        mp_pass_funcs(m, root);     /* PASS 2: functions + bodies           */
    }

    /* 5. status */
    m->parsed = 1;
    /* NOTE: model_analyze() is called by the caller next; do not call here.
     * focus was reset to -1 above; the caller sets it afterward. */
}

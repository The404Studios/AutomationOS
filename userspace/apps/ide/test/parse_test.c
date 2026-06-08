/*
 * parse_test.c -- HOST-side test harness for the IDE recursive-descent C parser.
 *
 * NOT part of the OS. Compiled with normal host gcc under WSL Arch to prove the
 * parser is correct. Reads the towerdefense sample files, parses each, dumps the
 * AST, prints function/global counts, runs targeted assertions on tower.c, and
 * fires a battery of malformed/degenerate inputs at the parser to prove it never
 * crashes or hangs.
 *
 * Build (from repo root):
 *   gcc -std=gnu11 -O1 -w \
 *     userspace/apps/ide/test/parse_test.c \
 *     userspace/apps/ide/ide_lex.c userspace/apps/ide/ide_ast.c \
 *     userspace/apps/ide/ide_pcore.c userspace/apps/ide/ide_pdecl.c \
 *     userspace/apps/ide/ide_pstmt.c userspace/apps/ide/ide_pexpr.c \
 *     userspace/apps/ide/ide_astprint.c -o /tmp/parse_test && /tmp/parse_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ide_parser.h"
#include "../ide_astprint.h"

/* ------------------------------------------------------------------ */
/* test bookkeeping                                                    */
/* ------------------------------------------------------------------ */
static int g_pass = 0, g_fail = 0;

static void check(int cond, const char* what) {
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* shared token buffer + AST dump scratch (kept off the stack) */
static Tok  g_toks[PARSE_MAX_TOKS];
static char g_dump[1 << 18];   /* 256 KiB AST text dump */

/* ------------------------------------------------------------------ */
/* host file slurp                                                     */
/* ------------------------------------------------------------------ */
static char* read_file(const char* path, int* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { if (out_len) *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); if (out_len) *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); if (out_len) *out_len = 0; return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = (int)got;
    return buf;
}

/* ------------------------------------------------------------------ */
/* AST walking helpers (host-side, depth-bounded)                      */
/* ------------------------------------------------------------------ */

/* Count direct children of `n` of a given kind. */
static int count_direct_children(AstNode* n, AstKind kind) {
    int c = 0;
    if (!n) return 0;
    for (AstNode* ch = n->first_child; ch; ch = ch->next)
        if (ch->kind == kind) c++;
    return c;
}

/* Recursively search the subtree rooted at `n` for any node of `kind`
 * whose name == want (or any name if want==NULL). Depth-bounded. */
static AstNode* find_kind_named(AstNode* n, AstKind kind,
                                const char* want, int depth) {
    if (!n || depth > 512) return NULL;
    if (n->kind == kind && (!want || strcmp(n->name, want) == 0))
        return n;
    for (AstNode* ch = n->first_child; ch; ch = ch->next) {
        AstNode* hit = find_kind_named(ch, kind, want, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* per-file parse + report                                             */
/* ------------------------------------------------------------------ */
static AstNode* parse_one(const char* path, Parser* P) {
    int len = 0;
    char* src = read_file(path, &len);
    if (!src) {
        printf("  !! could not open %s\n", path);
        return NULL;
    }
    ast_reset();
    parser_init(P, src, len, g_toks, PARSE_MAX_TOKS);
    AstNode* root = parse_translation_unit(P);
    /* NOTE: src buffer must outlive AST use only insofar as spans point into it;
     * we keep it alive until after dumping below. We intentionally leak it (host
     * process is short-lived) to keep span pointers valid. */
    return root;
}

static void report_file(const char* name, const char* path) {
    static Parser P;
    printf("\n========== %s ==========\n", name);
    AstNode* root = parse_one(path, &P);
    if (!root) return;

    int nfunc   = count_direct_children(root, AST_FUNC_DEF);
    int nproto  = count_direct_children(root, AST_FUNC_PROTO);
    int nglobal = count_direct_children(root, AST_VAR_DECL);
    int nrecord = count_direct_children(root, AST_RECORD);
    int ntypedef= count_direct_children(root, AST_TYPEDEF);

    /* truncated AST dump */
    int wrote = astprint_tree(root, g_dump, (int)sizeof(g_dump));
    /* print only the first ~2400 chars so the log stays readable */
    int show = wrote;
    if (show > 2400) show = 2400;
    char saved = g_dump[show];
    g_dump[show] = '\0';
    printf("--- AST (truncated, %d/%d bytes) ---\n%s", show, wrote, g_dump);
    if (wrote > show) printf("... [%d more bytes]\n", wrote - show);
    g_dump[show] = saved;

    printf("--- counts: funcs(AST_FUNC_DEF)=%d  protos=%d  globals(AST_VAR_DECL)=%d  records=%d  typedefs=%d  diags=%d  nodes=%d arena_full=%d ---\n",
           nfunc, nproto, nglobal, nrecord, ntypedef, P.ndiags,
           ast_node_count(), ast_arena_full());
}

/* ------------------------------------------------------------------ */
/* targeted assertions for tower.c                                     */
/* ------------------------------------------------------------------ */
static void assert_tower(const char* path) {
    static Parser P;
    printf("\n========== ASSERTIONS: tower.c ==========\n");
    AstNode* root = parse_one(path, &P);
    if (!root) { check(0, "tower.c parsed (root non-NULL)"); return; }
    check(root->kind == AST_TU, "root is AST_TU");

    AstNode* fn = ast_find_func("tower_tick");
    check(fn != NULL, "AST_FUNC_DEF named 'tower_tick' exists (ast_find_func)");
    if (!fn) {
        /* dump to help the parser authors */
        int wrote = astprint_tree(root, g_dump, (int)sizeof(g_dump));
        if (wrote > 3000) { g_dump[3000] = '\0'; }
        printf("  --- AST dump excerpt (no tower_tick) ---\n%s\n", g_dump);
        return;
    }
    check(fn->kind == AST_FUNC_DEF, "tower_tick node kind == AST_FUNC_DEF");

    AstNode* call = find_kind_named(fn, AST_CALL, "spawn_bullet", 0);
    check(call != NULL, "tower_tick body contains AST_CALL 'spawn_bullet'");

    AstNode* ge = find_kind_named(fn, AST_IDENT, "g_enemies", 0);
    check(ge != NULL, "tower_tick body contains AST_IDENT 'g_enemies'");

    AstNode* gb = find_kind_named(fn, AST_IDENT, "g_bullets", 0);
    check(gb != NULL, "tower_tick body contains AST_IDENT 'g_bullets'");

    /* If any failed, dump the tower_tick subtree to aid debugging. */
    if (!call || !ge || !gb) {
        int wrote = astprint_tree(fn, g_dump, (int)sizeof(g_dump));
        if (wrote > 4000) { g_dump[4000] = '\0'; }
        printf("  --- tower_tick subtree dump (assertion failed) ---\n%s\n", g_dump);
    }
}

/* ------------------------------------------------------------------ */
/* robustness: malformed / degenerate inputs must not crash or hang    */
/* ------------------------------------------------------------------ */
static void robust_one(const char* label, const char* src) {
    static Parser P;
    int len = (int)strlen(src);
    ast_reset();
    parser_init(&P, src, len, g_toks, PARSE_MAX_TOKS);
    AstNode* root = parse_translation_unit(&P);
    /* touch the result so the optimizer can't elide the call */
    int n = root ? ast_node_count() : -1;
    printf("  robust[%-22s] -> root=%p nodes=%d diags=%d\n",
           label, (void*)root, n, P.ndiags);
}

static void robustness_tests(void) {
    printf("\n========== ROBUSTNESS (must not crash/hang) ==========\n");
    robust_one("empty",        "");
    robust_one("int",          "int");
    robust_one("void f(",      "void f(");
    robust_one("struct {",     "struct {");
    robust_one("int x = ;",    "int x = ;");
    robust_one("if(",          "if(");

    /* 200-line deeply-nested-braces blob */
    {
        static char blob[8192];
        int p = 0;
        p += snprintf(blob + p, sizeof(blob) - p, "int deep(void) {\n");
        int depth = 0;
        for (int i = 0; i < 100 && p < (int)sizeof(blob) - 16; i++) {
            blob[p++] = '{'; blob[p++] = '\n'; depth++;
        }
        for (int i = 0; i < 95 && p < (int)sizeof(blob) - 16; i++) {
            blob[p++] = '}'; blob[p++] = '\n';
        }
        p += snprintf(blob + p, sizeof(blob) - p, "return 0;\n}\n");
        blob[p] = '\0';
        robust_one("deep-nested-braces", blob);
    }

    /* a few extra nasties for good measure */
    robust_one("unterminated-str", "char* s = \"abc");
    robust_one("comment-eof",      "int x; /* unterminated");
    robust_one("only-punct",       "(((((((((((");
    robust_one("garbage",          "@@@ ### $$$ %%%");

    printf("no-crash OK\n");
}

/* ------------------------------------------------------------------ */
int main(void) {
    const char* dir = "userspace/apps/ide/sample/towerdefense/";
    static char path[512];
    const char* files[] = {
        "tower.c", "enemy.c", "bullet.c", "wave.c", "main.c", "renderer.c"
    };
    int nf = (int)(sizeof(files) / sizeof(files[0]));

    printf("### IDE parser host test harness ###\n");
    printf("PARSE_MAX_TOKS=%d  sizeof(AstNode)=%zu\n",
           PARSE_MAX_TOKS, sizeof(AstNode));

    for (int i = 0; i < nf; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, files[i]);
        report_file(files[i], path);
    }

    snprintf(path, sizeof(path), "%stower.c", dir);
    assert_tower(path);

    robustness_tests();

    printf("\n========== SUMMARY ==========\n");
    printf("assertions: %d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "ALL ASSERTIONS PASSED" : "SOME ASSERTIONS FAILED");
    return g_fail == 0 ? 0 : 1;
}

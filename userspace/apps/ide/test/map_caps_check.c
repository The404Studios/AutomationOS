/*
 * map_caps_check.c -- HOST-side proof that the IDE's Semantic LEGO Map renders a
 * REAL game source file COMPLETELY (no silent truncation).
 *
 * NOT shipped. Built with host gcc (WSL Arch); links the SAME engine TUs the
 * on-device IDE uses (lexer + AST + recursive-descent parser + the ide_parse.c
 * model builder). It runs TWO layers on a file:
 *   (1) parse layer  -- parser_init + parse_translation_unit: reports token
 *                       count vs PARSE_MAX_TOKS + AST node count vs the arena,
 *                       so a truncated token stream (the root cause of a cut-off
 *                       map) is visible.
 *   (2) model layer  -- model_parse(): reports the Semantic LEGO Map's actual
 *                       funcs/globals/macros/includes/records counts vs their
 *                       M_MAX* caps, flagging any that hit the cap (= dropped
 *                       entries -> an incomplete "mental image").
 *
 * Exit 0 only if EVERYTHING fits (no token truncation, arena not full, and no
 * model array at its cap). Run (from repo root, in Arch WSL):
 *   gcc -I userspace/apps/ide -o /tmp/map_caps_check \
 *       userspace/apps/ide/test/map_caps_check.c \
 *       userspace/apps/ide/ide_parse.c userspace/apps/ide/ide_pcore.c \
 *       userspace/apps/ide/ide_pdecl.c userspace/apps/ide/ide_pstmt.c \
 *       userspace/apps/ide/ide_pexpr.c userspace/apps/ide/ide_lex.c \
 *       userspace/apps/ide/ide_ast.c
 *   /tmp/map_caps_check userspace/apps/deadzone/deadzone.c
 */
#include <stdio.h>
#include <string.h>

#include "../ide_parser.h"
#include "../ide_model.h"

/* ide_parse.c pulls these tiny string helpers from ide_sys.c, which also carries
 * syscall file IO that can't link on the host -- so stub just the three here,
 * matching ide_sys.c's contracts (ide_streq returns 1 on equal). */
int  ide_strlen(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
int  ide_streq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
void ide_strlcpy(char* d, const char* s, int cap) {
    if (!d || cap <= 0) return;
    int i = 0; if (s) while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static Tok   g_toks[PARSE_MAX_TOKS];
static char  g_src[1 << 18];           /* 256 KiB source slurp */
static Model g_model;                  /* big -- keep off the stack */

static int find_func(const char* name) {
    for (int i = 0; i < g_model.nfuncs; i++)
        if (strcmp(g_model.funcs[i].name, name) == 0) return i;
    return -1;
}
static int find_global(const char* name) {
    for (int i = 0; i < g_model.nglobals; i++)
        if (strcmp(g_model.globals[i].name, name) == 0) return i;
    return -1;
}

static int count_kids(AstNode* n, AstKind k) {
    int c = 0;
    if (!n) return 0;
    for (AstNode* ch = n->first_child; ch; ch = ch->next)
        if (ch->kind == k) c++;
    return c;
}

/* Print "<label>: <n> / cap <cap>  [FULL|*** AT CAP -> TRUNCATED ***]" and
 * return 1 if it is truncated (n >= cap). */
static int line(const char* label, int n, int cap) {
    int trunc = (n >= cap);
    printf("  %-22s %5d / cap %-6d %s\n", label, n, cap,
           trunc ? "*** AT CAP -> TRUNCATED ***" : "ok");
    return trunc;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.c>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    int n = (int)fread(g_src, 1, sizeof(g_src) - 1, f);
    fclose(f);
    g_src[n] = '\0';

    printf("=== map_caps_check: %s (%d bytes) ===\n", argv[1], n);

    int bad = 0;

    /* (1) parse layer -------------------------------------------------- */
    static Parser P;
    ast_reset();
    parser_init(&P, g_src, n, g_toks, PARSE_MAX_TOKS);
    AstNode* root = parse_translation_unit(&P);
    int arena_full = ast_arena_full();
    printf("[parse layer]\n");
    bad |= line("tokens", P.ntoks, PARSE_MAX_TOKS);
    printf("  %-22s %5d        %s\n", "ast_nodes", ast_node_count(),
           arena_full ? "*** ARENA FULL ***" : "ok");
    if (arena_full) bad = 1;
    printf("  %-22s %5d  (parse diags %d)\n", "ast funcs (direct)",
           count_kids(root, AST_FUNC_DEF), P.ndiags);

    /* (2) model layer (what the LEGO map actually renders) ------------- */
    model_parse(&g_model, g_src, n, "deadzone.c");
    printf("[model layer  -- the Semantic LEGO Map]\n");
    bad |= line("funcs",    g_model.nfuncs,    M_MAXFUNCS);
    bad |= line("globals",  g_model.nglobals,  M_MAXGLOBALS);
    bad |= line("macros",   g_model.nmacros,   M_MAXMACROS);
    bad |= line("includes", g_model.nincludes, M_MAXINCLUDES);
    bad |= line("records",  g_model.nrecords,  M_MAXRECORDS);
    bad |= line("protos",   g_model.nprotos,   M_MAXPROTOS);
    printf("  %-22s %5d\n", "total_lines", g_model.total_lines);

    /* (3) model CORRECTNESS spot-checks (parser got_core + caps fixes) --------
     * Only meaningful for deadzone.c; harmless elsewhere (just prints "absent"). */
    printf("[model correctness -- deadzone.c parser/caps checks]\n");
    /* a real function the storage-class+unknown-typedef bug used to DROP */
    int have_cam = find_func("camera_view") >= 0;
    printf("  %-30s %s\n", "func camera_view present", have_cam ? "YES" : "NO  <-- DROPPED");
    /* globals that used to be renamed to their TYPE / swallowed by dedup */
    int gz = find_global("g_zmesh"), gw = find_global("g_world"), gl = find_global("g_lowz");
    printf("  %-30s zmesh=%s world=%s lowz=%s\n", "globals (correct names)",
           gz>=0?"YES":"no", gw>=0?"YES":"no", gl>=0?"YES":"no");
    /* phantom globals = a TYPE word leaked in as a global NAME */
    const char* phantom[] = {"static","mat4","g3d_tri","g3d_i32","vec3","void",0};
    int nphantom = 0;
    for (int i = 0; phantom[i]; i++) if (find_global(phantom[i]) >= 0) {
        printf("  PHANTOM GLOBAL: \"%s\"\n", phantom[i]); nphantom++;
    }
    printf("  %-30s %d\n", "phantom (type-as-name) globals", nphantom);
    /* per-function call cap: _start used to truncate at M_MAXCALLS=16 */
    int si = find_func("_start");
    if (si >= 0) printf("  %-30s ncalls=%d (cap %d)  nreads=%d nwrites=%d\n",
                        "_start edges", g_model.funcs[si].ncalls, M_MAXCALLS,
                        g_model.funcs[si].nreads, g_model.funcs[si].nwrites);

    /* phantom globals (a TYPE word as a global NAME) are ALWAYS a parser bug, on
     * any file. The camera_view/g_world positive checks are deadzone-specific. */
    int is_deadzone = (find_func("deadzone_reset") >= 0) || (find_func("zombies_step") >= 0);
    int correctness_bad = (nphantom > 0) ||
                          (is_deadzone && ((!have_cam) || (gw < 0) || (gz < 0)));

    printf("=> %s%s\n",
           bad ? "INCOMPLETE MAP (truncation)" : "COMPLETE MAP (no truncation)",
           correctness_bad ? " + CORRUPT MODEL (parser)" : " + MODEL CORRECT");
    return (bad || correctness_bad) ? 1 : 0;
}

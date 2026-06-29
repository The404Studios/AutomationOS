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

/* ------------------------------------------------------------------------
 * AUDIT honest-map selftest: prove HEADLESSLY that coherence is decoupled from
 * M_MAXPORTS (#3) and that an overflowing function reports its true port count
 * for the "+N more" marker (#5). Separate --selftest invocation so the
 * real-source no-truncation assertion in main() is untouched. Needs
 * model_analyze (ide_semantic.c), so the harness must link that TU.
 * ---------------------------------------------------------------------- */
static const char SELFTEST_SRC[] =
    "int g_a,g_b,g_c,g_d,g_e,g_f,g_h,g_i,g_j,g_k,g_l,g_m,g_n,g_o,g_p,g_q,g_r,g_s,g_t,g_u;\n"
    "void other(void){ g_a = 1; g_b = 2; }\n"           /* 2nd writer -> g_a,g_b shared */
    "void hub(int p1, int p2){\n"
    "  g_a=1; g_b=2; g_c=3; g_d=4; g_e=5; g_f=6; g_h=7; g_i=8; g_j=9; g_k=10;\n"
    "  g_l=11; g_m=12; g_n=13; g_o=14; g_p=15; g_q=16; g_r=17; g_s=18;\n"
    "  int x = g_t + g_u; (void)x;\n"
    "}\n";
/* hub: 2 params + 18 writes + 2 reads = 22 connected ports + 2 absent gates
 * (no claim/cooldown call) = 24 intended ports >> M_MAXPORTS(16). g_a,g_b are
 * written by BOTH hub and other -> writes_shared, so the absent gates ARE added
 * but fall past slot 16. No reserved keyword (claim/lock/acquire/gate/reserve/
 * cooldown/ready/can_) appears, so wants_lifecycle && wants_gate stay 1. */

static int selftest(void) {
    model_parse(&g_model, SELFTEST_SRC, (int)(sizeof(SELFTEST_SRC) - 1), "selftest.c");
    int hi = find_func("hub");
    if (hi < 0) { printf("HONEST-OVERFLOW FAIL (hub not parsed)\n"); return 1; }
    g_model.focus = hi;
    model_analyze(&g_model);
    Func* f = &g_model.funcs[hi];

    int more = ide_more(f->nports, f->nports_true);
    /* PS_ABSENT ports actually STORED in the 16-slot array = what the OLD
     * coherence loop could see. 0 here proves the absent gates were truncated
     * (so the old code scored this function ~100, the bug) while the new
     * wants_*-based coherence still penalizes them. */
    int stored_absent = 0;
    for (int i = 0; i < f->nports && i < M_MAXPORTS; i++)
        if (f->ports[i].status == PS_ABSENT) stored_absent++;

    printf("HONEST-PORTS nports=%d nports_true=%d\n", f->nports, f->nports_true);
    printf("HONEST-MARKER +%d more ports\n", more);
    printf("HONEST-COH coherence=%d wants_lc=%d wants_gate=%d stored_absent=%d\n",
           g_model.coherence, f->wants_lifecycle, f->wants_gate, stored_absent);

    int ok = (f->nports == M_MAXPORTS)        /* the 16-slot store cap was hit   */
          && (f->nports_true > M_MAXPORTS)    /* but the TRUE count exceeds it    */
          && (more > 0)                       /* -> a "+N more" marker is warranted */
          && (f->wants_lifecycle == 1)        /* intent survives the truncation   */
          && (f->wants_gate == 1)
          && (g_model.coherence <= 80)        /* coherence PENALIZED the gates     */
          && (stored_absent == 0);            /* which were NOT among the 16 stored */
    printf("HONEST-OVERFLOW %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
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

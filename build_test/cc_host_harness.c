/* cc host harness (B4a / CC-REGRESSION-SUITE-0)
 *
 * Host-compiled driver over the REAL on-device compiler front+back end
 * (the IDE's lexer/parser + cc_codegen/cc_expr/cc_type). Reads a C file, runs
 * parser_init -> parse_translation_unit -> cc_compile, and prints the generated
 * Intel-subset assembly to stdout. The regression smoke greps that asm (e.g. a
 * global `int g = 5;` must emit `dq 5`, not `dq 0`).
 *
 * Build (host gcc): see build_test/cc_regression_smoke.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ide_parser.h"   /* Parser, parser_init, parse_translation_unit, Tok, PARSE_MAX_TOKS */
#include "tc.h"           /* cc_compile, TcDiag, TC_ASM_CAP, TC_MAXDIAG */
#include "ide_astprint.h" /* astprint_tree (debug: `FILE.c --ast`) */

static Tok    g_toks[PARSE_MAX_TOKS];
static Parser g_parser;
static char   g_src[TC_ASM_CAP];
static char   g_asm[TC_ASM_CAP];

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.c\n", argv[0]);
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cc_host_harness: cannot open %s\n", argv[1]);
        return 2;
    }
    int n = (int)fread(g_src, 1, sizeof(g_src) - 1, f);
    fclose(f);
    if (n <= 0) {
        fprintf(stderr, "cc_host_harness: empty/unreadable %s\n", argv[1]);
        return 2;
    }
    g_src[n] = '\0';

    parser_init(&g_parser, g_src, n, g_toks, PARSE_MAX_TOKS);
    AstNode* tu = parse_translation_unit(&g_parser);

    /* Debug aid: `cc_host_harness FILE.c --ast` dumps the parsed tree. */
    if (argc >= 3 && strcmp(argv[2], "--ast") == 0) {
        astprint_tree(tu, g_asm, TC_ASM_CAP);
        fputs(g_asm, stdout);
        return 0;
    }

    TcDiag diags[TC_MAXDIAG];
    int    ndiags = 0;
    int    ok = cc_compile(tu, g_asm, TC_ASM_CAP, diags, &ndiags);
    if (!ok) {
        fprintf(stderr, "cc_host_harness: cc_compile FAILED (ndiag=%d)\n", ndiags);
        for (int i = 0; i < ndiags && i < TC_MAXDIAG; i++)
            fprintf(stderr, "  L%d: %s\n", diags[i].line, diags[i].msg);
        return 1;
    }

    fputs(g_asm, stdout);
    return 0;
}

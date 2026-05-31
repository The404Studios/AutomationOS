/*
 * game_check.c -- HOST-side compile check for a single on-device-C source FILE.
 *
 * NOT shipped. Built with host gcc (WSL Arch), links the SAME toolchain TUs as
 * the on-device `cc` (parser core + lexer + ast + cc_codegen/expr/type +
 * as_x64 + elf_write). Drives the real pipeline:
 *     parser_init + parse_translation_unit -> cc_compile -> as_assemble -> elf_write
 * and reports whether the file compiles to a valid static ELF under the
 * toolchain's actual (restricted) C subset -- so we can verify new sample
 * programs (games) WITHOUT booting the OS. Mirrors test/tc_test.c.
 *
 * Build + run (from repo root, in Arch WSL):
 *   gcc -I userspace/apps/ide -o /tmp/game_check userspace/apps/ide/test/game_check.c \
 *       userspace/apps/ide/ide_pcore.c userspace/apps/ide/ide_pdecl.c \
 *       userspace/apps/ide/ide_pstmt.c userspace/apps/ide/ide_pexpr.c \
 *       userspace/apps/ide/ide_lex.c   userspace/apps/ide/ide_ast.c \
 *       userspace/apps/ide/cc_codegen.c userspace/apps/ide/cc_expr.c \
 *       userspace/apps/ide/cc_type.c   userspace/apps/ide/as_x64.c \
 *       userspace/apps/ide/elf_write.c
 *   /tmp/game_check userspace/apps/ide/sample/native/hanoi.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../ide_ast.h"
#include "../ide_lex.h"
#include "../ide_parser.h"
#include "../tc.h"

static Tok     g_toks[PARSE_MAX_TOKS];
static char    g_src[1 << 17];
static char    g_asm[TC_ASM_CAP];
static uint8_t g_code[TC_CODE_CAP];
static uint8_t g_elf[TC_ELF_CAP];

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.c>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "game_check: cannot open %s\n", argv[1]);
        return 2;
    }
    int n = (int)fread(g_src, 1, sizeof(g_src) - 1, f);
    fclose(f);
    g_src[n] = '\0';

    Parser   parser;
    AstNode *tu;
    TcDiag   cc_diags[TC_MAXDIAG]; int cc_nd = 0;
    TcDiag   as_diags[TC_MAXDIAG]; int as_nd = 0;
    int      cc_ok, as_ok, code_len = 0, elf_len;
    int      i;

    ast_reset();
    parser_init(&parser, g_src, n, g_toks, PARSE_MAX_TOKS);
    tu = parse_translation_unit(&parser);

    int arena_full = ast_arena_full();
    printf("[%s]\n", argv[1]);
    printf("  parse: %d toks, %d nodes, %d diags%s\n",
           parser.ntoks, ast_node_count(), parser.ndiags,
           arena_full ? "  *** ARENA FULL ***" : "");
    for (i = 0; i < parser.ndiags && i < PARSE_MAX_DIAGS; i++)
        printf("    parse @%d:%d  %s\n",
               parser.diags[i].line, parser.diags[i].col, parser.diags[i].msg);

    g_asm[0] = '\0';
    cc_ok = cc_compile(tu, g_asm, TC_ASM_CAP, cc_diags, &cc_nd);
    for (i = 0; i < cc_nd && i < TC_MAXDIAG; i++)
        printf("    cc  @%d  %s\n", cc_diags[i].line, cc_diags[i].msg);

    as_ok = as_assemble(g_asm, TC_ENTRY_VADDR, g_code, TC_CODE_CAP,
                        &code_len, as_diags, &as_nd);
    for (i = 0; i < as_nd && i < TC_MAXDIAG; i++)
        printf("    as  @%d  %s\n", as_diags[i].line, as_diags[i].msg);

    elf_len = elf_write(g_code, code_len, g_elf, TC_ELF_CAP);

    int e_type = -1;
    long e_entry = -1;
    if (elf_len >= 64) {
        e_type = (g_elf[16] | (g_elf[17] << 8));
        e_entry = 0;
        int b;
        for (b = 0; b < 8; b++)
            e_entry |= (long)g_elf[24 + b] << (8 * b);
    }

    int ok = tu && cc_ok && as_ok && code_len > 0 && elf_len > 0
             && !arena_full && e_type == 2 && e_entry == 0x200078L;

    printf("  cc_ok=%d as_ok=%d code_len=%d elf_len=%d e_type=%d e_entry=0x%lx\n",
           cc_ok, as_ok, code_len, elf_len, e_type, e_entry);
    printf("  => %s\n", ok ? "COMPILES (valid static ELF)" : "FAILS");
    return ok ? 0 : 1;
}

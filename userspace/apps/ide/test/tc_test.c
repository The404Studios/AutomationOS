/*
 * tc_test.c -- HOST-side end-to-end test of the AutomationOS native toolchain.
 *
 * NOT shipped. Built with normal host gcc (WSL Arch). Verification only.
 *
 * Drives the full pipeline for a handful of in-memory C programs:
 *     parser_init + parse_translation_unit  -> AST
 *     cc_compile(tu, asmbuf, ...)           -> Intel-subset asm text
 *     as_assemble(asm, TC_ENTRY_VADDR, ...) -> x86-64 machine code
 *     elf_write(code, len, ...)             -> static ELF64
 *
 * For each program it prints the asm, disassembles the raw bytes with objdump,
 * dumps the ELF header with readelf, and asserts the documented contract.
 *
 * This file purposefully includes the toolchain headers (which are freestanding
 * but compile fine on a hosted target) and links against the parser + compiler
 * translation units.  It supplies its own main().
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../ide_ast.h"
#include "../ide_lex.h"
#include "../ide_parser.h"
#include "../tc.h"

/* ---- test programs ------------------------------------------------------ */
typedef struct {
    const char* name;
    const char* src;
    long        expect; /* expected exit code if it were actually run */
} Prog;

static const Prog PROGS[] = {
    { "P1 (sum 1..10 = 55)",
      "int main(){ int s=0; int i=1; while(i<=10){ s=s+i; i=i+1; } return s; }",
      55 },
    { "P2 (add(40,2) = 42)",
      "int add(int a,int b){ return a+b; } int main(){ return add(40,2); }",
      42 },
    { "P3 (6! = 720)",
      "int main(){ int x=6; int f=1; while(x>1){ f=f*x; x=x-1; } return f; }",
      720 },
};
#define NPROGS ((int)(sizeof(PROGS) / sizeof(PROGS[0])))

/* ---- assertion bookkeeping --------------------------------------------- */
static int g_pass = 0;
static int g_fail = 0;

static int check(const char* label, int cond) {
    if (cond) { g_pass++; printf("    [PASS] %s\n", label); }
    else      { g_fail++; printf("    [FAIL] %s\n", label); }
    return cond;
}

/* substring helper (asm text is NUL-terminated) */
static int contains(const char* hay, const char* needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

/* fixed buffers (freestanding-style: no malloc inside the toolchain) */
static Tok     g_toks[PARSE_MAX_TOKS];
static char    g_asm[TC_ASM_CAP];
static uint8_t g_code[TC_CODE_CAP];
static uint8_t g_elf[TC_ELF_CAP];

static void dump_diags(const char* stage, const TcDiag* diags, int ndiags) {
    int i;
    if (ndiags <= 0) return;
    printf("    %s diagnostics (%d):\n", stage, ndiags);
    for (i = 0; i < ndiags && i < TC_MAXDIAG; i++)
        printf("      line %d: %s\n", diags[i].line, diags[i].msg);
}

static void write_file(const char* path, const void* buf, int len) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("    (could not open %s for write)\n", path); return; }
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
}

static int run_one(const Prog* prog) {
    Parser   parser;
    AstNode* tu;
    TcDiag   cc_diags[TC_MAXDIAG]; int cc_nd = 0;
    TcDiag   as_diags[TC_MAXDIAG]; int as_nd = 0;
    int      cc_ok, as_ok, code_len = 0, elf_len;
    int      starting_fail = g_fail;

    printf("\n========================================================\n");
    printf("== %s\n", prog->name);
    printf("== source: %s\n", prog->src);
    printf("========================================================\n");

    /* fresh AST arena per program */
    ast_reset();

    /* --- 1. parse ------------------------------------------------------- */
    parser_init(&parser, prog->src, (int)strlen(prog->src), g_toks, PARSE_MAX_TOKS);
    tu = parse_translation_unit(&parser);
    printf("  -- parse: %d tokens, %d ast nodes, %d diags%s\n",
           parser.ntoks, ast_node_count(), parser.ndiags,
           ast_arena_full() ? " (ARENA FULL!)" : "");
    {
        int i;
        for (i = 0; i < parser.ndiags && i < PARSE_MAX_DIAGS; i++)
            printf("     parse diag @%d:%d  %s\n",
                   parser.diags[i].line, parser.diags[i].col, parser.diags[i].msg);
    }
    check("parse produced an AST_TU root", tu && tu->kind == AST_TU && tu->first_child);

    /* --- 2. compile (AST -> asm) --------------------------------------- */
    g_asm[0] = '\0';
    cc_ok = cc_compile(tu, g_asm, TC_ASM_CAP, cc_diags, &cc_nd);
    dump_diags("cc_compile", cc_diags, cc_nd);

    printf("  -- generated asm --------------------------------------\n");
    fputs(g_asm, stdout);
    if (g_asm[0] && g_asm[strlen(g_asm) - 1] != '\n') putchar('\n');
    printf("  -------------------------------------------------------\n");

    /* persist the asm so an external cross-check (real NASM+ld) can run it */
    write_file("/tmp/p.asm", g_asm, (int)strlen(g_asm));

    check("cc_compile returned ok",            cc_ok);
    check("asm contains a `_start` label",     contains(g_asm, "_start:"));
    check("asm contains a `main` label",       contains(g_asm, "main:"));
    check("asm contains a `syscall`",          contains(g_asm, "syscall"));

    /* --- 3. assemble (asm -> machine code) ----------------------------- */
    as_ok = as_assemble(g_asm, TC_ENTRY_VADDR, g_code, TC_CODE_CAP,
                        &code_len, as_diags, &as_nd);
    dump_diags("as_assemble", as_diags, as_nd);
    printf("  -- as_assemble: ok=%d code_len=%d (entry vaddr=0x%llx)\n",
           as_ok, code_len, (unsigned long long)TC_ENTRY_VADDR);

    check("as_assemble returned ok",           as_ok);
    check("as_assemble produced code_len > 0", code_len > 0);

    if (code_len > 0) {
        write_file("/tmp/p.bin", g_code, code_len);
        printf("  -- objdump disassembly of /tmp/p.bin ------------------\n");
        fflush(stdout);
        system("objdump -D -b binary -m i386:x86-64 -M intel /tmp/p.bin | head -60");
        printf("  -------------------------------------------------------\n");
    }

    /* --- 4. link (machine code -> ELF) --------------------------------- */
    elf_len = elf_write(g_code, code_len, g_elf, TC_ELF_CAP);
    printf("  -- elf_write: returned %d\n", elf_len);
    check("elf_write returned > 0", elf_len > 0);

    if (elf_len > 0) {
        write_file("/tmp/p.elf", g_elf, elf_len);
        printf("  -- readelf -h /tmp/p.elf ------------------------------\n");
        fflush(stdout);
        system("readelf -h /tmp/p.elf | head");
        printf("  -------------------------------------------------------\n");

        /* Contract check straight from the ELF bytes (don't trust readelf
         * parsing alone): e_type at offset 16 (2 bytes LE), e_entry at 24
         * (8 bytes LE) per ELF64. */
        if (elf_len >= 64) {
            uint16_t e_type =
                (uint16_t)(g_elf[16] | (g_elf[17] << 8));
            uint64_t e_entry = 0; int b;
            for (b = 0; b < 8; b++)
                e_entry |= (uint64_t)g_elf[24 + b] << (8 * b);
            printf("  -- raw ELF: e_type=%u (2==EXEC) e_entry=0x%llx\n",
                   e_type, (unsigned long long)e_entry);
            check("ELF Type == EXEC (2)",       e_type == 2);
            check("ELF Entry == 0x200078",      e_entry == 0x200078ULL);
        } else {
            check("ELF Type == EXEC (2)",  0);
            check("ELF Entry == 0x200078", 0);
        }
    }

    printf("  == %s : %s ==\n", prog->name,
           (g_fail == starting_fail) ? "ALL ASSERTIONS PASSED"
                                     : "HAD FAILURES (see above)");
    return g_fail == starting_fail;
}

int main(void) {
    int i, ok_count = 0;

    printf("AutomationOS native toolchain -- HOST end-to-end test\n");
    printf("TC_BASE_VADDR=0x%llx  TC_ENTRY_VADDR=0x%llx  (0x200078 expected)\n",
           (unsigned long long)TC_BASE_VADDR, (unsigned long long)TC_ENTRY_VADDR);

    for (i = 0; i < NPROGS; i++)
        if (run_one(&PROGS[i])) ok_count++;

    printf("\n########################################################\n");
    printf("# SUMMARY: %d/%d programs fully passed | %d assertions PASS, %d FAIL\n",
           ok_count, NPROGS, g_pass, g_fail);
    printf("########################################################\n");

    return g_fail == 0 ? 0 : 1;
}

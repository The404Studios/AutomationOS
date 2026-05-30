/*
 * cc.c -- /bin/cc, the on-device C compiler for AutomationOS.
 *
 * The flagship self-hosting feature: the OS compiles its own C programs.
 * This is a thin DRIVER -- it does NOT contain a compiler. It REUSES the
 * IDE's verified, on-device C-subset toolchain (userspace/apps/ide/) and
 * mirrors exactly the lex -> parse -> AST -> codegen -> assemble -> ELF
 * sequence that ide_build.c / tc_driver.c drive. The only thing cc.c adds is
 * a command-line front-end and its own input/output syscalls.
 *
 *   Usage:  cc INPUT.c -o OUTPUT        (write ELF to OUTPUT)
 *           cc INPUT.c                  (write ELF to ./a.out)
 *           cc                          (run the built-in self-test)
 *
 * Pipeline (identical to tc_build() in ide/tc_driver.c):
 *   src text --parser_init + parse_translation_unit--> AST (AstNode* tu)
 *   tu       --cc_compile (wraps cc_gen_program)-----> Intel-subset asm text
 *   asm      --as_assemble @ TC_ENTRY_VADDR----------> x86-64 machine code
 *   code     --elf_write-----------------------------> static ELF64 the loader runs
 *
 * Linked with userspace/crt0.asm, so this file provides a real
 *   int main(int argc, char** argv).
 * Freestanding: no libc, no malloc, no stdio. All buffers are STATIC and the
 * stages themselves are freestanding (they take fixed buffers).
 *
 * Supported C subset (see ide/cc.h header comment): all integer/pointer types
 * are 64-bit except char/_Bool = 1 byte; global + local int/char/pointer vars,
 * functions with <=6 params, return, if/else, while, for, blocks, the operators
 * + - * / % & | ^ << >> ! ~ unary- && || == != < <= > >= = (and compound forms),
 * function calls incl recursion, string literals, array index a[i], deref *p /
 * addr-of &x, struct member layout, and builtins sys_write(fd,buf,len) /
 * sys_exit(code) that emit `syscall`. The generated ELF carries its own _start
 * trampoline (call main; mov rdi,rax; mov rax,0; syscall) -- output programs are
 * self-contained and do NOT use crt0.
 */

#include "ide_ast.h"
#include "ide_parser.h"   /* Parser, parser_init, parse_translation_unit, PARSE_MAX_TOKS */
#include "tc.h"           /* TC_*_CAP, TC_ENTRY_VADDR, cc_compile/as_assemble/elf_write, TcDiag */

/* ============================================================================
 *  Inline syscalls (freestanding; AutomationOS ABI: nr in RAX,
 *  args RDI,RSI,RDX,R10,R8,R9, ret RAX).
 * ==========================================================================*/

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_CREAT  0x0040
#define O_TRUNC  0x0200

/* 3-arg syscall covers open/read/write/close/exit here. */
static long sc(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding string helpers ---- */

static int slen(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int seq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void out(const char* s)
{
    int n = slen(s);
    if (n > 0) sc(SYS_WRITE, 1, (long)s, n);
}

/* ============================================================================
 *  File IO (own syscalls per the cc contract; mirrors ide_sys.c semantics).
 * ==========================================================================*/

/* Read whole file into buf[0..cap-1]; returns byte count (>=0) or <0 on error. */
static int read_file(const char* path, char* buf, int cap)
{
    long fd;
    int total = 0;
    if (!path || !buf || cap <= 0) return -1;
    fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return (int)fd;
    while (total < cap) {
        long m = sc(SYS_READ, fd, (long)(buf + total), cap - total);
        if (m <= 0) break;
        total += (int)m;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Truncate+write len bytes to path (creating it). Returns 0 on success, <0 else. */
static int write_file(const char* path, const unsigned char* buf, int len)
{
    long fd;
    int total = 0;
    if (!path || !buf || len < 0) return -1;
    /* mode arg goes in RDX (3rd) -- pass 0644 there. */
    fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return (int)fd;
    while (total < len) {
        long m = sc(SYS_WRITE, fd, (long)(buf + total), len - total);
        if (m <= 0) break;
        total += (int)m;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return (total == len) ? 0 : -1;
}

/* ============================================================================
 *  Shared static working buffers (sized like ide/tc_driver.c).
 * ==========================================================================*/

static char          g_src[TC_ASM_CAP];
static char          g_asm[TC_ASM_CAP];
static unsigned char g_code[TC_CODE_CAP];
static unsigned char g_elf[TC_ELF_CAP];
static Tok           g_toks[PARSE_MAX_TOKS];
static Parser        g_parser;
static TcDiag        g_diags[TC_MAXDIAG];

/* ============================================================================
 *  The compile pipeline -- the EXACT call sequence ide_build.c/tc_build use.
 *
 *  src (NUL-terminated, srclen bytes) -> ELF bytes in g_elf.
 *  Returns the ELF byte length (>0) on success, or <0 on the failing stage
 *  (-1 parse/codegen, -2 assemble, -3 elf_write). Diagnostics are printed to
 *  fd 1 by the caller.
 * ==========================================================================*/

static int g_ndiags;

static int compile_to_elf(const char* src, int srclen)
{
    AstNode* tu;
    int code_len = 0;
    int elen;

    g_ndiags = 0;

    /* 1. lex + parse -> AST.  parse_translation_unit() resets the AST arena
     *    and sets the root internally. */
    parser_init(&g_parser, src, srclen, g_toks, PARSE_MAX_TOKS);
    tu = parse_translation_unit(&g_parser);

    /* 2. AST -> Intel-subset asm text (cc_compile wraps cc_gen_program). */
    if (!cc_compile(tu, g_asm, TC_ASM_CAP, g_diags, &g_ndiags))
        return -1;

    /* 3. asm text -> machine code, labels resolved at the fixed load base. */
    if (!as_assemble(g_asm, TC_ENTRY_VADDR, g_code, TC_CODE_CAP,
                     &code_len, g_diags, &g_ndiags) || code_len <= 0)
        return -2;

    /* 4. machine code -> static ELF64 the OS loader runs. */
    elen = elf_write(g_code, code_len, g_elf, TC_ELF_CAP);
    if (elen <= 0)
        return -3;

    return elen;
}

/* Print up to TC_MAXDIAG collected diagnostics to fd 1. */
static void print_diags(void)
{
    int nd = g_ndiags;
    int i;
    if (nd < 0) nd = 0;
    if (nd > TC_MAXDIAG) nd = TC_MAXDIAG;
    for (i = 0; i < nd; i++) {
        char num[16];
        int v = g_diags[i].line;
        int li = 0, j;
        char tmp[16];
        int neg = 0;
        unsigned int u;
        out("cc: line ");
        if (v < 0) { neg = 1; u = (unsigned int)(-(long)v); } else u = (unsigned int)v;
        do { tmp[li++] = (char)('0' + (u % 10)); u /= 10; } while (u);
        j = 0;
        if (neg) num[j++] = '-';
        while (li > 0) num[j++] = tmp[--li];
        num[j] = 0;
        out(num);
        out(": ");
        out(g_diags[i].msg);
        out("\n");
    }
}

/* ============================================================================
 *  Self-test (argc <= 1): compile a tiny in-subset program, write it, reopen
 *  and verify the ELF magic.  Prints "CC SELFTEST: PASS" / "FAIL <reason>".
 * ==========================================================================*/

static const char SELFTEST_SRC[] =
    "int main(){ sys_write(1, \"hi\", 2); return 0; }\n";

static int self_test(void)
{
    static unsigned char back[16];
    const char* path = "/tmp/cc_out";
    int elen;
    int rd;

    out("cc: running self-test\n");

    elen = compile_to_elf(SELFTEST_SRC, slen(SELFTEST_SRC));
    if (elen < 0) {
        print_diags();
        if (elen == -1)      out("CC SELFTEST: FAIL compile\n");
        else if (elen == -2) out("CC SELFTEST: FAIL assemble\n");
        else                 out("CC SELFTEST: FAIL elfwrite\n");
        return 1;
    }

    if (write_file(path, g_elf, elen) < 0) {
        out("CC SELFTEST: FAIL write\n");
        return 1;
    }

    /* reopen and verify the first 4 bytes are the ELF magic. */
    rd = read_file(path, (char*)back, (int)sizeof(back));
    if (rd < 4) {
        out("CC SELFTEST: FAIL reopen\n");
        return 1;
    }
    if (back[0] != 0x7F || back[1] != 'E' || back[2] != 'L' || back[3] != 'F') {
        out("CC SELFTEST: FAIL magic\n");
        return 1;
    }

    out("CC SELFTEST: PASS\n");
    return 0;
}

/* ============================================================================
 *  main
 * ==========================================================================*/

int main(int argc, char** argv)
{
    const char* in_path  = 0;
    const char* out_path = 0;
    int i;
    int srclen;
    int elen;

    /* No real arguments -> self-test. (argv[0] is the program name.) */
    if (argc <= 1)
        return self_test();

    /* parse: cc INPUT.c [-o OUTPUT] */
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!a) continue;
        if (seq(a, "-o")) {
            if (i + 1 < argc) {
                out_path = argv[i + 1];
                i++;
            } else {
                out("cc: -o requires an argument\n");
                return 1;
            }
        } else if (!in_path) {
            in_path = a;
        } else {
            out("cc: too many input files (only one supported)\n");
            return 1;
        }
    }

    if (!in_path) {
        out("usage: cc INPUT.c [-o OUTPUT]\n");
        return 1;
    }
    if (!out_path)
        out_path = "a.out";

    /* read source. */
    srclen = read_file(in_path, g_src, TC_ASM_CAP - 1);
    if (srclen < 0) {
        out("cc: cannot read ");
        out(in_path);
        out("\n");
        return 1;
    }
    g_src[srclen] = '\0';

    /* compile. */
    elen = compile_to_elf(g_src, srclen);
    if (elen < 0) {
        print_diags();
        out("cc: compilation failed\n");
        return 1;
    }

    /* write the ELF. */
    if (write_file(out_path, g_elf, elen) < 0) {
        out("cc: cannot write ");
        out(out_path);
        out("\n");
        return 1;
    }

    out("cc: wrote ");
    out(out_path);
    out("\n");
    return 0;
}

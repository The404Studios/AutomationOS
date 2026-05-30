/*
 * js.c -- the `js` CLI app for AutomationOS.
 * ==========================================
 *
 *   js FILE.js     read and execute a JavaScript file
 *   js             (argc<=1) run the embedded engine self-test
 *
 * FREESTANDING ring-3, NO libc / stdio / malloc / standard headers. Pure
 * inline syscalls. The script's console.log output goes to fd 1 (SYS_WRITE).
 * The JS engine lives in userspace/lib/js (linked alongside this object).
 *
 * Build (flags DIRECTLY on the command line; never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -I userspace/lib/js -c userspace/apps/js/js.c -o js_app.o
 *   gcc ... -c userspace/lib/js/js_lex.c    -o js_lex.o   (etc. for each)
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o js_app.o js_lex.o js_parse.o js_value.o \
 *       js_interp.o js_builtin.o -o build/js
 *   objdump -d build/js | grep fs:0x28    # MUST be empty
 */

#include "../../lib/js/js.h"

/* ----------------------------------------------------------------- */
/*  Syscall numbers (match kernel/include/syscall.h)                  */
/* ----------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY   0x0000

typedef unsigned long usize;

/* ----------------------------------------------------------------- */
/*  Inline syscall (rdi/rsi/rdx/r10/r8 hold args 1..5)               */
/* ----------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

static long w(int fd, const void *buf, usize n) { return sc(SYS_WRITE, fd, (long)buf, (long)n, 0, 0); }
static void put(const char *s) { usize n=0; while (s[n]) n++; w(1, s, n); }

/* console.log sink wired into the engine */
static void emit_sink(const char *s, unsigned long n) { w(1, s, n); }

/* ----------------------------------------------------------------- */
/*  Read a whole file into a fixed static buffer                      */
/* ----------------------------------------------------------------- */
#define SRC_CAP (1024u * 1024u)   /* 1 MB max script size */
static char g_src[SRC_CAP];

/* read all of FILE into g_src; returns byte count, or -1 on error */
static long read_file(const char *path)
{
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long got = sc(SYS_READ, fd, (long)(g_src + total),
                      (long)(SRC_CAP - 1 - total), 0, 0);
        if (got < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0); return -1; }
        if (got == 0) break;
        total += got;
        if ((usize)total >= SRC_CAP - 1) break;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    g_src[total] = 0;
    return total;
}

static usize slen(const char *s) { usize n=0; while (s[n]) n++; return n; }

/* result buffer for the completion value / error message */
static char g_result[1024];

/* ----------------------------------------------------------------- */
/*  main                                                             */
/* ----------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc <= 1) {
        /* self-test mode */
        int failures = js_selftest();
        if (failures == 0) {
            put("JS SELFTEST: PASS\n");
            return 0;
        }
        put("JS SELFTEST: FAIL (");
        /* print failure count */
        char nb[16]; usize nn=0; char rev[16]; usize rn=0; int t=failures;
        if (t==0) rev[rn++]='0';
        while (t){ rev[rn++]=(char)('0'+t%10); t/=10; }
        while (rn) nb[nn++]=rev[--rn];
        nb[nn]=0;
        put(nb);
        put(" failing)\n");
        return 1;
    }

    /* run a file */
    const char *path = argv[1];
    long len = read_file(path);
    if (len < 0) {
        put("js: cannot open file: ");
        put(path);
        put("\n");
        return 1;
    }

    js_vm *vm = js_new();
    if (!vm) { put("js: engine init failed\n"); return 1; }
    js_set_print(vm, emit_sink);

    int rc = js_eval(vm, g_src, (unsigned long)len, g_result, sizeof g_result);
    if (rc < 0) {
        /* error message is in g_result */
        put(g_result);
        put("\n");
        return 1;
    }

    /* Successful run. If the completion value is meaningful (not just
     * "undefined"), echo it the way a REPL would. Scripts that print via
     * console.log have already produced their output through emit_sink. */
    {
        const char *r = g_result;
        int is_undef = (r[0]=='u'&&r[1]=='n'&&r[2]=='d'&&r[3]=='e'&&r[4]=='f'&&
                        r[5]=='i'&&r[6]=='n'&&r[7]=='e'&&r[8]=='d'&&r[9]==0);
        if (!is_undef && slen(r) > 0) { put(r); put("\n"); }
    }
    return 0;
}

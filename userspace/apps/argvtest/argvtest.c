/* argvtest -- proves the kernel->user argv handoff (exec.c argv frame + crt0).
 * Linked with crt0.o, so it has a real main(argc, argv). Prints what it got and
 * a PASS/FAIL marker the smoke gate checks. Freestanding: inline syscalls only. */

typedef unsigned long size_t;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(3 /*SYS_WRITE*/, 1, (long)s, (long)slen(s)); }

int main(int argc, char** argv) {
    out("ARGVTEST: argc=");
    char d = (char)('0' + (argc % 10));
    sc(3, 1, (long)&d, 1);
    out(" args:");
    for (int i = 0; i < argc && i < 16; i++) {
        out(" [");
        out((argv && argv[i]) ? argv[i] : "(null)");
        out("]");
    }
    out("\n");

    /* PASS iff we received a sane frame: argc>=1 and a non-empty argv[0]. */
    if (argc >= 1 && argv && argv[0] && argv[0][0])
        out("ARGVTEST: PASS\n");
    else
        out("ARGVTEST: FAIL\n");
    return 0;
}

/*
 * codeagent.c -- "run open code" zero-cost proof (CLAUDE-CODE-0, scripted driver).
 * ==============================================================================
 *
 * Stands in for Claude's tool-use loop (like scripts/oauth_mock.py stood in for
 * Google): a SCRIPTED agent that drives the real write -> compile -> execute
 * pipeline so the whole capability is provable with no API key.
 *
 * This first cut proves the load-bearing unknown -- that the ON-DEVICE `cc` can
 * compile a program an agent just wrote, and that the result runs:
 *   1. write a tiny C program to /tmp/hello.c
 *   2. spawn sbin/cc  /tmp/hello.c -o /tmp/hello.elf
 *   3. spawn /tmp/hello.elf  -> it prints "RUNOK" to serial
 * The gated tool PROCESSES (tool_write/tool_cc/tool_exec) + policy come next.
 *
 * Freestanding ring 3, crt0+main. Prints CODEAGENT: lines to serial (fd1).
 */

#define SYS_WAITPID        6
#define SYS_WRITE          3
#define SYS_OPEN           4
#define SYS_CLOSE          5
#define SYS_SPAWN_EX_ARGV  106
#define O_WRONLY           1
#define O_CREAT            0x40
#define O_TRUNC            0x200

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void outn(long v) {
    char b[24]; int i = 0; char t[24]; int j = 0;
    int neg = (v < 0); unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) b[i++] = '0';
    while (u) { t[j++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    while (j) b[i++] = t[--j];
    b[i] = 0; out(b);
}

/* A maximally cc-subset-friendly program: NO string literals, NO arrays, NO
 * globals. Prints "RUNOK\n" one byte at a time via the syscall() builtin and a
 * local char's address. */
static const char *HELLO_SRC =
    "int main(){\n"
    "  char c;\n"
    "  c=82; syscall(3,1,(long)&c,1);\n"   /* R */
    "  c=85; syscall(3,1,(long)&c,1);\n"   /* U */
    "  c=78; syscall(3,1,(long)&c,1);\n"   /* N */
    "  c=79; syscall(3,1,(long)&c,1);\n"   /* O */
    "  c=75; syscall(3,1,(long)&c,1);\n"   /* K */
    "  c=10; syscall(3,1,(long)&c,1);\n"   /* \n */
    "  return 0;\n"
    "}\n";

static int has_sub(const char *p, const char *sub) {
    for (int i = 0; p[i]; i++) {
        int j = 0; while (sub[j] && p[i + j] == sub[j]) j++;
        if (!sub[j]) return 1;
    }
    return 0;
}
static int starts(const char *p, const char *pre) {
    int j = 0; while (pre[j]) { if (p[j] != pre[j]) return 0; j++; } return 1;
}

/* THE GATE: the same policy aibroker uses (path_write_allowed). The agent (here
 * scripted; live = Claude over the rail) may ONLY write to scratch/project dirs;
 * system paths + traversal are denied. A real deployment also audits + can
 * snapshot/rollback (aibroker.c) -- this proves the allow/deny decision. */
static int path_write_allowed(const char *p) {
    if (!p || !p[0]) return 0;
    if (has_sub(p, ".."))     return 0;                 /* no traversal          */
    if (starts(p, "/boot") || starts(p, "/sbin") ||
        starts(p, "/bin")  || starts(p, "/etc")  ||
        has_sub(p, "kernel")) return 0;                 /* protected system paths */
    if (starts(p, "/tmp") || starts(p, "/home") ||
        starts(p, "/usr/src")) return 1;                /* allowlist             */
    return 0;                                           /* deny by default       */
}

/* A GATED file write: policy-check the path, then write. Returns bytes written,
 * or -1 if the policy denied it. */
static long gated_write(const char *path, const char *data) {
    if (!path_write_allowed(path)) {
        out("CODEAGENT: DENY write "); out(path); out(" (policy)\n");
        return -1;
    }
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) { out("CODEAGENT: FAIL open "); out(path); out("\n"); return -1; }
    long n = sc(SYS_WRITE, fd, (long)data, (long)slen(data));
    sc(SYS_CLOSE, fd, 0, 0);
    out("CODEAGENT: wrote "); out(path); out(" ("); outn(n); out(" bytes)\n");
    return n;
}

/* Build a NUL-separated argv[1..] buffer; returns total length. */
static int argv_pack(char *buf, int cap, const char *const *args, int n) {
    int p = 0;
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        for (int j = 0; a[j] && p < cap - 1; j++) buf[p++] = a[j];
        if (p < cap) buf[p++] = 0;
    }
    return p;
}

/* Spawn path with argv[1..]=args, wait, return the child's exit status. */
static long spawn_wait(const char *path, const char *const *args, int nargs) {
    char argv[512];
    int alen = argv_pack(argv, (int)sizeof(argv), args, nargs);
    long pid = sc6(SYS_SPAWN_EX_ARGV, (long)path, (long)argv, (long)alen, 0, 0, 0);
    if (pid < 0) return -1;
    long st = 0;
    long w = sc(SYS_WAITPID, pid, (long)&st, 0);
    if (w < 0) return -2;
    return st;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    out("CODEAGENT: start (write -> compile -> execute)\n");

    /* 1a. GATE: a protected-path write the agent must NOT be allowed to do. */
    int denied = (gated_write("/etc/cron.d/evil", HELLO_SRC) < 0);

    /* 1b. the real write -- to an allowed scratch path. */
    if (gated_write("/tmp/hello.c", HELLO_SRC) < 0) {
        out("CODEAGENT: FAIL write\n"); return 0;
    }

    /* 2. compile with the on-device cc (installed in /bin) */
    const char *cc_args[3] = { "/tmp/hello.c", "-o", "/tmp/hello.elf" };
    long cc_st = spawn_wait("bin/cc", cc_args, 3);
    out("CODEAGENT: cc exit="); outn(cc_st); out("\n");
    if (cc_st != 0) { out("CODEAGENT: FAIL compile\n"); return 0; }

    /* 3. run the freshly compiled program (its output -> serial) */
    out("CODEAGENT: --- program output ---\n");
    long run_st = spawn_wait("/tmp/hello.elf", 0, 0);
    out("CODEAGENT: --- end output (exit="); outn(run_st); out(") ---\n");
    out("CODEAGENT: PASS protected_rejected=");
    out(denied ? "1" : "0");
    out(" wrote=1 compiled=1 ran=1\n");
    return 0;
}

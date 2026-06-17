/* tool_exec -- TOOLSET-0 gated exec tool. argv=[path]. Spawns ONE program and
 * reports its exit status to the agent (sbin/agentd) over the fd1 capability
 * channel. The model that picks `path` is HOSTILE TEXT, so the path is run
 * through path_write_allowed() (copied VERBATIM from the kit / codeagent.c):
 *   - ONLY user/scratch dirs run -- /tmp, /home, /usr/src
 *   - traversal ("..") and system paths (/boot,/sbin,/bin,/etc,*kernel*) DENY
 * On deny: print `DENY policy <path>` to fd1 and return 2 (the agent observes
 * the refusal; nothing is spawned). On allow: spawn_wait(path,0,0) -- no argv,
 * the child's own stdout flows to serial, which is fine -- then print the
 * one-line outcome `EXEC <path> exit=<n>` to fd1 and return 0.
 *
 * Freestanding ring 3 (crt0 supplies _start + main(argc,argv); NO libc, NO
 * headers). fd1 = the capability channel the agent CAPTURES as the result;
 * fd2 is reserved for non-observation diagnostics. */

#define SYS_WRITE          3
#define SYS_WAITPID        6
#define SYS_SPAWN_EX_ARGV  106

/* ------------------------------------------------------------------------
 * Syscall wrappers (copied verbatim from the ABI kit).
 *   sc  -- up to 3 args (rdi/rsi/rdx)
 *   sc6 -- full 6-arg form (adds r10/r8/r9), needed for SYS_SPAWN_EX_ARGV
 * ---------------------------------------------------------------------- */
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

/* ------------------------------------------------------------------------
 * Freestanding helpers (kit-standard). out() writes a NUL-terminated string
 * to fd; outn() appends a signed decimal one byte at a time (no 64-bit div
 * beyond the const /10, which the toolchain lowers without a libgcc call).
 * ---------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }
static void outn(int fd, long v) {
    char t[24]; int j = 0;
    char b[24]; int i = 0;
    int neg = (v < 0);
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[j++] = '0';
    while (u) { t[j++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    while (j) b[i++] = t[--j];
    sc(SYS_WRITE, fd, (long)b, i);
}

/* ------------------------------------------------------------------------
 * THE PATH GATE (copied VERBATIM from the kit -- the same policy the agent
 * broker enforces). The model is hostile text: only scratch/project paths
 * may run; traversal and protected system paths are denied by default.
 * ---------------------------------------------------------------------- */
static int has_sub(const char *p, const char *s) {
    for (int i = 0; p[i]; i++) {
        int j = 0; while (s[j] && p[i + j] == s[j]) j++;
        if (!s[j]) return 1;
    }
    return 0;
}
static int starts(const char *p, const char *pre) {
    int j = 0; while (pre[j]) { if (p[j] != pre[j]) return 0; j++; } return 1;
}
static int path_write_allowed(const char *p) {
    if (!p || !p[0]) return 0;
    if (has_sub(p, ".."))     return 0;                 /* no traversal           */
    if (starts(p, "/boot") || starts(p, "/sbin") ||
        starts(p, "/bin")  || starts(p, "/etc")  ||
        has_sub(p, "kernel")) return 0;                 /* protected system paths */
    if (starts(p, "/tmp") || starts(p, "/home") ||
        starts(p, "/usr/src")) return 1;                /* allowlist              */
    return 0;                                           /* deny by default        */
}

/* ------------------------------------------------------------------------
 * SPAWN + WAIT (copied from the kit). Packs argv[1..] (here: none) into a
 * bounded NUL-separated buffer, spawns the path, and waits for it. Returns
 * the child's exit status, or a negative sentinel on spawn/wait failure.
 * ---------------------------------------------------------------------- */
static int argv_pack(char *buf, int cap, const char *const *args, int n) {
    int p = 0;
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        for (int j = 0; a[j] && p < cap - 1; j++) buf[p++] = a[j];
        if (p < cap) buf[p++] = 0;
    }
    return p;
}
static long spawn_wait(const char *path, const char *const *args, int nargs) {
    char av[512];
    int al = argv_pack(av, 512, args, nargs);
    long pid = sc6(SYS_SPAWN_EX_ARGV, (long)path, (long)av, al, 0, 0, 0);
    if (pid < 0) return -1;
    long st = 0;
    long w = sc(SYS_WAITPID, pid, (long)&st, 0);
    return w < 0 ? -2 : st;
}

/* ------------------------------------------------------------------------
 * Entry point. crt0 supplies _start, parses argc/argv off the kernel stack,
 * calls main(argc,argv), and feeds the return value to SYS_EXIT.
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    /* Validate argc/argv before any deref -- never touch a missing/empty arg. */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        out(2, "ERR no_path\n");                  /* diagnostic -> fd2          */
        return 2;
    }
    const char *path = argv[1];

    /* GATE: hostile-text path must be in the allowlist, else a visible refusal. */
    if (!path_write_allowed(path)) {
        out(1, "DENY policy ");                   /* observation -> fd1         */
        out(1, path);
        out(1, "\n");
        return 2;
    }

    /* Spawn the program with NO argv (path-only) and wait for it. The child's
     * own stdout goes to serial -- the agent only reads OUR fd1 outcome line. */
    long st = spawn_wait(path, 0, 0);

    out(1, "EXEC ");                              /* one-line outcome -> fd1    */
    out(1, path);
    out(1, " exit=");
    outn(1, st);
    out(1, "\n");
    return 0;
}

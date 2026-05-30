/*
 * xargs.c -- minimal freestanding `xargs` for the from-scratch x86_64 OS.
 * =======================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers. All diagnostics to fd 1.
 *
 * This OS has no pipes/stdin, so the classic `... | xargs CMD` is adapted to
 * a file-argument form: the token source is an ARGSFILE rather than stdin.
 *
 * Usage:
 *   xargs ARGSFILE CMDPATH     read whitespace-separated tokens from ARGSFILE
 *                              and SYS_SPAWN CMDPATH once per token, passing
 *                              that token as CMDPATH's argument (via SYS_SPAWN
 *                              arg2 -- a space-separated args string the exec
 *                              layer turns into argv = [CMDPATH, token]).
 *                              Each child is awaited (SYS_WAITPID) so runs are
 *                              serial. exit 0 on success, 1 on error.
 *   xargs                      (argc<=1) run the built-in self-test, printing
 *                              "XARGS SELFTEST: PASS" or "XARGS SELFTEST: FAIL".
 *
 * SYS_SPAWN (kernel/core/syscall/handlers.c) takes arg1=path, arg2=user
 * pointer to a NUL-terminated space-separated args string (or 0). exec.c
 * builds the child argv as [path, tokens...] from that string. We pass each
 * token through arg2 so the child receives it as argv[1].
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/xargs/xargs.c -o /tmp/xargs.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/xargs.o -o /tmp/xargs.elf
 *   objdump -d /tmp/xargs.elf | grep 'fs:0x28'   # must produce no output
 */

#define SYS_EXIT    0
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_WAITPID 6
#define SYS_SPAWN   16
#define SYS_UNLINK  34

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

#define KPATH_MAX 4096

/* SYS_SPAWN needs two pointer args (path + args string), so use a 2-arg-plus
 * variant. The base sc() covers everything else. */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned long x_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void x_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)x_strlen(s)); }
static void out_num(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1); }
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

/* ======================================================================
 * ARGSFILE buffer (static; off the small user stack). Bounded at 64 KB.
 * ==================================================================== */
#define ARGS_MAX (64 * 1024)
static char g_args[ARGS_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));
static char g_cmd[KPATH_MAX]  __attribute__((aligned(16)));  /* CMDPATH copy */
static char g_token[256];                                    /* one token    */

/* Read the whole ARGSFILE into g_args. Returns bytes (>=0), -1 open fail. */
static long slurp_args(const char *path) {
    x_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < ARGS_MAX) {
        long n = sc(SYS_READ, fd, (long)(g_args + total), ARGS_MAX - total);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/*
 * spawn_one -- SYS_SPAWN g_cmd with `token` as its argument and wait for it.
 * The args string handed to SYS_SPAWN is just the token (exec.c prepends the
 * path as argv[0]). Returns the child's exit status, or -1 on spawn failure.
 */
static long spawn_one(const char *token) {
    long pid = sc(SYS_SPAWN, (long)g_cmd, (long)token, 0);
    if (pid <= 0) {
        out("xargs: spawn failed for '"); out(g_cmd); out("'\n");
        return -1;
    }
    long status = 0;
    sc(SYS_WAITPID, pid, (long)&status, 0);   /* run serially */
    return status;
}

/*
 * xargs_core -- tokenize g_args[0..len) on whitespace and spawn g_cmd once
 * per token. Returns the number of tokens dispatched, or -1 on error.
 * (Spawn handler is invoked here; self-test overrides via run_each.)
 */
static long tokenize(const char *buf, long len,
                     long (*run_each)(const char *token)) {
    long count = 0;
    long i = 0;
    while (i < len) {
        while (i < len && is_space(buf[i])) i++;       /* skip whitespace */
        if (i >= len) break;
        int t = 0;
        while (i < len && !is_space(buf[i])) {
            if (t < (int)sizeof(g_token) - 1) g_token[t++] = buf[i];
            i++;
        }
        g_token[t] = '\0';
        if (t == 0) continue;
        if (run_each(g_token) < 0) return -1;
        count++;
    }
    return count;
}

static long run_spawn(const char *token) { return spawn_one(token); }

/* ======================================================================
 * xargs_run -- argv-driven entry.  xargs ARGSFILE CMDPATH
 * ==================================================================== */
static int xargs_run(int argc, char **argv) {
    if (argc < 3 || !argv[1] || !argv[2]) {
        out("usage: xargs ARGSFILE CMDPATH\n");
        return 1;
    }
    long len = slurp_args(argv[1]);
    if (len < 0) { out("xargs: cannot open '"); out(argv[1]); out("'\n"); return 1; }

    x_strlcpy(g_cmd, argv[2], KPATH_MAX);

    long n = tokenize(g_args, len, run_spawn);
    if (n < 0) return 1;
    out("xargs: dispatched "); out_num((unsigned long)n); out(" command(s)\n");
    return 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * Spawning real children depends on an initrd CMD being present, which the
 * self-test cannot guarantee. Instead the self-test verifies the TOKENIZER
 * (the part xargs owns): it writes an ARGSFILE with mixed whitespace and
 * confirms tokenize() yields the exact expected tokens, in order.
 * Prints XARGS SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TF "/tmp/xargs_args.txt"

/* Capture-mode "run_each": records tokens into a list for verification. */
static char g_tok_list[16][64];
static int  g_tok_n;

static long run_capture(const char *token) {
    if (g_tok_n < 16) { x_strlcpy(g_tok_list[g_tok_n], token, 64); g_tok_n++; }
    return 0;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int write_file(const char *path, const char *content) {
    x_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long l = x_strlen(content), off = 0;
    while (off < l) {
        long w = sc(SYS_WRITE, fd, (long)(content + off), (long)(l - off));
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += (unsigned long)w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

static void selftest(void) {
    out("XARGS: selftest begin\n");

    /* mixed spaces, tabs, newlines, leading/trailing whitespace */
    const char *content = "  alpha\tbeta\n  gamma   delta\n";
    if (write_file(TF, content) != 0) {
        out("XARGS SELFTEST: FAIL (could not write args file)\n");
        return;
    }

    long len = slurp_args(TF);
    if (len < 0) { out("XARGS SELFTEST: FAIL (could not read args file)\n"); return; }

    g_tok_n = 0;
    long n = tokenize(g_args, len, run_capture);

    out("XARGS: tokens ->");
    for (int i = 0; i < g_tok_n; i++) { out(" ["); out(g_tok_list[i]); out("]"); }
    out("\n");

    int ok = (n == 4) && (g_tok_n == 4) &&
             streq(g_tok_list[0], "alpha") &&
             streq(g_tok_list[1], "beta") &&
             streq(g_tok_list[2], "gamma") &&
             streq(g_tok_list[3], "delta");

    x_strlcpy(g_path, TF, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (ok) out("XARGS SELFTEST: PASS\n");
    else    out("XARGS SELFTEST: FAIL\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return xargs_run(argc, argv);
}

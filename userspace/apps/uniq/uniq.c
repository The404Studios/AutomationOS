/*
 * uniq.c -- minimal freestanding `uniq` for the from-scratch x86_64 OS.
 * =====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled helpers. All
 * output goes to fd 1 (SYS_WRITE).
 *
 * Collapses ADJACENT identical lines (classic `uniq` semantics -- it does NOT
 * sort, it only folds runs of consecutive duplicates).
 *
 * Usage:
 *   uniq FILE        collapse adjacent identical lines, print survivors.
 *   uniq -c FILE     prefix each line with its run count ("<n> line").
 *   uniq -d FILE     print only lines that were duplicated (run length > 1),
 *                    once each.
 *   uniq -u FILE     print only lines that were NOT duplicated (run length 1).
 *   uniq             (argc<=1) run the built-in self-test, printing
 *                    "UNIQ SELFTEST: PASS" or "UNIQ SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and objdump shows an fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/uniq/uniq.c -o /tmp/uniq.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/uniq.o -o /tmp/uniq.elf
 *   objdump -d /tmp/uniq.elf | grep 'fs:0x28'   # MUST produce no output
 */

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY  0x0000

#define KPATH_MAX 4096

typedef unsigned long size_t;

/* -----------------------------------------------------------------------
 * 6-arg inline syscall wrapper (rdi, rsi, rdx, r10, r8).
 * --------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers (our own tiny libc).
 * ======================================================================= */
static size_t u_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void u_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int u_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n, 0, 0); }
static void out(const char *s)           { out_n(s, (long)u_strlen(s)); }

/* =======================================================================
 *  Buffers (static; off the tiny 64KB user stack).
 * ======================================================================= */
#define INBUF_MAX  (64 * 1024)
#define OUTBUF_MAX (96 * 1024)

static char g_in[INBUF_MAX]    __attribute__((aligned(16)));
static char g_out[OUTBUF_MAX]  __attribute__((aligned(16)));
static long g_out_len;
static char g_path[KPATH_MAX]  __attribute__((aligned(16)));

static void ob_putc(char c) { if (g_out_len < OUTBUF_MAX) g_out[g_out_len++] = c; }
static void ob_put(const char *s, long n) { for (long i = 0; i < n; i++) ob_putc(s[i]); }
static void ob_num(unsigned long v) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) ob_putc(b[--i]);
}

/* =======================================================================
 *  File slurp: read whole file at `path` into g_in. Returns bytes read
 *  (>=0), -1 if it could not be opened, -2 if larger than the buffer.
 * ======================================================================= */
static long slurp(const char *path) {
    u_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = INBUF_MAX - total;
        if (room <= 0) {
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1, 0, 0);
            sc(SYS_CLOSE, fd, 0, 0, 0, 0);
            return (n > 0) ? -2 : total;
        }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

/* =======================================================================
 *  Core: collapse adjacent identical lines from buf[0..len) into g_out.
 *
 *  Lines are delimited by '\n'. "Identical" compares the line content WITHOUT
 *  its trailing newline. A run is a maximal block of consecutive equal lines.
 *  Flags:
 *    flag_c : prefix each emitted line with its run count + a space.
 *    flag_d : emit only runs with count > 1.
 *    flag_u : emit only runs with count == 1.
 *  (-d and -u together emit nothing, matching GNU uniq.)
 *
 *  Each emitted line keeps a trailing '\n'. A final line lacking a newline is
 *  emitted without one (unless a count prefix is added, in which case GNU adds
 *  no newline either -- we mirror the input's terminator).
 * ======================================================================= */
static void uniq_core(const char *buf, long len, int flag_c, int flag_d, int flag_u) {
    g_out_len = 0;
    long i = 0;
    /* iteration guard: at most one pass per byte */
    long guard = 0;
    while (i < len && guard <= len) {
        guard++;
        /* current line span [i, e); had_nl indicates a trailing '\n' */
        long e = i;
        while (e < len && buf[e] != '\n') e++;
        int had_nl = (e < len);
        long clen = e - i;                 /* content length (no newline) */
        const char *cur = buf + i;

        /* count consecutive identical following lines */
        unsigned long count = 1;
        long next = had_nl ? e + 1 : len;  /* start of next line */
        long scan = next;
        long iguard = 0;
        while (scan <= len && iguard <= len) {
            iguard++;
            if (scan >= len) break;
            long se = scan;
            while (se < len && buf[se] != '\n') se++;
            long slen = se - scan;
            int same = (slen == clen);
            if (same) for (long k = 0; k < clen; k++) if (buf[scan + k] != cur[k]) { same = 0; break; }
            if (!same) break;
            count++;
            int s_had_nl = (se < len);
            scan = s_had_nl ? se + 1 : len;
            if (!s_had_nl) { next = len; break; }
            next = scan;
        }

        /* decide whether to emit this run */
        int emit = 1;
        if (flag_d && count <= 1) emit = 0;
        if (flag_u && count >  1) emit = 0;

        if (emit) {
            if (flag_c) { ob_num(count); ob_putc(' '); }
            ob_put(cur, clen);
            if (had_nl) ob_putc('\n');
        }

        i = next;
        if (!had_nl) break;                /* consumed to end */
    }
}

/* =======================================================================
 *  uniq_run -- argv-driven entry.  uniq [-c|-d|-u] FILE
 * ======================================================================= */
static int uniq_run(int argc, char **argv) {
    int flag_c = 0, flag_d = 0, flag_u = 0;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if      (u_streq(argv[ai], "-c")) flag_c = 1;
        else if (u_streq(argv[ai], "-d")) flag_d = 1;
        else if (u_streq(argv[ai], "-u")) flag_u = 1;
        else { out("uniq: unknown option: "); out(argv[ai]); out("\n"); return 1; }
        ai++;
    }
    if (ai >= argc || !argv[ai]) { out("usage: uniq [-c|-d|-u] FILE\n"); return 1; }

    long n = slurp(argv[ai]);
    if (n == -1) { out("uniq: cannot open '"); out(argv[ai]); out("'\n"); return 1; }
    if (n == -2) { out("uniq: input too large (>64KB)\n"); return 1; }

    uniq_core(g_in, n, flag_c, flag_d, flag_u);
    out_n(g_out, g_out_len);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (in-memory, no files).
 * ======================================================================= */
static int bytes_eq(const char *a, long alen, const char *b, long blen) {
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int t_case(const char *in, int fc, int fd, int fu, const char *expect) {
    long ilen = (long)u_strlen(in);
    uniq_core(in, ilen, fc, fd, fu);
    long elen = (long)u_strlen(expect);
    if (bytes_eq(g_out, g_out_len, expect, elen)) return 1;
    out("  uniq case FAIL\n    got:  "); out_n(g_out, g_out_len);
    out("\n    want: "); out(expect); out("\n");
    return 0;
}

static int selftest(void) {
    out("UNIQ: selftest begin\n");
    int ok = 1;
    /* plain: "a\na\nb\n" -> "a\nb\n" */
    ok &= t_case("a\na\nb\n", 0, 0, 0, "a\nb\n");
    /* -c: "a\na\nb\n" -> "2 a\n1 b\n" */
    ok &= t_case("a\na\nb\n", 1, 0, 0, "2 a\n1 b\n");
    /* -d (only duplicated): "a\na\nb\nc\nc\nc\n" -> "a\nc\n" */
    ok &= t_case("a\na\nb\nc\nc\nc\n", 0, 1, 0, "a\nc\n");
    /* -u (only unique): "a\na\nb\nc\nc\nc\n" -> "b\n" */
    ok &= t_case("a\na\nb\nc\nc\nc\n", 0, 0, 1, "b\n");
    /* final line without newline preserved */
    ok &= t_case("x\nx\ny", 0, 0, 0, "x\ny");

    if (ok) { out("UNIQ SELFTEST: PASS\n"); return 0; }
    out("UNIQ SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return uniq_run(argc, argv);
    (void)selftest();
    return 0;
}

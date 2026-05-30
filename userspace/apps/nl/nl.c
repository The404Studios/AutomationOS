/*
 * nl.c -- minimal freestanding `nl` for the from-scratch x86_64 OS.
 * =================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled helpers. All
 * output goes to fd 1 (SYS_WRITE).
 *
 * Numbers lines of a file. By default (like GNU `nl`) only NON-EMPTY lines
 * receive a number; empty lines are passed through unnumbered. The number is
 * right-aligned in a fixed-width field, followed by a TAB, then the line.
 *
 * Usage:
 *   nl FILE        number non-empty lines.
 *   nl -ba FILE    number ALL lines, including empty ones.
 *   nl             (argc<=1) run the built-in self-test, printing
 *                  "NL SELFTEST: PASS" or "NL SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/nl/nl.c -o /tmp/nl.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/nl.o -o /tmp/nl.elf
 *   objdump -d /tmp/nl.elf | grep 'fs:0x28'   # MUST produce no output
 */

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY  0x0000
#define KPATH_MAX 4096

typedef unsigned long size_t;

/* 6-arg inline syscall wrapper (rdi, rsi, rdx, r10, r8). */
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
 *  Freestanding helpers.
 * ======================================================================= */
static size_t n_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void n_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int n_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n, 0, 0); }
static void out(const char *s)           { out_n(s, (long)n_strlen(s)); }

/* =======================================================================
 *  Buffers.
 * ======================================================================= */
#define INBUF_MAX  (64 * 1024)
#define OUTBUF_MAX (96 * 1024)

static char g_in[INBUF_MAX]   __attribute__((aligned(16)));
static char g_out[OUTBUF_MAX] __attribute__((aligned(16)));
static long g_out_len;
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

static void ob_putc(char c) { if (g_out_len < OUTBUF_MAX) g_out[g_out_len++] = c; }
static void ob_put(const char *s, long n) { for (long i = 0; i < n; i++) ob_putc(s[i]); }

/* Emit `v` right-aligned in a field of `width` (space-padded) into g_out. */
#define NUM_WIDTH 6
static void ob_num_right(unsigned long v, int width) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    for (int pad = width - i; pad > 0; pad--) ob_putc(' ');
    while (i > 0) ob_putc(b[--i]);
}

/* =======================================================================
 *  Core: number lines of buf[0..len) into g_out.
 *
 *  number_all == 0 : only non-empty lines get a number (GNU `nl` default).
 *  number_all == 1 : every line gets a number (`nl -ba`).
 *
 *  Format for a numbered line:  "<right-aligned num>\t<line>\n".
 *  Unnumbered (empty) lines are emitted as just "\n".
 * ======================================================================= */
static void nl_core(const char *buf, long len, int number_all) {
    g_out_len = 0;
    unsigned long lineno = 0;
    long start = 0;
    long guard = 0;
    while (start < len && guard <= len) {
        guard++;
        long e = start;
        while (e < len && buf[e] != '\n') e++;
        int had_nl = (e < len);
        long llen = e - start;
        const char *line = buf + start;

        int is_empty = (llen == 0);
        int number_it = number_all || !is_empty;

        if (number_it) {
            lineno++;
            ob_num_right(lineno, NUM_WIDTH);
            ob_putc('\t');
            ob_put(line, llen);
        } else {
            ob_put(line, llen);   /* empty line: nothing to copy */
        }
        if (had_nl) ob_putc('\n');

        if (!had_nl) break;
        start = e + 1;
    }
}

/* =======================================================================
 *  nl_run -- argv-driven entry.  nl [-ba] FILE
 * ======================================================================= */
static int nl_run(int argc, char **argv) {
    int number_all = 0;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (n_streq(argv[ai], "-ba"))      number_all = 1;
        else if (n_streq(argv[ai], "-bt")) number_all = 0;   /* explicit default */
        else { out("nl: unknown option: "); out(argv[ai]); out("\n"); return 1; }
        ai++;
    }
    if (ai >= argc || !argv[ai]) { out("usage: nl [-ba] FILE\n"); return 1; }

    n_strlcpy(g_path, argv[ai], KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) { out("nl: cannot open '"); out(argv[ai]); out("'\n"); return 1; }
    long total = 0;
    for (;;) {
        long room = INBUF_MAX - total;
        if (room <= 0) { out("nl: input too large (>64KB)\n"); sc(SYS_CLOSE, fd, 0, 0, 0, 0); return 1; }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);

    nl_core(g_in, total, number_all);
    out_n(g_out, g_out_len);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (in-memory).
 *
 *  "x\n\ny\n": default numbers line 1 (x) and line 2 (y), skipping the blank.
 *  Expected (NUM_WIDTH=6):
 *      "     1\tx\n" + "\n" + "     2\ty\n"
 * ======================================================================= */
static int bytes_eq(const char *a, long alen, const char *b, long blen) {
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int t_case(const char *in, int number_all, const char *expect) {
    nl_core(in, (long)n_strlen(in), number_all);
    long elen = (long)n_strlen(expect);
    if (bytes_eq(g_out, g_out_len, expect, elen)) return 1;
    out("  nl case FAIL\n    got:  "); out_n(g_out, g_out_len);
    out("\n    want: "); out(expect); out("\n");
    return 0;
}

static int selftest(void) {
    out("NL: selftest begin\n");
    int ok = 1;
    /* default: number non-empty lines only; blank line skipped */
    ok &= t_case("x\n\ny\n", 0, "     1\tx\n\n     2\ty\n");
    /* -ba: number all lines including the blank one */
    ok &= t_case("x\n\ny\n", 1, "     1\tx\n     2\t\n     3\ty\n");
    /* single line, no trailing newline */
    ok &= t_case("hi", 0, "     1\thi");

    if (ok) { out("NL SELFTEST: PASS\n"); return 0; }
    out("NL SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return nl_run(argc, argv);
    (void)selftest();
    return 0;
}

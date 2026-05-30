/*
 * diff.c -- minimal freestanding `diff` for the from-scratch x86_64 OS.
 * =====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers. All output to fd 1.
 *
 * This is a SIMPLE positional line diff (NOT a full LCS): it walks both
 * files line by line in lockstep. For each line index it compares line A[i]
 * to line B[i]; when they differ it prints the GNU-ish:
 *
 *   <line> N
 *   < TEXT-FROM-A          (omitted if A ran out of lines)
 *   > TEXT-FROM-B          (omitted if B ran out of lines)
 *
 * Usage:
 *   diff A B     compare files A and B line-by-line.
 *                exit 0 if identical, 1 if they differ (2 on error).
 *   diff         (argc<=1) run the built-in self-test, printing
 *                "DIFF SELFTEST: PASS" or "DIFF SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/diff/diff.c -o /tmp/diff.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/diff.o -o /tmp/diff.elf
 *   objdump -d /tmp/diff.elf | grep 'fs:0x28'   # must produce no output
 */

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_UNLINK 34

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

#define KPATH_MAX 4096

static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- helpers ---- */
static unsigned long d_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void d_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)d_strlen(s)); }
static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n); }
static void out_num(long n) {
    char b[24]; int i = 0;
    if (n == 0) { char z = '0'; sc(SYS_WRITE, 1, (long)&z, 1); return; }
    unsigned long u = (unsigned long)n;
    do { b[i++] = (char)('0' + (u % 10)); u /= 10; } while (u > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1); }
}

/* ======================================================================
 * Two file buffers (static -- the user stack is small). 64 KB each as
 * specified for the file tools.
 * ==================================================================== */
#define BUF_MAX (64 * 1024)
static char g_a[BUF_MAX] __attribute__((aligned(16)));
static char g_b[BUF_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

/* Read whole file into buf[BUF_MAX]. Returns bytes (>=0), -1 open fail,
 * -2 too large. */
static long slurp(const char *path, char *buf) {
    d_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = BUF_MAX - total;
        if (room <= 0) {
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1);
            sc(SYS_CLOSE, fd, 0, 0);
            return (n > 0) ? -2 : total;
        }
        long n = sc(SYS_READ, fd, (long)(buf + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Find the end (exclusive of '\n') of the line starting at `start` within
 * buf[0..len). Returns the index of '\n' or `len` if the line is unterminated.
 */
static long line_end(const char *buf, long len, long start) {
    long e = start;
    while (e < len && buf[e] != '\n') e++;
    return e;
}

/* Are the two byte ranges equal? */
static int range_eq(const char *a, long alen, const char *b, long blen) {
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ======================================================================
 * diff_core -- positional line diff of a[0..alen) vs b[0..blen).
 * Returns 0 if identical, 1 if they differ. `emit` controls printing.
 * ==================================================================== */
static int diff_core(const char *a, long alen, const char *b, long blen, int emit) {
    long ai = 0, bi = 0;
    long lineno = 0;
    int differ = 0;

    while (ai < alen || bi < blen) {
        lineno++;
        int has_a = (ai < alen);
        int has_b = (bi < blen);

        long ae = has_a ? line_end(a, alen, ai) : ai;
        long be = has_b ? line_end(b, blen, bi) : bi;
        long alen_l = ae - ai;
        long blen_l = be - bi;

        int same = has_a && has_b && range_eq(a + ai, alen_l, b + bi, blen_l);
        if (!same) {
            differ = 1;
            if (emit) {
                out("line "); out_num(lineno); out("\n");
                if (has_a) { out("< "); out_n(a + ai, alen_l); out("\n"); }
                if (has_b) { out("> "); out_n(b + bi, blen_l); out("\n"); }
            }
        }

        if (has_a) ai = (ae < alen) ? ae + 1 : alen + 1;  /* advance past line */
        if (has_b) bi = (be < blen) ? be + 1 : blen + 1;
    }
    return differ;
}

/* ======================================================================
 * diff_run -- argv-driven entry.  diff A B
 * Returns 0 identical, 1 differ, 2 on error.
 * ==================================================================== */
static int diff_run(int argc, char **argv) {
    if (argc < 3 || !argv[1] || !argv[2]) {
        out("usage: diff A B\n");
        return 2;
    }
    long na = slurp(argv[1], g_a);
    if (na == -1) { out("diff: cannot open '"); out(argv[1]); out("'\n"); return 2; }
    if (na == -2) { out("diff: '"); out(argv[1]); out("' too large (>64KB)\n"); return 2; }
    long nb = slurp(argv[2], g_b);
    if (nb == -1) { out("diff: cannot open '"); out(argv[2]); out("'\n"); return 2; }
    if (nb == -2) { out("diff: '"); out(argv[2]); out("' too large (>64KB)\n"); return 2; }

    return diff_core(g_a, na, g_b, nb, 1) ? 1 : 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * Writes two temp files differing on line 2, verifies diff_core reports a
 * difference, then writes two identical buffers and verifies it reports none.
 * Prints DIFF SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TA "/tmp/diff_a.txt"
#define TB "/tmp/diff_b.txt"

static int write_file(const char *path, const char *content) {
    d_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long len = d_strlen(content), off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(content + off), (long)(len - off));
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += (unsigned long)w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

static void selftest(void) {
    out("DIFF: selftest begin\n");

    const char *ca = "same line\nold middle\ntail\n";
    const char *cb = "same line\nnew middle\ntail\n";
    if (write_file(TA, ca) != 0 || write_file(TB, cb) != 0) {
        out("DIFF SELFTEST: FAIL (could not write temp files)\n");
        return;
    }

    long na = slurp(TA, g_a);
    long nb = slurp(TB, g_b);
    if (na < 0 || nb < 0) {
        out("DIFF SELFTEST: FAIL (could not read temp files)\n");
        return;
    }

    /* differing files -> must report a difference (no print). */
    int d1 = diff_core(g_a, na, g_b, nb, 0);
    /* identical buffers -> must report no difference. */
    int d2 = diff_core(g_a, na, g_a, na, 0);

    /* informational: show the real diff output of A vs B */
    out("DIFF: A vs B ->\n");
    (void)diff_core(g_a, na, g_b, nb, 1);

    /* clean up */
    d_strlcpy(g_path, TA, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);
    d_strlcpy(g_path, TB, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (d1 == 1 && d2 == 0) out("DIFF SELFTEST: PASS\n");
    else                    out("DIFF SELFTEST: FAIL\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return diff_run(argc, argv);
}

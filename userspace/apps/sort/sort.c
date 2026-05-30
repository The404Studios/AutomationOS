/*
 * sort.c -- minimal freestanding `sort` for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled string helpers.
 * Single self-contained file. All output to fd 1 (SYS_WRITE).
 *
 * Reads a FILE into a static buffer, indexes its lines into a fixed-size array
 * of (offset,length) records, sorts them, and prints the result.
 *
 * Usage:
 *   sort [-r] [-n] [-u] FILE
 *       -r  reverse the sort order
 *       -n  numeric sort (compare leading integer value, ties broken
 *           lexicographically)
 *       -u  unique: drop adjacent equal lines after sorting
 *   sort            (argc<=1) run the built-in self-test, printing
 *                   "SORT SELFTEST: PASS" or "SORT SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/sort/sort.c -o sort.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o sort.o -o build/sort
 *   objdump -d build/sort | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers -- verified against kernel/include/syscall.h.
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY  0x0000

#define KPATH_MAX 4096

typedef unsigned long size_t;

/* -----------------------------------------------------------------------
 * 6-argument inline syscall wrapper (rdi/rsi/rdx/r10/r8 for args 1..5).
 * --------------------------------------------------------------------- */
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

/* =======================================================================
 *  Freestanding string helpers.
 * ======================================================================= */
static size_t s_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static size_t s_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* =======================================================================
 *  Output helpers.
 * ======================================================================= */
static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0); }
static void out(const char *s)             { out_n(s, s_strlen(s)); }

/* =======================================================================
 *  Static buffers + line index. Lines capped at MAX_LINES; the file buffer
 *  at FILE_MAX. Each record holds the byte offset/length of a line (without
 *  its trailing newline) inside the source buffer.
 * ======================================================================= */
#define FILE_MAX  (256 * 1024)
#define MAX_LINES 8192

static char g_in[FILE_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

typedef struct { long off; long len; } line_t;
static line_t g_line[MAX_LINES];
static int    g_nlines;

static long slurp(const char *path)
{
    s_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_MAX - total;
        if (room <= 0) break;
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

/* Split buf[0..len) into the g_line[] index (newline-delimited; trailing
 * newline does not create an empty final record). Caps at MAX_LINES. */
static void index_lines(const char *buf, long len)
{
    g_nlines = 0;
    long start = 0;
    while (start < len && g_nlines < MAX_LINES) {
        long e = start;
        while (e < len && buf[e] != '\n') e++;
        g_line[g_nlines].off = start;
        g_line[g_nlines].len = e - start;
        g_nlines++;
        if (e >= len) break;            /* last line, no trailing newline */
        start = e + 1;
    }
}

/* =======================================================================
 *  Comparison.
 *
 *  Lexicographic byte compare of two line records (shorter is "less" on a
 *  common prefix). Returns <0, 0, >0.
 * ======================================================================= */
static int cmp_lex(const char *buf, const line_t *a, const line_t *b)
{
    long n = a->len < b->len ? a->len : b->len;
    for (long i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)buf[a->off + i];
        unsigned char cb = (unsigned char)buf[b->off + i];
        if (ca != cb) return (int)ca - (int)cb;
    }
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

/* Parse a leading (optionally signed) integer from a line record. Stops at
 * the first non-digit. Used for -n numeric ordering. */
static long line_to_num(const char *buf, const line_t *l)
{
    long i = 0, sign = 1, v = 0;
    while (i < l->len && (buf[l->off + i] == ' ' || buf[l->off + i] == '\t')) i++;
    if (i < l->len && (buf[l->off + i] == '-' || buf[l->off + i] == '+')) {
        if (buf[l->off + i] == '-') sign = -1;
        i++;
    }
    while (i < l->len && buf[l->off + i] >= '0' && buf[l->off + i] <= '9') {
        v = v * 10 + (buf[l->off + i] - '0');
        i++;
    }
    return sign * v;
}

/* Numeric comparison; ties broken lexicographically for a stable feel. */
static int cmp_num(const char *buf, const line_t *a, const line_t *b)
{
    long na = line_to_num(buf, a), nb = line_to_num(buf, b);
    if (na < nb) return -1;
    if (na > nb) return 1;
    return cmp_lex(buf, a, b);
}

/* One comparison entry point honouring the numeric / reverse flags. */
static int line_cmp(const char *buf, const line_t *a, const line_t *b,
                    int numeric, int reverse)
{
    int c = numeric ? cmp_num(buf, a, b) : cmp_lex(buf, a, b);
    return reverse ? -c : c;
}

/* =======================================================================
 *  Insertion sort over the index (stable; bounded; no recursion, no libc).
 *  MAX_LINES is modest (8192) so O(n^2) worst case stays well bounded.
 * ======================================================================= */
static void sort_lines(const char *buf, int numeric, int reverse)
{
    for (int i = 1; i < g_nlines; i++) {
        line_t key = g_line[i];
        int j = i - 1;
        while (j >= 0 && line_cmp(buf, &g_line[j], &key, numeric, reverse) > 0) {
            g_line[j + 1] = g_line[j];
            j--;
        }
        g_line[j + 1] = key;
    }
}

/* =======================================================================
 *  Emit the sorted (optionally uniq'd) lines, each newline-terminated.
 *  With -u, adjacent records equal by *lexicographic* content are collapsed.
 *  Returns the number of lines emitted.
 * ======================================================================= */
static int emit_lines(const char *buf, int unique)
{
    int emitted = 0;
    for (int i = 0; i < g_nlines; i++) {
        if (unique && i > 0 && cmp_lex(buf, &g_line[i - 1], &g_line[i]) == 0)
            continue;
        out_n(buf + g_line[i].off, g_line[i].len);
        out("\n");
        emitted++;
    }
    return emitted;
}

/* =======================================================================
 *  sort_run -- argv-driven entry: sort [-r] [-n] [-u] FILE
 * ======================================================================= */
static int sort_run(int argc, char **argv)
{
    int opt_r = 0, opt_n = 0, opt_u = 0;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        const char *f = argv[ai];
        for (int k = 1; f[k]; k++) {
            if (f[k] == 'r') opt_r = 1;
            else if (f[k] == 'n') opt_n = 1;
            else if (f[k] == 'u') opt_u = 1;
            else { out("sort: unknown option: "); out(f); out("\n"); return 1; }
        }
        ai++;
    }

    if (ai >= argc || !argv[ai]) { out("usage: sort [-r] [-n] [-u] FILE\n"); return 1; }
    const char *infile = argv[ai++];

    long len = slurp(infile);
    if (len < 0) { out("sort: cannot open '"); out(infile); out("'\n"); return 1; }

    index_lines(g_in, len);
    sort_lines(g_in, opt_n, opt_r);
    emit_lines(g_in, opt_u);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (no filesystem access; pure in-memory).
 *
 *  Sample "banana\napple\ncherry\n"; plain sort must yield apple, banana,
 *  cherry. We verify the resulting index order against the expectation, and
 *  also exercise -r, -n, and -u.
 * ======================================================================= */
static int line_is(const char *buf, const line_t *l, const char *want)
{
    long wl = (long)s_strlen(want);
    if (l->len != wl) return 0;
    for (long i = 0; i < wl; i++) if (buf[l->off + i] != want[i]) return 0;
    return 1;
}

static int selftest(void)
{
    out("SORT SELFTEST: begin\n");

    int ok = 1;

    /* 1) plain lexicographic sort. */
    const char *s1 = "banana\napple\ncherry\n";
    index_lines(s1, (long)s_strlen(s1));
    sort_lines(s1, 0, 0);
    if (g_nlines != 3 ||
        !line_is(s1, &g_line[0], "apple")  ||
        !line_is(s1, &g_line[1], "banana") ||
        !line_is(s1, &g_line[2], "cherry")) {
        out("  FAIL: lexicographic order\n"); ok = 0;
    }

    /* 2) reverse. */
    index_lines(s1, (long)s_strlen(s1));
    sort_lines(s1, 0, 1);
    if (g_nlines != 3 ||
        !line_is(s1, &g_line[0], "cherry") ||
        !line_is(s1, &g_line[1], "banana") ||
        !line_is(s1, &g_line[2], "apple")) {
        out("  FAIL: reverse order\n"); ok = 0;
    }

    /* 3) numeric: "10\n2\n1\n" -> 1, 2, 10 (lexicographic would give 1,10,2). */
    const char *s2 = "10\n2\n1\n";
    index_lines(s2, (long)s_strlen(s2));
    sort_lines(s2, 1, 0);
    if (g_nlines != 3 ||
        !line_is(s2, &g_line[0], "1")  ||
        !line_is(s2, &g_line[1], "2")  ||
        !line_is(s2, &g_line[2], "10")) {
        out("  FAIL: numeric order\n"); ok = 0;
    }

    /* 4) unique after sort: "b\na\nb\na\n" sorts to a,a,b,b; -u -> a,b (2). */
    const char *s3 = "b\na\nb\na\n";
    index_lines(s3, (long)s_strlen(s3));
    sort_lines(s3, 0, 0);
    {
        /* count unique without printing: replicate emit_lines' uniq test */
        int uniq = 0;
        for (int i = 0; i < g_nlines; i++)
            if (!(i > 0 && cmp_lex(s3, &g_line[i - 1], &g_line[i]) == 0)) uniq++;
        if (uniq != 2) { out("  FAIL: unique count != 2\n"); ok = 0; }
    }

    /* Visual confirmation of the plain sort. */
    out("SORT SELFTEST: sort ->\n");
    index_lines(s1, (long)s_strlen(s1));
    sort_lines(s1, 0, 0);
    emit_lines(s1, 0);

    if (ok) { out("SORT SELFTEST: PASS\n"); return 0; }
    out("SORT SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1) return sort_run(argc, argv);
    (void)selftest();
    return 0;
}

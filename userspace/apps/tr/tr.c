/*
 * tr.c -- minimal freestanding `tr` for the from-scratch x86_64 OS.
 * =================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled helpers. All
 * output goes to fd 1 (SYS_WRITE).
 *
 * This OS has no stdin, so `tr` reads a FILE argument instead of stdin.
 *
 * Usage:
 *   tr SET1 SET2 FILE   translate: each char in SET1 is mapped, by position,
 *                       to the char at the same index in SET2. If SET1 is
 *                       longer than SET2, the surplus SET1 chars all map to
 *                       the LAST char of SET2 (GNU default).
 *   tr -d SET1 FILE     delete: drop every char that appears in SET1.
 *   tr                  (argc<=1) run the built-in self-test, printing
 *                       "TR SELFTEST: PASS" or "TR SELFTEST: FAIL".
 *
 *   SETs support simple ranges, e.g. "a-z", "A-Z", "0-9". A literal '-' is
 *   one at the start or end of a set. Sets are expanded into byte arrays.
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tr/tr.c -o /tmp/tr.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/tr.o -o /tmp/tr.elf
 *   objdump -d /tmp/tr.elf | grep 'fs:0x28'   # MUST produce no output
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
static size_t r_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void r_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int r_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n, 0, 0); }
static void out(const char *s)           { out_n(s, (long)r_strlen(s)); }

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

/* =======================================================================
 *  Set expansion. A SET like "a-z0-9_" expands into a flat byte array.
 *  Bounded to SET_MAX bytes.
 * ======================================================================= */
#define SET_MAX 1024

/* Expand `set` into out[] (capacity SET_MAX). Returns expanded length.
 * Supports M-N ranges; a '-' at the very start or end is literal. */
static int expand_set(const char *set, unsigned char *out_buf) {
    int len = 0;
    const char *p = set;
    long guard = 0;
    while (*p && len < SET_MAX && guard < 100000) {
        guard++;
        unsigned char lo = (unsigned char)*p;
        if (p[1] == '-' && p[2] != '\0') {
            unsigned char hi = (unsigned char)p[2];
            if (hi >= lo) {
                for (int c = lo; c <= hi && len < SET_MAX; c++)
                    out_buf[len++] = (unsigned char)c;
            } else {
                out_buf[len++] = lo;     /* malformed range: literal lo */
            }
            p += 3;
        } else {
            out_buf[len++] = lo;
            p++;
        }
    }
    return len;
}

static unsigned char g_set1[SET_MAX];
static unsigned char g_set2[SET_MAX];

/* =======================================================================
 *  Core (translate). Build a 256-entry map: map[c] = replacement byte.
 *  Surplus SET1 entries (beyond len(SET2)) map to the last SET2 byte.
 * ======================================================================= */
static void tr_translate_core(const char *buf, long len,
                              const unsigned char *s1, int n1,
                              const unsigned char *s2, int n2) {
    unsigned char map[256];
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    for (int i = 0; i < n1; i++) {
        unsigned char repl;
        if (n2 == 0)            repl = s1[i];          /* defensive */
        else if (i < n2)        repl = s2[i];
        else                    repl = s2[n2 - 1];     /* pad with last */
        map[s1[i]] = repl;
    }
    g_out_len = 0;
    for (long i = 0; i < len; i++) ob_putc((char)map[(unsigned char)buf[i]]);
}

/* =======================================================================
 *  Core (delete). Drop every byte present in SET1.
 * ======================================================================= */
static void tr_delete_core(const char *buf, long len,
                           const unsigned char *s1, int n1) {
    unsigned char drop[256];
    for (int i = 0; i < 256; i++) drop[i] = 0;
    for (int i = 0; i < n1; i++) drop[s1[i]] = 1;
    g_out_len = 0;
    for (long i = 0; i < len; i++)
        if (!drop[(unsigned char)buf[i]]) ob_putc(buf[i]);
}

/* =======================================================================
 *  tr_run -- argv-driven entry.
 *    tr SET1 SET2 FILE
 *    tr -d SET1 FILE
 * ======================================================================= */
static int tr_run(int argc, char **argv) {
    int del = 0;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (r_streq(argv[ai], "-d")) del = 1;
        else { out("tr: unknown option: "); out(argv[ai]); out("\n"); return 1; }
        ai++;
    }

    const char *set1 = 0, *set2 = 0, *file = 0;
    if (del) {
        if (ai + 1 >= argc) { out("usage: tr -d SET1 FILE\n"); return 1; }
        set1 = argv[ai]; file = argv[ai + 1];
    } else {
        if (ai + 2 >= argc) { out("usage: tr SET1 SET2 FILE\n"); return 1; }
        set1 = argv[ai]; set2 = argv[ai + 1]; file = argv[ai + 2];
    }

    int n1 = expand_set(set1, g_set1);
    int n2 = del ? 0 : expand_set(set2, g_set2);

    r_strlcpy(g_path, file, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) { out("tr: cannot open '"); out(file); out("'\n"); return 1; }
    long total = 0;
    for (;;) {
        long room = INBUF_MAX - total;
        if (room <= 0) { out("tr: input too large (>64KB)\n"); sc(SYS_CLOSE, fd, 0, 0, 0, 0); return 1; }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);

    if (del) tr_delete_core(g_in, total, g_set1, n1);
    else     tr_translate_core(g_in, total, g_set1, n1, g_set2, n2);

    out_n(g_out, g_out_len);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (in-memory).
 * ======================================================================= */
static int bytes_eq(const char *a, long alen, const char *b, long blen) {
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int t_xlate(const char *s1, const char *s2, const char *in, const char *expect) {
    int n1 = expand_set(s1, g_set1);
    int n2 = expand_set(s2, g_set2);
    tr_translate_core(in, (long)r_strlen(in), g_set1, n1, g_set2, n2);
    long elen = (long)r_strlen(expect);
    if (bytes_eq(g_out, g_out_len, expect, elen)) return 1;
    out("  tr xlate FAIL\n    got:  "); out_n(g_out, g_out_len);
    out("\n    want: "); out(expect); out("\n");
    return 0;
}

static int t_del(const char *s1, const char *in, const char *expect) {
    int n1 = expand_set(s1, g_set1);
    tr_delete_core(in, (long)r_strlen(in), g_set1, n1);
    long elen = (long)r_strlen(expect);
    if (bytes_eq(g_out, g_out_len, expect, elen)) return 1;
    out("  tr del FAIL\n    got:  "); out_n(g_out, g_out_len);
    out("\n    want: "); out(expect); out("\n");
    return 0;
}

static int selftest(void) {
    out("TR: selftest begin\n");
    int ok = 1;
    /* tr a-z A-Z of "abc" -> "ABC" */
    ok &= t_xlate("a-z", "A-Z", "abc", "ABC");
    /* positional single chars */
    ok &= t_xlate("abc", "xyz", "cab\n", "zxy\n");
    /* surplus SET1 maps to last SET2 char: "abc" -> "x" each */
    ok &= t_xlate("abc", "x", "cab", "xxx");
    /* digits to '#' via range */
    ok &= t_xlate("0-9", "#", "a1b2c3", "a#b#c#");
    /* delete vowels */
    ok &= t_del("aeiou", "hello world", "hll wrld");
    /* delete a range */
    ok &= t_del("0-9", "a1b2c3\n", "abc\n");

    if (ok) { out("TR SELFTEST: PASS\n"); return 0; }
    out("TR SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return tr_run(argc, argv);
    (void)selftest();
    return 0;
}

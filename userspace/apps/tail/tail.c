/*
 * tail.c -- minimal freestanding `tail` for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled string helpers.
 * Single self-contained file. All output to fd 1 (SYS_WRITE).
 *
 * Prints the last N lines of a FILE (default 10). The whole file is read into
 * a static buffer; we then walk backwards to find the start of the last N
 * lines and emit from there.
 *
 * Usage:
 *   tail [-n N] FILE
 *       -n N   print the last N lines (default 10)
 *   tail            (argc<=1) run the built-in self-test, printing
 *                   "TAIL SELFTEST: PASS" or "TAIL SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tail/tail.c -o tail.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o tail.o -o build/tail
 *   objdump -d build/tail | grep fs:0x28   # MUST be empty
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
static size_t t_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int t_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t t_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static long t_atoul(const char *s)
{
    if (!*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

/* =======================================================================
 *  Output helpers.
 * ======================================================================= */
static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0); }
static void out(const char *s)             { out_n(s, t_strlen(s)); }

/* =======================================================================
 *  Static buffers.
 * ======================================================================= */
#define FILE_MAX (256 * 1024)
static char g_in[FILE_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

static long slurp(const char *path)
{
    t_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_MAX - total;
        if (room <= 0) break;            /* keep first FILE_MAX bytes      */
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

/* =======================================================================
 *  Core: find the byte offset at which the last `n` lines begin.
 *
 *  We count newlines from the end. A trailing '\n' terminates the final line
 *  and is not itself the start of a new (empty) line for counting purposes.
 *  Returns the start offset (0..len); emit buf[off..len) to print the tail.
 * ======================================================================= */
static long tail_start(const char *buf, long len, long n)
{
    if (n <= 0) return len;             /* nothing to print               */
    if (len <= 0) return 0;

    /* If the file ends with '\n', ignore that terminator when scanning so it
     * does not count as an extra (empty) trailing line. */
    long end = len;
    if (buf[end - 1] == '\n') end--;

    long count = 0;                     /* newlines seen scanning backward */
    long i = end - 1;
    for (; i >= 0; i--) {
        if (buf[i] == '\n') {
            count++;
            if (count == n) return i + 1;   /* start just past this '\n'  */
        }
    }
    return 0;                           /* fewer than n lines -> whole file */
}

/* Emit the last `n` lines of buf[0..len). Returns the start offset used (so
 * the self-test can verify exactly which bytes were chosen). */
static long tail_core(const char *buf, long len, long n)
{
    long off = tail_start(buf, len, n);
    if (off < len) out_n(buf + off, len - off);
    return off;
}

/* =======================================================================
 *  tail_run -- argv-driven entry: tail [-n N] FILE
 * ======================================================================= */
static int tail_run(int argc, char **argv)
{
    long n = 10;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (t_streq(argv[ai], "-n")) {
            if (ai + 1 >= argc || !argv[ai + 1]) { out("tail: -n needs a number\n"); return 1; }
            long v = t_atoul(argv[ai + 1]);
            if (v < 0) { out("tail: invalid count\n"); return 1; }
            n = v;
            ai += 2;
        } else {
            out("tail: unknown option: "); out(argv[ai]); out("\n");
            return 1;
        }
    }

    if (ai >= argc || !argv[ai]) { out("usage: tail [-n N] FILE\n"); return 1; }
    const char *infile = argv[ai++];

    long len = slurp(infile);
    if (len < 0) { out("tail: cannot open '"); out(infile); out("'\n"); return 1; }

    tail_core(g_in, len, n);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (no filesystem access; pure in-memory).
 *
 *  5-line sample; tail -n 2 must select the last two lines ("l4\nl5\n").
 *  We verify the chosen start offset matches the known position of "l4".
 * ======================================================================= */
static int bytes_eq(const char *a, long alen, const char *b, long blen)
{
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int selftest(void)
{
    out("TAIL SELFTEST: begin\n");

    const char *sample = "l1\nl2\nl3\nl4\nl5\n";   /* 5 lines, 15 bytes */
    long slen = (long)t_strlen(sample);

    int ok = 1;

    /* tail -n 2 -> last 2 lines start at offset 9 ("l4\nl5\n"). */
    long off2 = tail_start(sample, slen, 2);
    const char *want2 = "l4\nl5\n";
    if (!bytes_eq(sample + off2, slen - off2, want2, (long)t_strlen(want2))) {
        out("  FAIL: -n 2 tail mismatch\n"); ok = 0;
    }

    /* tail -n 1 -> "l5\n". */
    long off1 = tail_start(sample, slen, 1);
    const char *want1 = "l5\n";
    if (!bytes_eq(sample + off1, slen - off1, want1, (long)t_strlen(want1))) {
        out("  FAIL: -n 1 tail mismatch\n"); ok = 0;
    }

    /* tail -n 10 over a 5-line sample -> whole file (offset 0). */
    long offall = tail_start(sample, slen, 10);
    if (offall != 0) { out("  FAIL: -n 10 offset != 0\n"); ok = 0; }

    /* last line without a trailing newline: "a\nb\nc" tail -n 2 -> "b\nc". */
    const char *nonl = "a\nb\nc";
    long offn = tail_start(nonl, (long)t_strlen(nonl), 2);
    const char *wantn = "b\nc";
    if (!bytes_eq(nonl + offn, (long)t_strlen(nonl) - offn, wantn, (long)t_strlen(wantn))) {
        out("  FAIL: no-final-newline tail mismatch\n"); ok = 0;
    }

    /* Visual confirmation. */
    out("TAIL SELFTEST: tail -n 2 ->\n");
    tail_core(sample, slen, 2);

    if (ok) { out("TAIL SELFTEST: PASS\n"); return 0; }
    out("TAIL SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1) return tail_run(argc, argv);
    (void)selftest();
    return 0;
}

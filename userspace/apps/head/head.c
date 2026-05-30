/*
 * head.c -- minimal freestanding `head` for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled string helpers.
 * Single self-contained file. All output to fd 1 (SYS_WRITE).
 *
 * Prints the first N lines of a FILE (default 10).
 *
 * Usage:
 *   head [-n N] FILE
 *       -n N   print the first N lines (default 10)
 *   head            (argc<=1) run the built-in self-test, printing
 *                   "HEAD SELFTEST: PASS" or "HEAD SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/head/head.c -o head.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o head.o -o build/head
 *   objdump -d build/head | grep fs:0x28   # MUST be empty
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
static size_t h_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int h_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t h_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* Parse a non-negative decimal integer. Returns -1 on any invalid char. */
static long h_atoul(const char *s)
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
static void out(const char *s)             { out_n(s, h_strlen(s)); }

/* =======================================================================
 *  Static buffers.
 * ======================================================================= */
#define FILE_MAX (256 * 1024)
static char g_in[FILE_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

static long slurp(const char *path)
{
    h_strlcpy(g_path, path, KPATH_MAX);
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
 *  Core: write the first `n` lines of buf[0..len) to fd 1. Returns the
 *  number of bytes written (so the self-test can verify exact output).
 *
 *  A "line" ends at '\n' (inclusive). A final line without a trailing '\n'
 *  still counts as a line and is emitted as-is.
 * ======================================================================= */
static long head_core(const char *buf, long len, long n)
{
    if (n <= 0) return 0;
    long start = 0;
    long printed = 0;       /* lines emitted so far */
    long written = 0;
    while (start < len && printed < n) {
        long e = start;
        while (e < len && buf[e] != '\n') e++;
        long seg = (e < len) ? (e - start + 1) : (e - start);  /* include \n */
        out_n(buf + start, seg);
        written += seg;
        printed++;
        if (e >= len) break;             /* last line, no newline          */
        start = e + 1;
    }
    return written;
}

/* =======================================================================
 *  head_run -- argv-driven entry: head [-n N] FILE
 * ======================================================================= */
static int head_run(int argc, char **argv)
{
    long n = 10;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (h_streq(argv[ai], "-n")) {
            if (ai + 1 >= argc || !argv[ai + 1]) { out("head: -n needs a number\n"); return 1; }
            long v = h_atoul(argv[ai + 1]);
            if (v < 0) { out("head: invalid count\n"); return 1; }
            n = v;
            ai += 2;
        } else {
            out("head: unknown option: "); out(argv[ai]); out("\n");
            return 1;
        }
    }

    if (ai >= argc || !argv[ai]) { out("usage: head [-n N] FILE\n"); return 1; }
    const char *infile = argv[ai++];

    long len = slurp(infile);
    if (len < 0) { out("head: cannot open '"); out(infile); out("'\n"); return 1; }

    head_core(g_in, len, n);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (no filesystem access; pure in-memory).
 *
 *  5-line sample; head -n 2 must emit exactly the first two lines and report
 *  the correct byte count. We count emitted lines by re-scanning the sample
 *  ourselves (head_core writes to fd1; we validate the byte length it
 *  returns against the known prefix length).
 * ======================================================================= */
static int selftest(void)
{
    out("HEAD SELFTEST: begin\n");

    const char *sample = "l1\nl2\nl3\nl4\nl5\n";   /* 5 lines, 15 bytes */
    long slen = (long)h_strlen(sample);

    int ok = 1;

    /* head -n 2 -> "l1\nl2\n" = 6 bytes. */
    out("HEAD SELFTEST: head -n 2 ->\n");
    long w2 = head_core(sample, slen, 2);
    if (w2 != 6) { out("  FAIL: -n 2 byte count != 6\n"); ok = 0; }

    /* head -n 0 -> nothing (0 bytes). */
    long w0 = head_core(sample, slen, 0);
    if (w0 != 0) { out("  FAIL: -n 0 byte count != 0\n"); ok = 0; }

    /* head -n 10 over a 5-line sample -> whole thing (15 bytes). */
    long wall = head_core(sample, slen, 10);
    if (wall != slen) { out("  FAIL: -n 10 byte count != 15\n"); ok = 0; }

    /* sample whose last line lacks a newline: "a\nb" head -n 5 -> 3 bytes. */
    const char *nonl = "a\nb";
    long wn = head_core(nonl, (long)h_strlen(nonl), 5);
    if (wn != 3) { out("  FAIL: no-final-newline byte count != 3\n"); ok = 0; }

    if (ok) { out("HEAD SELFTEST: PASS\n"); return 0; }
    out("HEAD SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1) return head_run(argc, argv);
    (void)selftest();
    return 0;
}

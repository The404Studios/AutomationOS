/*
 * grep.c -- minimal freestanding `grep` for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled string helpers.
 * Single self-contained file. All output to fd 1 (SYS_WRITE).
 *
 * Prints the lines of a FILE that contain a literal substring PATTERN.
 *
 * Usage:
 *   grep [-i] [-n] [-v] [-c] PATTERN FILE
 *       -i  case-insensitive match
 *       -n  prefix each printed line with its 1-based line number
 *       -v  invert: print lines that do NOT contain PATTERN
 *       -c  print only the count of matching lines
 *   grep            (argc<=1) run the built-in self-test, printing
 *                   "GREP SELFTEST: PASS" or "GREP SELFTEST: FAIL".
 *
 * Exit code: 0 if at least one matching line was found, 1 if none (or error).
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/grep/grep.c -o grep.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o grep.o -o build/grep
 *   objdump -d build/grep | grep fs:0x28   # MUST be empty
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
 *  Freestanding string helpers (our own tiny libc).
 * ======================================================================= */
static size_t g_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int g_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t g_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static char g_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* =======================================================================
 *  Output helpers (output and diagnostics both go to fd 1).
 * ======================================================================= */
static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0); }
static void out(const char *s)             { out_n(s, g_strlen(s)); }
static void out_num(unsigned long n)
{
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1, 0, 0); }
}

/* =======================================================================
 *  Static buffers (the kernel hands each app a tiny stack; large data is
 *  always static). Input is read in full up to FILE_MAX.
 * ======================================================================= */
#define FILE_MAX (256 * 1024)
static char g_in[FILE_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

/*
 * Read the whole file at `path` into g_in (up to FILE_MAX). Returns the byte
 * count (>=0), -1 if it could not be opened. Bytes beyond FILE_MAX are dropped.
 * `path` is copied through g_path first so copy_from_user has KPATH_MAX
 * readable bytes behind the pointer.
 */
static long slurp(const char *path)
{
    g_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_MAX - total;
        if (room <= 0) {                 /* buffer full: drain & stop      */
            char extra[512];
            long n = sc(SYS_READ, fd, (long)extra, 512, 0, 0);
            if (n <= 0) break;           /* nothing more to read           */
            /* else: file is larger than FILE_MAX; we keep what we have    */
            break;
        }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

/* =======================================================================
 *  Matching.
 *
 *  Does hay[0..hlen) contain `needle` (NUL-terminated)? `ci` selects
 *  case-insensitive matching. An empty needle matches every line.
 * ======================================================================= */
static int line_has(const char *hay, long hlen, const char *needle, int ci)
{
    long nlen = (long)g_strlen(needle);
    if (nlen == 0) return 1;
    for (long i = 0; i + nlen <= hlen; i++) {
        long j = 0;
        while (j < nlen) {
            char a = hay[i + j], b = needle[j];
            if (ci) { a = g_lower(a); b = g_lower(b); }
            if (a != b) break;
            j++;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* =======================================================================
 *  Core: scan g_in[0..len) line by line, emit / count matches.
 *
 *  opt_i/opt_n/opt_v/opt_c are the flags. When count_only is set we only
 *  return the count (no per-line output). Otherwise matching lines are
 *  printed (with an optional "N:" prefix). Returns the number of matches.
 * ======================================================================= */
static unsigned long grep_core(const char *buf, long len, const char *pat,
                               int opt_i, int opt_n, int opt_v, int opt_c)
{
    unsigned long matches = 0;
    long start = 0;
    unsigned long lineno = 0;
    while (start < len) {
        long e = start;
        while (e < len && buf[e] != '\n') e++;
        int had_nl = (e < len);
        long llen = e - start;
        const char *line = buf + start;
        lineno++;

        int hit = line_has(line, llen, pat, opt_i);
        if (opt_v) hit = !hit;

        if (hit) {
            matches++;
            if (!opt_c) {
                if (opt_n) { out_num(lineno); out(":"); }
                out_n(line, llen);
                out("\n");   /* always newline-terminate printed lines */
            }
        }

        if (!had_nl) break;
        start = e + 1;
    }
    if (opt_c) { out_num(matches); out("\n"); }
    return matches;
}

/* =======================================================================
 *  grep_run -- argv-driven entry. Parses leading flags, then PATTERN FILE.
 * ======================================================================= */
static int grep_run(int argc, char **argv)
{
    int opt_i = 0, opt_n = 0, opt_v = 0, opt_c = 0;
    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        const char *f = argv[ai];
        /* allow clustered flags like -in */
        for (int k = 1; f[k]; k++) {
            if (f[k] == 'i') opt_i = 1;
            else if (f[k] == 'n') opt_n = 1;
            else if (f[k] == 'v') opt_v = 1;
            else if (f[k] == 'c') opt_c = 1;
            else { out("grep: unknown option: "); out(f); out("\n"); return 1; }
        }
        ai++;
    }

    if (ai >= argc || !argv[ai]) {
        out("usage: grep [-i] [-n] [-v] [-c] PATTERN FILE\n");
        return 1;
    }
    const char *pat = argv[ai++];

    if (ai >= argc || !argv[ai]) { out("grep: no input file\n"); return 1; }
    const char *infile = argv[ai++];

    long n = slurp(infile);
    if (n < 0) { out("grep: cannot open '"); out(infile); out("'\n"); return 1; }

    unsigned long m = grep_core(g_in, n, pat, opt_i, opt_n, opt_v, opt_c);
    return m > 0 ? 0 : 1;
}

/* =======================================================================
 *  SELF-TEST (no filesystem access; pure in-memory).
 *
 *  Sample "alpha\nbeta\ngamma\n". Pattern "a" should match lines
 *  1 (alpha) and 3 (gamma) -- 2 matches. We verify grep_core's count for
 *  the basic, -i, -v, and -c paths against known expectations.
 * ======================================================================= */
static int selftest(void)
{
    out("GREP SELFTEST: begin\n");

    const char *sample = "alpha\nbeta\ngamma\n";
    long slen = (long)g_strlen(sample);

    int ok = 1;

    /* "a" matches alpha, betA, and gamma -> all 3 lines contain an 'a'. */
    unsigned long m1 = grep_core(sample, slen, "a", 0, 0, 0, 1);  /* count */
    if (m1 != 3) { out("  FAIL: 'a' count != 3\n"); ok = 0; }

    /* -i with "A" matches the same three lines. */
    unsigned long m2 = grep_core(sample, slen, "A", 1, 0, 0, 1);
    if (m2 != 3) { out("  FAIL: -i 'A' count != 3\n"); ok = 0; }

    /* invert "a": lines WITHOUT 'a' -> none (all three contain 'a') -> 0. */
    unsigned long m3 = grep_core(sample, slen, "a", 0, 0, 1, 1);
    if (m3 != 0) { out("  FAIL: -v 'a' count != 0\n"); ok = 0; }

    /* pattern present nowhere -> 0 matches. */
    unsigned long m4 = grep_core(sample, slen, "zzz", 0, 0, 0, 1);
    if (m4 != 0) { out("  FAIL: 'zzz' count != 0\n"); ok = 0; }

    /* whole-substring "amma" -> only gamma -> 1. */
    unsigned long m5 = grep_core(sample, slen, "amma", 0, 0, 0, 1);
    if (m5 != 1) { out("  FAIL: 'amma' count != 1\n"); ok = 0; }

    /* Show a -n match run for visual confirmation. */
    out("GREP SELFTEST: grep -n 'a' ->\n");
    grep_core(sample, slen, "a", 0, 1, 0, 0);

    if (ok) { out("GREP SELFTEST: PASS\n"); return 0; }
    out("GREP SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1) return grep_run(argc, argv);
    (void)selftest();
    return 0;
}

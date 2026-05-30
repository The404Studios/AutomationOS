/*
 * hexdump.c -- freestanding `hexdump -C` for the from-scratch x86_64 OS.
 * =====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard
 * headers. Pure inline syscalls + fixed static buffers + hand-rolled
 * helpers. All output goes to fd 1.
 *
 * Produces canonical `hexdump -C`-style output: an 8-hex-digit byte
 * offset, sixteen bytes shown as hex (split into two groups of eight),
 * then an ASCII gutter (`|...|`) where printable bytes are shown literally
 * and everything else as '.'. A final line prints the total length.
 *
 *   00000000  00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  |................|
 *
 * Usage:
 *   hexdump FILE            dump FILE (canonical -C format).
 *   hexdump -C FILE         same (the -C flag is the documented default).
 *   hexdump -n LEN FILE     dump at most LEN bytes of FILE.
 *   hexdump                 (argc <= 1) run the built-in self-test, printing
 *                           "HEXDUMP SELFTEST: PASS" or
 *                           "HEXDUMP SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/hexdump/hexdump.c -o hexdump.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o hexdump.o -o build/hexdump
 *   objdump -d build/hexdump | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers -- verified against kernel/include/syscall.h.
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_YIELD  15

/* open() flags. */
#define O_RDONLY  0x0000

/* The kernel copies up to MAX_PATH_LEN bytes from a path pointer; keep all
 * paths in a statically sized buffer so there are always that many readable
 * bytes behind the pointer. */
#define KPATH_MAX 4096

typedef unsigned long size_t;

/* -----------------------------------------------------------------------
 * Inline 6-arg syscall wrapper (rdi, rsi, rdx, r10, r8). We only ever use
 * up to three real args, but keep the full register-correct form.
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
 *  Freestanding helpers (our own tiny libc).
 * ======================================================================= */

static size_t h_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static size_t h_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static int h_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Parse a non-negative decimal. Returns -1 on any non-digit / empty. */
static long h_atol(const char *s)
{
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > (1L << 40)) return v;   /* bound: silly-large values clamp */
    }
    return v;
}

/* -----------------------------------------------------------------------
 * Output: everything (data and diagnostics) goes to fd 1.
 * --------------------------------------------------------------------- */
static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0); }
static void out(const char *s)             { out_n(s, h_strlen(s)); }

/* =======================================================================
 *  Buffers (static -- off the tiny user stack).
 * ======================================================================= */
#define FILEBUF_MAX  (256 * 1024)               /* 256 KB input cap        */
static unsigned char g_buf[FILEBUF_MAX] __attribute__((aligned(16)));
static char          g_path[KPATH_MAX]  __attribute__((aligned(16)));

/* Each canonical line is at most:
 *   8 (offset) + 2 + 16*3 + 1 (mid gap) + 2 (bars) + 16 (ascii) + 1 (nl)
 * which is comfortably under 96. Use a fixed line buffer. */
#define LINE_MAX 96
static char g_line[LINE_MAX] __attribute__((aligned(16)));

static const char HEX[16] = {
    '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

/* Is byte c a printable ASCII char (0x20..0x7e)? */
static int is_print(unsigned char c) { return c >= 0x20 && c <= 0x7e; }

/* =======================================================================
 *  Core formatting helper.
 *
 *  hd_line() formats ONE canonical `hexdump -C` line for the up-to-16
 *  bytes in data[0..n) belonging to file offset `off`, writing a
 *  NUL-terminated, newline-terminated string into `dst` (cap >= LINE_MAX).
 *  Returns the string length (excluding the NUL).
 *
 *  This is the formatting helper the self-test verifies byte-for-byte.
 * ======================================================================= */
static size_t hd_line(char *dst, unsigned long off, const unsigned char *data, int n)
{
    size_t k = 0;

    /* 8-digit hex offset */
    for (int sh = 28; sh >= 0; sh -= 4)
        dst[k++] = HEX[(off >> sh) & 0xf];
    dst[k++] = ' ';
    dst[k++] = ' ';

    /* 16 bytes as hex, grouped 8 + 8 (extra space before the 9th byte). */
    for (int i = 0; i < 16; i++) {
        if (i == 8) dst[k++] = ' ';          /* gap between the two groups */
        if (i < n) {
            dst[k++] = HEX[(data[i] >> 4) & 0xf];
            dst[k++] = HEX[data[i] & 0xf];
        } else {
            dst[k++] = ' ';                  /* pad missing bytes with "   " */
            dst[k++] = ' ';
        }
        dst[k++] = ' ';
    }

    /* ASCII gutter */
    dst[k++] = ' ';
    dst[k++] = '|';
    for (int i = 0; i < n; i++)
        dst[k++] = is_print(data[i]) ? (char)data[i] : '.';
    dst[k++] = '|';
    dst[k++] = '\n';
    dst[k]   = '\0';
    return k;
}

/* Format the final "offset" footer line (canonical hexdump prints the total
 * byte count on its own line). Returns length. */
static size_t hd_footer(char *dst, unsigned long total)
{
    size_t k = 0;
    for (int sh = 28; sh >= 0; sh -= 4)
        dst[k++] = HEX[(total >> sh) & 0xf];
    dst[k++] = '\n';
    dst[k]   = '\0';
    return k;
}

/* Dump len bytes of g_buf to fd 1 in canonical format. */
static void hd_dump(unsigned long len)
{
    unsigned long off = 0;
    while (off < len) {
        int n = (int)(len - off);
        if (n > 16) n = 16;
        size_t ln = hd_line(g_line, off, g_buf + off, n);
        out_n(g_line, ln);
        off += (unsigned long)n;
    }
    size_t fn = hd_footer(g_line, len);
    out_n(g_line, fn);
}

/* =======================================================================
 *  File read.
 *
 *  Slurp up to `cap` bytes of `path` into g_buf. Returns bytes read (>=0)
 *  or -1 if the file could not be opened. Stops at `cap` (bounded).
 * ======================================================================= */
static long slurp(const char *path, unsigned long cap)
{
    h_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;

    unsigned long total = 0;
    int guard = 0;
    while (total < cap) {
        long room = (long)(cap - total);
        long r = sc(SYS_READ, fd, (long)(g_buf + total), room, 0, 0);
        if (r < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0); return -1; }
        if (r == 0) break;                       /* EOF / short read => done */
        total += (unsigned long)r;
        if (++guard > 1000000) break;            /* runaway guard (bounded)  */
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return (long)total;
}

/* =======================================================================
 *  hexdump_run -- argv-driven entry.
 *
 *    hexdump [-C] [-n LEN] FILE
 * ======================================================================= */
static int hexdump_run(int argc, char **argv)
{
    long limit = -1;            /* -n LEN: -1 means "no limit"            */
    const char *file = 0;

    int ai = 1;
    while (ai < argc && argv[ai]) {
        const char *a = argv[ai];
        if (h_streq(a, "-C")) {
            ai++;                                /* canonical (the default) */
        } else if (h_streq(a, "-n")) {
            if (ai + 1 >= argc || !argv[ai + 1]) {
                out("hexdump: -n requires an argument\n");
                return 1;
            }
            limit = h_atol(argv[ai + 1]);
            if (limit < 0) { out("hexdump: bad length for -n\n"); return 1; }
            ai += 2;
        } else if (a[0] == '-' && a[1]) {
            out("hexdump: unknown option: "); out(a); out("\n");
            return 1;
        } else {
            file = a;
            ai++;
            break;                               /* first non-flag = FILE   */
        }
    }

    if (!file) { out("usage: hexdump [-C] [-n LEN] FILE\n"); return 1; }

    unsigned long cap = FILEBUF_MAX;
    if (limit >= 0 && (unsigned long)limit < cap) cap = (unsigned long)limit;

    long n = slurp(file, cap);
    if (n < 0) { out("hexdump: cannot open '"); out(file); out("'\n"); return 1; }

    hd_dump((unsigned long)n);
    return 0;
}

/* =======================================================================
 *  SELF-TEST
 *
 *  Verifies hd_line() byte-for-byte on the canonical 16-byte input
 *  0x00..0x0f, which must format to exactly:
 *
 *  "00000000  00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  |........|...|\n"
 *
 *  (the ASCII gutter for those control bytes is all '.'). It also checks a
 *  short, partial-line case to confirm padding + the printable gutter.
 *  Prints "HEXDUMP SELFTEST: PASS" / "HEXDUMP SELFTEST: FAIL".
 * ======================================================================= */
static int bytes_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int selftest(void)
{
    out("HEXDUMP: selftest begin\n");

    int ok = 1;

    /* Case 1: full 16-byte line 0x00..0x0f. */
    unsigned char in1[16];
    for (int i = 0; i < 16; i++) in1[i] = (unsigned char)i;

    const char *want1 =
        "00000000  "
        "00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  "
        "|................|\n";

    hd_line(g_line, 0, in1, 16);
    out("HEXDUMP: line -> "); out(g_line);
    if (!bytes_eq(g_line, want1)) { out("HEXDUMP: case1 mismatch\n"); ok = 0; }

    /* Case 2: a short, printable, partial line ("ABC", offset 0x10) must pad
     * the missing 13 hex columns and show "ABC" in the gutter. */
    const unsigned char in2[3] = { 'A', 'B', 'C' };
    const char *want2 =
        "00000010  "
        "41 42 43                                          "
        "|ABC|\n";
    hd_line(g_line, 0x10, in2, 3);
    out("HEXDUMP: line -> "); out(g_line);
    if (!bytes_eq(g_line, want2)) { out("HEXDUMP: case2 mismatch\n"); ok = 0; }

    /* Case 3: footer for a total of 0x1f bytes. */
    const char *wantf = "0000001f\n";
    hd_footer(g_line, 0x1f);
    if (!bytes_eq(g_line, wantf)) { out("HEXDUMP: footer mismatch\n"); ok = 0; }

    if (ok) { out("HEXDUMP SELFTEST: PASS\n"); return 0; }
    out("HEXDUMP SELFTEST: FAIL\n");
    return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 (userspace/crt0.asm) reads argc/argv off the kernel-prepared stack
 *  and calls main(argc, argv), turning the return value into SYS_EXIT. With
 *  arguments we dump the named file; with none we run the self-test (which
 *  needs no input and never hangs), and main returns 0 on PASS.
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) return hexdump_run(argc, argv);
    (void)selftest();
    return 0;
}

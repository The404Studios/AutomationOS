/*
 * cmp.c -- minimal freestanding `cmp` for the from-scratch x86_64 OS.
 * ===================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers. All output to fd 1.
 *
 * Byte-for-byte comparison of two files, streamed in fixed chunks (so file
 * size is bounded only by patience, not by a buffer). Reports the FIRST
 * differing byte offset and, if files share a prefix but one is shorter,
 * reports EOF on the shorter one.
 *
 * Usage:
 *   cmp A B      compare files A and B byte-by-byte.
 *                Identical -> no output, exit 0.
 *                Differ    -> "A B differ: byte N" to fd 1, exit 1.
 *                Error     -> exit 2.
 *   cmp          (argc<=1) run the built-in self-test, printing
 *                "CMP SELFTEST: PASS" or "CMP SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/cmp/cmp.c -o /tmp/cmp.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/cmp.o -o /tmp/cmp.elf
 *   objdump -d /tmp/cmp.elf | grep 'fs:0x28'   # must produce no output
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

static unsigned long c_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void c_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)c_strlen(s)); }
static void out_num(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1); }
}

/* ======================================================================
 * Streaming chunk buffers (static; kept off the small user stack).
 * ==================================================================== */
#define CHUNK 4096
static char g_ba[CHUNK] __attribute__((aligned(16)));
static char g_bb[CHUNK] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

/* Read up to n bytes, looping over short reads. Returns bytes read (>=0). */
static long read_chunk(long fd, char *buf, long n) {
    long off = 0;
    while (off < n) {
        long r = sc(SYS_READ, fd, (long)(buf + off), (long)(n - off));
        if (r <= 0) break;
        off += r;
    }
    return off;
}

/*
 * cmp_core -- compare the two open fds. On the first differing byte sets
 * *diff_off to its 0-based offset and returns 1. If the files are equal up
 * to the shorter length but lengths differ, sets *eof_short to 1 (A) or 2 (B)
 * and returns 1. Returns 0 if fully identical. On read error returns -1.
 */
static int cmp_core(long fa, long fb, unsigned long *diff_off, int *eof_short) {
    unsigned long base = 0;
    *diff_off = 0;
    *eof_short = 0;
    for (;;) {
        long ra = read_chunk(fa, g_ba, CHUNK);
        long rb = read_chunk(fb, g_bb, CHUNK);

        long common = (ra < rb) ? ra : rb;
        for (long i = 0; i < common; i++) {
            if (g_ba[i] != g_bb[i]) { *diff_off = base + (unsigned long)i; return 1; }
        }
        if (ra != rb) {
            /* shared prefix equal, one file is shorter */
            *diff_off = base + (unsigned long)common;
            *eof_short = (ra < rb) ? 1 : 2;
            return 1;
        }
        if (ra == 0) return 0;          /* both hit EOF together -> identical */
        base += (unsigned long)ra;
    }
}

/* ======================================================================
 * cmp_run -- argv-driven entry.  cmp A B
 * Returns 0 identical, 1 differ, 2 on error.
 * ==================================================================== */
static int cmp_run(int argc, char **argv) {
    if (argc < 3 || !argv[1] || !argv[2]) {
        out("usage: cmp A B\n");
        return 2;
    }
    c_strlcpy(g_path, argv[1], KPATH_MAX);
    long fa = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fa < 0) { out("cmp: cannot open '"); out(argv[1]); out("'\n"); return 2; }
    c_strlcpy(g_path, argv[2], KPATH_MAX);
    long fb = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fb < 0) { sc(SYS_CLOSE, fa, 0, 0); out("cmp: cannot open '"); out(argv[2]); out("'\n"); return 2; }

    unsigned long off; int eof;
    int r = cmp_core(fa, fb, &off, &eof);
    sc(SYS_CLOSE, fa, 0, 0);
    sc(SYS_CLOSE, fb, 0, 0);

    if (r < 0) { out("cmp: read error\n"); return 2; }
    if (r == 0) return 0;   /* identical: print nothing */

    out(argv[1]); out(" "); out(argv[2]); out(" differ: byte "); out_num(off);
    if (eof == 1)      { out(" (EOF on "); out(argv[1]); out(")"); }
    else if (eof == 2) { out(" (EOF on "); out(argv[2]); out(")"); }
    out("\n");
    return 1;
}

/* ======================================================================
 * SELF-TEST
 *
 * Writes two temp files differing at byte 3, verifies cmp_core reports
 * offset 3, then compares a file to itself and verifies "identical".
 * Prints CMP SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TA "/tmp/cmp_a.bin"
#define TB "/tmp/cmp_b.bin"

static int write_file(const char *path, const char *content, unsigned long len) {
    c_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(content + off), (long)(len - off));
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += (unsigned long)w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

static void selftest(void) {
    out("CMP: selftest begin\n");

    /* differ at index 3: "abcX..." vs "abcY..." */
    const char *ca = "abcXdef";
    const char *cb = "abcYdef";
    if (write_file(TA, ca, 7) != 0 || write_file(TB, cb, 7) != 0) {
        out("CMP SELFTEST: FAIL (could not write temp files)\n");
        return;
    }

    int ok = 1;

    /* 1) differing files -> offset 3 */
    {
        c_strlcpy(g_path, TA, KPATH_MAX);
        long fa = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
        c_strlcpy(g_path, TB, KPATH_MAX);
        long fb = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
        unsigned long off = 99; int eof = 0;
        int r = cmp_core(fa, fb, &off, &eof);
        sc(SYS_CLOSE, fa, 0, 0); sc(SYS_CLOSE, fb, 0, 0);
        if (!(r == 1 && off == 3 && eof == 0)) ok = 0;
    }

    /* 2) identical (A vs A) -> no diff */
    {
        c_strlcpy(g_path, TA, KPATH_MAX);
        long fa = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
        c_strlcpy(g_path, TA, KPATH_MAX);
        long fb = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
        unsigned long off = 99; int eof = 0;
        int r = cmp_core(fa, fb, &off, &eof);
        sc(SYS_CLOSE, fa, 0, 0); sc(SYS_CLOSE, fb, 0, 0);
        if (r != 0) ok = 0;
    }

    /* clean up */
    c_strlcpy(g_path, TA, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);
    c_strlcpy(g_path, TB, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (ok) out("CMP SELFTEST: PASS\n");
    else    out("CMP SELFTEST: FAIL\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return cmp_run(argc, argv);
}

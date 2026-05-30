/*
 * wcx.c -- minimal freestanding `wc` for the from-scratch x86_64 OS.
 * ==================================================================
 *
 * Named `wcx` (not `wc`) to avoid colliding with the shell's built-in `wc`.
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers. All output to fd 1.
 *
 * Counts lines, words, and bytes of a file, streamed in fixed chunks (any
 * file size). A "line" is a '\n'. A "word" is a maximal run of non-whitespace
 * (space, tab, newline, CR, form-feed, vertical-tab) -- matching POSIX wc.
 *
 * Usage:
 *   wcx FILE     print "<lines> <words> <bytes> FILE" to fd 1. exit 0 / 1.
 *   wcx          (argc<=1) run the built-in self-test, printing
 *                "WCX SELFTEST: PASS" or "WCX SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/wcx/wcx.c -o /tmp/wcx.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/wcx.o -o /tmp/wcx.elf
 *   objdump -d /tmp/wcx.elf | grep 'fs:0x28'   # must produce no output
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

static unsigned long w_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void w_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)w_strlen(s)); }
static void out_num(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1); }
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

/* ======================================================================
 * Streaming chunk buffer (static; off the small user stack).
 * ==================================================================== */
#define CHUNK 4096
static char g_buf[CHUNK] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

/*
 * wcx_core -- count lines/words/bytes from an open fd. Returns 0 / -1.
 * `in_word` state is carried across chunk boundaries so a word split by the
 * chunk edge is still counted exactly once.
 */
static int wcx_core(long fd, unsigned long *lines, unsigned long *words,
                    unsigned long *bytes) {
    unsigned long L = 0, W = 0, B = 0;
    int in_word = 0;
    for (;;) {
        long r = sc(SYS_READ, fd, (long)g_buf, CHUNK);
        if (r < 0) return -1;
        if (r == 0) break;
        for (long i = 0; i < r; i++) {
            char c = g_buf[i];
            B++;
            if (c == '\n') L++;
            if (is_space(c)) in_word = 0;
            else if (!in_word) { in_word = 1; W++; }
        }
    }
    *lines = L; *words = W; *bytes = B;
    return 0;
}

/* ======================================================================
 * wcx_run -- argv-driven entry.  wcx FILE
 * ==================================================================== */
static int wcx_run(int argc, char **argv) {
    if (argc < 2 || !argv[1]) {
        out("usage: wcx FILE\n");
        return 1;
    }
    w_strlcpy(g_path, argv[1], KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) { out("wcx: cannot open '"); out(argv[1]); out("'\n"); return 1; }

    unsigned long L, W, B;
    int rc = wcx_core(fd, &L, &W, &B);
    sc(SYS_CLOSE, fd, 0, 0);
    if (rc != 0) { out("wcx: read error\n"); return 1; }

    out_num(L); out(" "); out_num(W); out(" "); out_num(B);
    out(" "); out(argv[1]); out("\n");
    return 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * Writes a known file ("one two three\nfour\n" = 2 lines, 4 words, 19 bytes)
 * and verifies wcx_core returns those exact counts.
 * Prints WCX SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TF "/tmp/wcx_test.txt"

static int write_file(const char *path, const char *content) {
    w_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long len = w_strlen(content), off = 0;
    while (off < len) {
        long wn = sc(SYS_WRITE, fd, (long)(content + off), (long)(len - off));
        if (wn <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += (unsigned long)wn;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

static void selftest(void) {
    out("WCX: selftest begin\n");

    const char *content = "one two three\nfour\n";   /* 2 \n, 4 words, 19 bytes */
    if (write_file(TF, content) != 0) {
        out("WCX SELFTEST: FAIL (could not write temp file)\n");
        return;
    }

    w_strlcpy(g_path, TF, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) { out("WCX SELFTEST: FAIL (could not open temp file)\n"); return; }

    unsigned long L = 0, W = 0, B = 0;
    int rc = wcx_core(fd, &L, &W, &B);
    sc(SYS_CLOSE, fd, 0, 0);

    out("WCX: counts -> "); out_num(L); out(" "); out_num(W); out(" "); out_num(B); out("\n");

    int ok = (rc == 0) && (L == 2) && (W == 4) && (B == 19);

    w_strlcpy(g_path, TF, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (ok) out("WCX SELFTEST: PASS\n");
    else    out("WCX SELFTEST: FAIL\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return wcx_run(argc, argv);
}

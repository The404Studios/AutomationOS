/*
 * tee.c -- minimal freestanding `tee` for the from-scratch x86_64 OS.
 * ===================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers.
 *
 * This OS has no pipes/stdin, so the classic `tee FILE` (copy stdin to both
 * FILE and stdout) is adapted to a file-argument form: a SOURCE file is the
 * stand-in for stdin.
 *
 * Usage:
 *   tee SRC DST  copy SRC -> DST and ALSO echo SRC's bytes to fd 1, exactly
 *                like `tee` splits stdin to a file and stdout. Streamed in
 *                fixed chunks (any file size). exit 0 on success, 1 on error.
 *   tee          (argc<=1) run the built-in self-test, printing
 *                "TEE SELFTEST: PASS" or "TEE SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tee/tee.c -o /tmp/tee.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/tee.o -o /tmp/tee.elf
 *   objdump -d /tmp/tee.elf | grep 'fs:0x28'   # must produce no output
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

static unsigned long t_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void t_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int t_memeq(const char *a, const char *b, long n) {
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)t_strlen(s)); }

/* ======================================================================
 * Streaming chunk buffer (static; off the small user stack).
 * ==================================================================== */
#define CHUNK 4096
static char g_buf[CHUNK] __attribute__((aligned(16)));
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

/* Write exactly n bytes to fd, looping over short writes. Returns 0 / -1. */
static int write_all(long fd, const char *buf, long n) {
    long off = 0;
    while (off < n) {
        long w = sc(SYS_WRITE, fd, (long)(buf + off), (long)(n - off));
        if (w <= 0) return -1;
        off += w;
    }
    return 0;
}

/*
 * tee_core -- copy from open fd `src` to open fd `dst` (a file) and also to
 * fd 1 (stdout), chunk by chunk. Returns 0 on success, -1 on error.
 */
static int tee_core(long src, long dst) {
    for (;;) {
        long r = sc(SYS_READ, src, (long)g_buf, CHUNK);
        if (r < 0) return -1;
        if (r == 0) break;
        if (write_all(dst, g_buf, r) != 0) return -1;   /* to the file  */
        if (write_all(1,   g_buf, r) != 0) return -1;   /* and to fd 1  */
    }
    return 0;
}

/* ======================================================================
 * tee_run -- argv-driven entry.  tee SRC DST
 * ==================================================================== */
static int tee_run(int argc, char **argv) {
    if (argc < 3 || !argv[1] || !argv[2]) {
        out("usage: tee SRC DST\n");
        return 1;
    }
    t_strlcpy(g_path, argv[1], KPATH_MAX);
    long src = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (src < 0) { out("tee: cannot open '"); out(argv[1]); out("'\n"); return 1; }

    t_strlcpy(g_path, argv[2], KPATH_MAX);
    long dst = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst < 0) {
        sc(SYS_CLOSE, src, 0, 0);
        out("tee: cannot create '"); out(argv[2]); out("'\n");
        return 1;
    }

    int rc = tee_core(src, dst);
    sc(SYS_CLOSE, src, 0, 0);
    sc(SYS_CLOSE, dst, 0, 0);
    if (rc != 0) { out("tee: I/O error\n"); return 1; }
    return 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * Writes a known SRC, runs tee SRC->DST (echo to fd1 is visible), then reads
 * DST back and verifies it equals SRC byte-for-byte.
 * Prints TEE SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TSRC "/tmp/tee_src.txt"
#define TDST "/tmp/tee_dst.txt"

static int write_file(const char *path, const char *content) {
    t_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long len = t_strlen(content);
    int rc = write_all(fd, content, (long)len);
    sc(SYS_CLOSE, fd, 0, 0);
    return rc;
}

/* Read whole file into buf[cap]; returns bytes read or -1. */
static long read_file(const char *path, char *buf, int cap) {
    t_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap) {
        long r = sc(SYS_READ, fd, (long)(buf + total), (long)(cap - total));
        if (r <= 0) break;
        total += r;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

static char g_verify[CHUNK];

static void selftest(void) {
    out("TEE: selftest begin\n");

    const char *content = "tee line one\ntee line two\n";
    if (write_file(TSRC, content) != 0) {
        out("TEE SELFTEST: FAIL (could not write source)\n");
        return;
    }

    /* run the real tee_run path: SRC -> DST (also echoes SRC to fd 1). */
    char a0[] = "tee";
    char a1[KPATH_MAX]; t_strlcpy(a1, TSRC, sizeof(a1));
    char a2[KPATH_MAX]; t_strlcpy(a2, TDST, sizeof(a2));
    char *argv[] = { a0, a1, a2, 0 };
    out("TEE: echo of SRC ->\n");
    int rc = tee_run(3, argv);

    /* read DST back and compare to the original content */
    long n = read_file(TDST, g_verify, (int)sizeof(g_verify));
    unsigned long el = t_strlen(content);
    int ok = (rc == 0) && (n == (long)el) && t_memeq(g_verify, content, (long)el);

    /* clean up */
    t_strlcpy(g_path, TSRC, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);
    t_strlcpy(g_path, TDST, KPATH_MAX); sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (ok) out("TEE SELFTEST: PASS\n");
    else    out("TEE SELFTEST: FAIL\n");
}

int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return tee_run(argc, argv);
}

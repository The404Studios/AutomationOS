/*
 * basename.c -- minimal freestanding `basename` for the x86_64 OS.
 * ===============================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure string logic; the ONLY
 * syscall used is SYS_WRITE (to fd 1).
 *
 * Strips the leading directory components of PATH (everything up to and
 * including the final '/'), then optionally strips a trailing SUFFIX, and
 * prints the result followed by a newline -- matching POSIX basename:
 *   basename /a/b/c.txt        -> c.txt
 *   basename /a/b/c.txt .txt   -> c
 *   basename file              -> file
 *   basename /                 -> /
 *   basename ""                -> .         (empty path)
 *   basename /usr/             -> usr       (trailing slashes ignored)
 *
 * Usage:
 *   basename             (argc<=1) run the built-in self-test, printing
 *                        "BASENAME SELFTEST: PASS" / "FAIL".
 *   basename PATH [SUFFIX]
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/basename/basename.c -o /tmp/basename.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/basename.o -o /tmp/basename.elf
 *   objdump -d /tmp/basename.elf | grep 'fs:0x28'   # must produce no output
 */

#define SYS_WRITE  3

/* ======================================================================
 * Inline syscall (6-arg wrapper; only SYS_WRITE is ever issued).
 * ==================================================================== */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ======================================================================
 * Freestanding string helpers.
 * ==================================================================== */
static unsigned long f_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static int f_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)f_strlen(s), 0, 0); }
static void out_n(const char *s, unsigned long len) { sc(SYS_WRITE, 1, (long)s, (long)len, 0, 0); }

#define PATH_CAP 1024

/* ======================================================================
 * Core: compute basename(path [, suffix]) into `dst` (NUL-terminated).
 *
 * POSIX rules: trailing '/' characters are stripped first; a path that is all
 * slashes reduces to "/"; an empty path yields ".". A non-empty SUFFIX is
 * removed only if it is a proper trailing substring (and not the whole name).
 * `cap` bounds the output buffer.
 * ==================================================================== */
static void basename_into(const char *path, const char *suffix, char *dst, int cap) {
    if (cap <= 0) return;

    unsigned long len = f_strlen(path);

    /* Empty path -> "." */
    if (len == 0) { dst[0] = '.'; dst[1] = '\0'; return; }

    /* Strip trailing slashes (but keep one if the whole string is slashes). */
    unsigned long end = len;
    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { dst[0] = '/'; dst[1] = '\0'; return; }   /* all slashes */

    /* Find start of the final component (just after the last '/' before end). */
    unsigned long start = end;
    while (start > 0 && path[start - 1] != '/') start--;

    /* Copy [start, end) into dst (bounded). */
    int n = 0;
    for (unsigned long i = start; i < end && n < cap - 1; i++) dst[n++] = path[i];
    dst[n] = '\0';

    /* Optional suffix removal: strip only if it's a proper trailing match. */
    if (suffix && suffix[0] != '\0') {
        unsigned long sl = f_strlen(suffix);
        if ((int)sl < n) {   /* must not consume the entire name */
            int match = 1;
            for (unsigned long i = 0; i < sl; i++)
                if (dst[n - (int)sl + (int)i] != suffix[i]) { match = 0; break; }
            if (match) { n -= (int)sl; dst[n] = '\0'; }
        }
    }
}

/* ======================================================================
 * basename_run -- argv-driven entry.  basename PATH [SUFFIX]
 * ==================================================================== */
static int basename_run(int argc, char **argv) {
    if (argc < 2 || !argv[1]) {
        out("usage: basename PATH [SUFFIX]\n");
        return 1;
    }
    const char *suffix = (argc >= 3 && argv[2]) ? argv[2] : "";

    char dst[PATH_CAP];
    basename_into(argv[1], suffix, dst, PATH_CAP);
    out(dst); out("\n");
    return 0;
}

/* ======================================================================
 * SELF-TEST -- pure string cases (no FS).
 * Prints BASENAME SELFTEST: PASS / FAIL.
 * ==================================================================== */
static int check(const char *path, const char *suffix, const char *want) {
    char dst[PATH_CAP];
    basename_into(path, suffix, dst, PATH_CAP);
    return f_streq(dst, want);
}

static void selftest(void) {
    out("BASENAME: selftest begin\n");

    int ok = 1;
    ok &= check("/a/b/c.txt", "",      "c.txt");
    ok &= check("/a/b/c.txt", ".txt",  "c");
    ok &= check("file",        "",      "file");
    ok &= check("file.txt",    ".txt",  "file");
    ok &= check("/usr/lib",    "",      "lib");
    ok &= check("/usr/",       "",      "usr");      /* trailing slash */
    ok &= check("/",           "",      "/");        /* root */
    ok &= check("",            "",      ".");        /* empty */
    ok &= check("a/b/",        "",      "b");        /* trailing slash */
    ok &= check(".txt",        ".txt",  ".txt");     /* suffix == whole name: keep */

    if (ok) out("BASENAME SELFTEST: PASS\n");
    else    out("BASENAME SELFTEST: FAIL\n");
}

/* ======================================================================
 * Entry point. crt0 calls main(argc, argv).
 * ==================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return basename_run(argc, argv);
}

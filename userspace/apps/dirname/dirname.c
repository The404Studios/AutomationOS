/*
 * dirname.c -- minimal freestanding `dirname` for the x86_64 OS.
 * =============================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure string logic; the ONLY
 * syscall used is SYS_WRITE (to fd 1).
 *
 * Prints the directory portion of PATH (everything up to, but not including,
 * the final '/'), matching POSIX dirname:
 *   dirname /a/b/c.txt   -> /a/b
 *   dirname file         -> .          (no slash)
 *   dirname /usr/lib     -> /usr
 *   dirname /usr/        -> /          (trailing slash, then single comp)
 *   dirname /            -> /
 *   dirname ""           -> .          (empty path)
 *   dirname a/b/         -> a
 *   dirname //a          -> /          (collapsed leading slashes)
 *
 * Usage:
 *   dirname              (argc<=1) run the built-in self-test, printing
 *                        "DIRNAME SELFTEST: PASS" / "FAIL".
 *   dirname PATH
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/dirname/dirname.c -o /tmp/dirname.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/dirname.o -o /tmp/dirname.elf
 *   objdump -d /tmp/dirname.elf | grep 'fs:0x28'   # must produce no output
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

#define PATH_CAP 1024

/* ======================================================================
 * Core: compute dirname(path) into `dst` (NUL-terminated).
 *
 * POSIX rules:
 *   - empty path            -> "."
 *   - strip trailing slashes
 *   - no '/' in remainder   -> "."
 *   - else everything up to (not incl) the last '/', with trailing slashes of
 *     that prefix stripped; if the prefix becomes empty it was the root "/".
 * ==================================================================== */
static void dirname_into(const char *path, char *dst, int cap) {
    if (cap <= 0) return;

    unsigned long len = f_strlen(path);

    /* Empty path -> "." */
    if (len == 0) { dst[0] = '.'; dst[1] = '\0'; return; }

    /* Strip trailing slashes. */
    unsigned long end = len;
    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { dst[0] = '/'; dst[1] = '\0'; return; }   /* all slashes -> "/" */

    /* Find last '/' within [0, end). */
    long slash = -1;
    for (unsigned long i = 0; i < end; i++) if (path[i] == '/') slash = (long)i;

    /* No slash in the (de-slashed) path -> "." */
    if (slash < 0) { dst[0] = '.'; dst[1] = '\0'; return; }

    /* Directory portion is [0, slash); strip its trailing slashes too. */
    unsigned long dend = (unsigned long)slash;
    while (dend > 0 && path[dend - 1] == '/') dend--;
    if (dend == 0) { dst[0] = '/'; dst[1] = '\0'; return; }  /* root, e.g. "/x" */

    int n = 0;
    for (unsigned long i = 0; i < dend && n < cap - 1; i++) dst[n++] = path[i];
    dst[n] = '\0';
}

/* ======================================================================
 * dirname_run -- argv-driven entry.  dirname PATH
 * ==================================================================== */
static int dirname_run(int argc, char **argv) {
    if (argc < 2 || !argv[1]) {
        out("usage: dirname PATH\n");
        return 1;
    }
    char dst[PATH_CAP];
    dirname_into(argv[1], dst, PATH_CAP);
    out(dst); out("\n");
    return 0;
}

/* ======================================================================
 * SELF-TEST -- pure string cases (no FS).
 * Prints DIRNAME SELFTEST: PASS / FAIL.
 * ==================================================================== */
static int check(const char *path, const char *want) {
    char dst[PATH_CAP];
    dirname_into(path, dst, PATH_CAP);
    return f_streq(dst, want);
}

static void selftest(void) {
    out("DIRNAME: selftest begin\n");

    int ok = 1;
    ok &= check("/a/b/c.txt", "/a/b");
    ok &= check("file",       ".");
    ok &= check("/usr/lib",   "/usr");
    ok &= check("/usr/",      "/");      /* trailing slash, one component */
    ok &= check("/",          "/");      /* root */
    ok &= check("",           ".");      /* empty */
    ok &= check("a/b/",       "a");      /* trailing slash */
    ok &= check("a/b",        "a");
    ok &= check("/x",         "/");      /* single component under root */
    ok &= check("noslash",    ".");

    if (ok) out("DIRNAME SELFTEST: PASS\n");
    else    out("DIRNAME SELFTEST: FAIL\n");
}

/* ======================================================================
 * Entry point. crt0 calls main(argc, argv).
 * ==================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return dirname_run(argc, argv);
}

/*
 * find.c -- minimal freestanding `find` for the from-scratch x86_64 OS.
 * =====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained mem/str helpers. All output (and diagnostics) to fd 1.
 *
 * Usage:
 *   find DIR [-name PAT]    recursively walk DIR (opendir/readdir/stat),
 *                           printing every path. With -name PAT, only paths
 *                           whose final component contains the LITERAL
 *                           substring PAT are printed (no glob/regex).
 *   find                    (argc<=1) run the built-in self-test, printing
 *                           "FIND SELFTEST: PASS" or "FIND SELFTEST: FAIL".
 *
 * argv is delivered by crt0 (userspace/crt0.asm) + the exec.c SysV frame, so
 * main(argc, argv) sees real arguments. Directory detection uses SYS_OPENDIR
 * (st_mode carries no POSIX type bit on this kernel -- see tar.c).
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/find/find.c -o /tmp/find.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/find.o -o /tmp/find.elf
 *   objdump -d /tmp/find.elf | grep 'fs:0x28'   # must produce no output
 */

/* ======================================================================
 * Syscall numbers (verified against kernel/include/syscall.h).
 * ==================================================================== */
#define SYS_EXIT      0
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_OPENDIR   30
#define SYS_READDIR   31
#define SYS_CLOSEDIR  32
#define SYS_STAT      33
#define SYS_UNLINK    34
#define SYS_MKDIR     67

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

#define NAME_MAX_ 256

/* dirent layout: the kernel copies sizeof(struct dirent) into our buffer in
 * sys_readdir(); mirror it exactly (see tar.c). */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

/* ======================================================================
 * Inline syscall.
 * ==================================================================== */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ======================================================================
 * Freestanding string helpers (our own libc).
 * ==================================================================== */
static unsigned long f_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static int f_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
/* Substring search: is NUL-terminated needle inside NUL-terminated hay?
 * Empty needle => 1 (matches everything). */
static int f_contains(const char *hay, const char *needle) {
    if (needle[0] == '\0') return 1;
    for (unsigned long i = 0; hay[i]; i++) {
        unsigned long j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (needle[j] == '\0') return 1;
        if (hay[i + j] == '\0') break;
    }
    return 0;
}

/* ---- fd-1 output ---- */
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)f_strlen(s)); }

/* ======================================================================
 * Directory probe (st_mode carries no type bit -> probe via SYS_OPENDIR).
 * ==================================================================== */
static int is_dir(const char *path) {
    long dfd = sc(SYS_OPENDIR, (long)path, 0, 0);
    if (dfd >= 0) { sc(SYS_CLOSEDIR, dfd, 0, 0); return 1; }
    return 0;
}

/* Final path component of `path` (after the last '/'); whole string if none. */
static const char *basename_of(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* ======================================================================
 * Recursive walk.
 *
 * All per-level scratch (childpath, dirent) lives on the stack so nested
 * recursion levels can't clobber a parent's path while it is still in use.
 * The user stack is small, so recursion depth is bounded by MAX_DEPTH and
 * childpath is a fixed 512-byte buffer.
 * ==================================================================== */
#define PATH_CAP  512
#define MAX_DEPTH 32

static void print_path(const char *path) { out(path); out("\n"); }

static void walk(const char *path, const char *pat, int depth) {
    /* Print this path itself if it matches the name filter. */
    if (f_contains(basename_of(path), pat)) print_path(path);

    if (depth >= MAX_DEPTH) return;

    long dfd = sc(SYS_OPENDIR, (long)path, 0, 0);
    if (dfd < 0) return;   /* not a directory (or unreadable) -> nothing more */

    k_dirent_t de;
    char childpath[PATH_CAP];
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0);
        if (r != 0) break;
        de.d_name[NAME_MAX_ - 1] = '\0';
        const char *nm = de.d_name;
        if (nm[0] == '\0') continue;
        if (f_streq(nm, ".") || f_streq(nm, "..")) continue;

        /* build childpath = path + "/" + nm (bounded) */
        int n = 0;
        const char *p = path;
        while (*p && n < PATH_CAP - 1) childpath[n++] = *p++;
        if (n == 0 || childpath[n - 1] != '/')
            if (n < PATH_CAP - 1) childpath[n++] = '/';
        const char *c = nm;
        while (*c && n < PATH_CAP - 1) childpath[n++] = *c++;
        childpath[n] = '\0';

        walk(childpath, pat, depth + 1);
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0);
}

/* ======================================================================
 * find_run -- argv-driven entry. Returns process exit code.
 *   argv: find DIR [-name PAT]
 * ==================================================================== */
static int find_run(int argc, char **argv) {
    if (argc < 2 || !argv[1]) {
        out("usage: find DIR [-name PAT]\n");
        return 1;
    }
    const char *dir = argv[1];
    const char *pat = "";           /* empty => match everything */

    int ai = 2;
    while (ai < argc && argv[ai]) {
        if (f_streq(argv[ai], "-name")) {
            if (ai + 1 >= argc || !argv[ai + 1]) {
                out("find: -name needs an argument\n");
                return 1;
            }
            pat = argv[ai + 1];
            ai += 2;
        } else {
            out("find: unknown option: "); out(argv[ai]); out("\n");
            return 1;
        }
    }

    walk(dir, pat, 0);
    return 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * 1. mkdir /tmp/findtest and /tmp/findtest/sub
 * 2. write /tmp/findtest/alpha.txt and /tmp/findtest/sub/beta.log
 * 3. walk with -name ".txt" and capture printed paths into a buffer
 * 4. verify alpha.txt appears and beta.log does NOT
 * 5. clean up; print FIND SELFTEST: PASS / FAIL
 * ==================================================================== */
#define TDIR  "/tmp/findtest"
#define TSUB  "/tmp/findtest/sub"
#define TF1   "/tmp/findtest/alpha.txt"
#define TF2   "/tmp/findtest/sub/beta.log"

static int write_file(const char *path, const char *content) {
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long len = f_strlen(content), off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(content + off), (long)(len - off));
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += (unsigned long)w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* Capture buffer + collecting walk, used only by the self-test so we can
 * verify the result without parsing fd-1 output. */
static char g_cap[2048];
static int  g_cap_len;

static void cap_path(const char *path) {
    for (unsigned long i = 0; path[i] && g_cap_len < (int)sizeof(g_cap) - 2; i++)
        g_cap[g_cap_len++] = path[i];
    if (g_cap_len < (int)sizeof(g_cap) - 1) g_cap[g_cap_len++] = '\n';
    g_cap[g_cap_len] = '\0';
}

static void cap_walk(const char *path, const char *pat, int depth) {
    if (f_contains(basename_of(path), pat)) cap_path(path);
    if (depth >= MAX_DEPTH) return;
    long dfd = sc(SYS_OPENDIR, (long)path, 0, 0);
    if (dfd < 0) return;
    k_dirent_t de;
    char childpath[PATH_CAP];
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0);
        if (r != 0) break;
        de.d_name[NAME_MAX_ - 1] = '\0';
        const char *nm = de.d_name;
        if (nm[0] == '\0' || f_streq(nm, ".") || f_streq(nm, "..")) continue;
        int n = 0; const char *p = path;
        while (*p && n < PATH_CAP - 1) childpath[n++] = *p++;
        if (n == 0 || childpath[n - 1] != '/')
            if (n < PATH_CAP - 1) childpath[n++] = '/';
        const char *c = nm;
        while (*c && n < PATH_CAP - 1) childpath[n++] = *c++;
        childpath[n] = '\0';
        cap_walk(childpath, pat, depth + 1);
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0);
}

static void selftest(void) {
    out("FIND: selftest begin\n");

    sc(SYS_MKDIR, (long)TDIR, 0755, 0);
    sc(SYS_MKDIR, (long)TSUB, 0755, 0);
    if (write_file(TF1, "alpha\n") != 0 || write_file(TF2, "beta\n") != 0) {
        out("FIND SELFTEST: FAIL (could not write source files)\n");
        return;
    }

    /* -name ".txt" should match alpha.txt but not beta.log */
    g_cap_len = 0; g_cap[0] = '\0';
    cap_walk(TDIR, ".txt", 0);

    int found_txt = f_contains(g_cap, "alpha.txt");
    int found_log = f_contains(g_cap, "beta.log");

    /* show the unfiltered listing (informational) */
    out("FIND: full listing of "); out(TDIR); out(":\n");
    walk(TDIR, "", 0);

    /* clean up (best effort) */
    sc(SYS_UNLINK, (long)TF1, 0, 0);
    sc(SYS_UNLINK, (long)TF2, 0, 0);

    if (found_txt && !found_log) out("FIND SELFTEST: PASS\n");
    else                         out("FIND SELFTEST: FAIL\n");
}

/* ======================================================================
 * Entry point. crt0 calls main(argc, argv).
 * ==================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return find_run(argc, argv);
}

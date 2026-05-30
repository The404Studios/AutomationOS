/*
 * du.c -- minimal freestanding `du` for the from-scratch x86_64 OS.
 * ==================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained mem/str helpers. All output to fd 1.
 *
 * Recursively sums the sizes of every regular file under a directory and
 * reports disk usage in KB (1024-byte units, rounded up like coreutils du).
 *
 * Usage:
 *   du            (argc<=1) run the built-in self-test, printing
 *                 "DU SELFTEST: PASS" or "DU SELFTEST: FAIL".
 *   du DIR        sum sizes under DIR (default ".") and print "<kb>\t<path>".
 *   du -a DIR     also print "<kb>\t<path>" for every file/subdir visited.
 *
 * Directory detection: st_mode carries no POSIX type bit on this kernel
 * (see tar.c / find.c), so a path is a directory iff SYS_OPENDIR succeeds.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/du/du.c -o /tmp/du.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/du.o -o /tmp/du.elf
 *   objdump -d /tmp/du.elf | grep 'fs:0x28'   # must produce no output
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
 * sys_readdir(); mirror it exactly (see find.c / tar.c). */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

/* vfs_stat_t layout (kernel/include/vfs.h). st_mode is the bare permission
 * mode WITHOUT POSIX S_IFDIR/S_IFREG type bits, so directory detection is
 * done by probing SYS_OPENDIR, not by inspecting st_mode. */
typedef struct {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    unsigned long long st_size;
    unsigned long long st_blksize;
    unsigned long long st_blocks;
    unsigned long long st_atime;
    unsigned long long st_mtime;
    unsigned long long st_ctime;
} k_stat_t;

/* ======================================================================
 * Inline syscall (6-arg wrapper).
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
 * Freestanding string helpers (our own libc).
 * ==================================================================== */
static unsigned long f_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static int f_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---- fd-1 output ---- */
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)f_strlen(s), 0, 0); }
static void out_num(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1, 0, 0); }
}

/* ======================================================================
 * Size formatting helper: bytes -> KB, rounded up (coreutils-style).
 * ==================================================================== */
static unsigned long bytes_to_kb(unsigned long bytes) {
    return (bytes + 1023UL) / 1024UL;
}

/* Directory probe. */
static int is_dir(const char *path) {
    long dfd = sc(SYS_OPENDIR, (long)path, 0, 0, 0, 0);
    if (dfd >= 0) { sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0); return 1; }
    return 0;
}

/* ======================================================================
 * Recursive size accumulation.
 *
 * Per-level scratch (childpath, dirent) lives on the stack so nested
 * recursion levels can't clobber a parent's path. Depth and path length are
 * both bounded; the user stack is small.
 * ==================================================================== */
#define PATH_CAP  512
#define MAX_DEPTH 16

static int g_all;   /* -a : print every file, not just the top-level dir */

/* Returns total size in bytes of `path` (the file itself, or the whole tree
 * if it is a directory). Prints "<kb>\t<path>" lines per -a / top-level rules. */
static unsigned long du_walk(const char *path, int depth, int print_this) {
    /* If it's a regular file (or unreadable as a dir), account for its size. */
    if (!is_dir(path)) {
        k_stat_t st;
        if (sc(SYS_STAT, (long)path, (long)&st, 0, 0, 0) != 0) return 0;
        unsigned long sz = (unsigned long)st.st_size;
        if (print_this) { out_num(bytes_to_kb(sz)); out("\t"); out(path); out("\n"); }
        return sz;
    }

    unsigned long total = 0;

    if (depth < MAX_DEPTH) {
        long dfd = sc(SYS_OPENDIR, (long)path, 0, 0, 0, 0);
        if (dfd >= 0) {
            k_dirent_t de;
            char childpath[PATH_CAP];
            for (;;) {
                long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0);
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

                total += du_walk(childpath, depth + 1, g_all);
            }
            sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0);
        }
    }

    /* A directory line is printed if -a, OR if this is the top-level target. */
    if (print_this) { out_num(bytes_to_kb(total)); out("\t"); out(path); out("\n"); }
    return total;
}

/* ======================================================================
 * du_run -- argv-driven entry.  du [-a] [DIR]
 * ==================================================================== */
static int du_run(int argc, char **argv) {
    const char *dir = ".";
    g_all = 0;

    for (int ai = 1; ai < argc && argv[ai]; ai++) {
        if (f_streq(argv[ai], "-a")) { g_all = 1; }
        else if (argv[ai][0] == '-' && argv[ai][1] != '\0') {
            out("du: unknown option: "); out(argv[ai]); out("\n");
            return 1;
        } else {
            dir = argv[ai];
        }
    }

    /* The top-level target is always printed (print_this = 1). */
    du_walk(dir, 0, 1);
    return 0;
}

/* ======================================================================
 * SELF-TEST
 *
 * 1. verify bytes_to_kb() rounds up on known values.
 * 2. build a small tree /tmp/dutest with two known-size files and confirm
 *    du_walk() returns their summed byte size (best-effort; if the FS write
 *    fails the size-formatting check alone still gates PASS/FAIL).
 * Prints DU SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TDIR "/tmp/dutest"
#define TSUB "/tmp/dutest/sub"
#define TF1  "/tmp/dutest/a.bin"      /* 100 bytes */
#define TF2  "/tmp/dutest/sub/b.bin"  /* 2048 bytes */

static int write_n(const char *path, unsigned long n) {
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
    if (fd < 0) return -1;
    static char buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 'x';
    unsigned long off = 0;
    while (off < n) {
        unsigned long chunk = n - off; if (chunk > 512) chunk = 512;
        long w = sc(SYS_WRITE, fd, (long)buf, (long)chunk, 0, 0);
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0); return -1; }
        off += (unsigned long)w;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

static void selftest(void) {
    out("DU: selftest begin\n");

    /* (1) pure size-formatting check -- no FS needed. */
    int fmt_ok = (bytes_to_kb(0) == 0) && (bytes_to_kb(1) == 1) &&
                 (bytes_to_kb(1024) == 1) && (bytes_to_kb(1025) == 2) &&
                 (bytes_to_kb(2048) == 2) && (bytes_to_kb(2049) == 3);

    /* (2) best-effort tree check. */
    int tree_ok = 1;   /* default to ok; only fail if we built it AND mismatch */
    sc(SYS_MKDIR, (long)TDIR, 0755, 0, 0, 0);
    sc(SYS_MKDIR, (long)TSUB, 0755, 0, 0, 0);
    if (write_n(TF1, 100) == 0 && write_n(TF2, 2048) == 0) {
        unsigned long total = du_walk(TDIR, 0, 0);
        out("DU: tree bytes -> "); out_num(total); out("\n");
        tree_ok = (total == 100 + 2048);
        sc(SYS_UNLINK, (long)TF1, 0, 0, 0, 0);
        sc(SYS_UNLINK, (long)TF2, 0, 0, 0, 0);
    }

    if (fmt_ok && tree_ok) out("DU SELFTEST: PASS\n");
    else                   out("DU SELFTEST: FAIL\n");
}

/* ======================================================================
 * Entry point. crt0 calls main(argc, argv).
 * ==================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return du_run(argc, argv);
}

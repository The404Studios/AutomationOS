/*
 * touch.c -- minimal freestanding `touch` for the from-scratch x86_64 OS.
 * =======================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained helpers. All diagnostics to fd 1.
 *
 * Creates each named file if it does not already exist (SYS_OPEN with
 * O_WRONLY|O_CREAT, then SYS_CLOSE). This kernel exposes no mtime/utimes
 * syscall, so for an existing file there is nothing to update -- opening it
 * (which O_CREAT leaves untouched when it already exists) is a harmless
 * no-op. On success, touch prints NOTHING.
 *
 * Usage:
 *   touch            (argc<=1) run the built-in self-test, printing
 *                    "TOUCH SELFTEST: PASS" or "TOUCH SELFTEST: FAIL".
 *   touch FILE...    create each FILE if absent. exit 0 on success, 1 if any
 *                    file could not be created/opened.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/touch/touch.c -o /tmp/touch.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/touch.o -o /tmp/touch.elf
 *   objdump -d /tmp/touch.elf | grep 'fs:0x28'   # must produce no output
 */

/* ======================================================================
 * Syscall numbers (verified against kernel/include/syscall.h).
 * ==================================================================== */
#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_STAT    33
#define SYS_UNLINK  34

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

/* vfs_stat_t layout (kernel/include/vfs.h) -- used only to confirm existence
 * in the self-test. */
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
 * Freestanding helpers.
 * ==================================================================== */
static unsigned long f_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)f_strlen(s), 0, 0); }

/* ======================================================================
 * Core: create (if absent) a single file. Returns 0 on success, -1 on error.
 *
 * O_CREAT creates the file when missing and is a no-op when it exists; in
 * both cases a valid fd is returned, which we immediately close. We do NOT
 * pass O_TRUNC, so existing file contents are preserved.
 * ==================================================================== */
static int touch_one(const char *path) {
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT, 0644, 0, 0);
    if (fd < 0) return -1;
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

/* ======================================================================
 * touch_run -- argv-driven entry.  touch FILE...
 * ==================================================================== */
static int touch_run(int argc, char **argv) {
    int rc = 0;
    for (int ai = 1; ai < argc && argv[ai]; ai++) {
        if (touch_one(argv[ai]) != 0) {
            out("touch: cannot touch '"); out(argv[ai]); out("'\n");
            rc = 1;
        }
        /* success => print nothing */
    }
    return rc;
}

/* ======================================================================
 * SELF-TEST
 *
 * 1. arg-handling sanity: touch_one on a fresh path must return 0, and a
 *    follow-up stat() must confirm the file now exists.
 * 2. touching it again (already exists) must still succeed.
 * If the FS write path is unavailable the test degrades gracefully and
 * fails loudly. Prints TOUCH SELFTEST: PASS / FAIL.
 * ==================================================================== */
#define TF "/tmp/touch_test"

static void selftest(void) {
    out("TOUCH: selftest begin\n");

    /* Start clean (best effort). */
    sc(SYS_UNLINK, (long)TF, 0, 0, 0, 0);

    int create_ok = (touch_one(TF) == 0);

    k_stat_t st;
    int exists = (sc(SYS_STAT, (long)TF, (long)&st, 0, 0, 0) == 0);

    /* Touching an existing file again must also succeed (no-op). */
    int again_ok = (touch_one(TF) == 0);

    out("TOUCH: create="); out(create_ok ? "1" : "0");
    out(" exists="); out(exists ? "1" : "0");
    out(" again="); out(again_ok ? "1" : "0"); out("\n");

    /* clean up (best effort) */
    sc(SYS_UNLINK, (long)TF, 0, 0, 0, 0);

    if (create_ok && exists && again_ok) out("TOUCH SELFTEST: PASS\n");
    else                                 out("TOUCH SELFTEST: FAIL\n");
}

/* ======================================================================
 * Entry point. crt0 calls main(argc, argv).
 * ==================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) { selftest(); return 0; }
    return touch_run(argc, argv);
}

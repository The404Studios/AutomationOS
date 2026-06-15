// forkfdtest -- fork-fd-table inheritance proof (no libs, own _start, direct
// syscalls). Prints to fd 1 (serial). The smoke greps "FORKFDTEST RESULT: PASS".
//
// Proves fork() deep-copies the parent's REGULAR-FILE fd table into the child:
//   * the child can USE an inherited ramfs fd (write succeeds) -- this is the
//     property that FAILS on a kernel that does not inherit fds at all (the
//     child's write to the inherited number returns EBADF -> child exit 2);
//   * the child closing + exiting does NOT poison the parent's fd: the parent
//     can still write through that same fd AFTER reaping the child;
//   * the parent closing AFTER the child already closed does not double-free;
//   * 100 open/fork/use/close cycles do not corrupt inode refs (a later open or
//     the kernel itself would fault if the shared-inode refcount underflowed).
//
// Offsets are intentionally INDEPENDENT (fork deep-copies the vfs_file_t), so
// this test asserts only on syscall return values, never on shared-offset byte
// positions. lseek() is not a syscall in this kernel, so each cycle re-opens
// the file with O_TRUNC to get a fresh offset 0.

typedef unsigned long size_t;

#define SYS_EXIT     0
#define SYS_FORK     1
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_WAITPID  6
#define SYS_GETPID   8
#define SYS_YIELD    15

#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

#define ITERS 100

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc1(n,a)     sc6((n),(a),0,0,0,0)
#define sc0(n)       sc6((n),0,0,0,0,0)

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

// minimal signed->decimal for diagnostics (no libc)
static void outnum(long v) {
    char buf[24];
    char* p = buf + 23;
    *p = 0;
    if (v < 0) { out("-"); v = -v; }
    if (v == 0) { *--p = '0'; }
    else { while (v > 0) { *--p = (char)('0' + (v % 10)); v /= 10; } }
    out(p);
}

void _start(void) {
    out("FORKFDTEST: start\n");

    int child_inherit_ok = 1;   // every child USED its inherited fd
    int parent_live_ok   = 1;   // parent fd survived every child close/exit
    int setup_ok         = 1;
    long first_fail_iter = -1;
    long first_fail_det  = 0;   // child exit code, or a negative phase marker

    int i;
    for (i = 0; i < ITERS; i++) {
        // Fresh file each cycle (O_TRUNC -> size 0, fresh fd at offset 0). The
        // returned fd must be a real table slot (>= 3; 0/1/2 are stdio).
        long fd = sc6(SYS_OPEN, (long)"/tmp/forkfd.txt", O_CREAT | O_RDWR | O_TRUNC,
                      0644, 0, 0);
        if (fd < 3) { setup_ok = 0; first_fail_iter = i; first_fail_det = fd; break; }

        // Parent writes one byte through its own fd BEFORE forking (fd is live).
        if (sc3(SYS_WRITE, fd, (long)"P", 1) != 1) {
            setup_ok = 0; first_fail_iter = i; first_fail_det = -10;
            sc1(SYS_CLOSE, fd); break;
        }

        long cpid = sc0(SYS_FORK);
        if (cpid < 0) {                 // fork itself failed (e.g. unexpected fail-closed)
            parent_live_ok = 0;
            if (first_fail_iter < 0) { first_fail_iter = i; first_fail_det = -1; }
            sc1(SYS_CLOSE, fd); break;
        }

        if (cpid == 0) {
            // ---- CHILD ----
            // Use the INHERITED fd directly. On a kernel that does not inherit
            // fds, this write returns EBADF (< 0) -> exit(2): the fail-on-main
            // signal. On a correct kernel the cloned ramfs fd accepts the write.
            long wc = sc3(SYS_WRITE, fd, (long)"C", 1);
            if (wc != 1) sc3(SYS_EXIT, 2, 0, 0);
            // Child closes its OWN clone. Must not touch the parent's file.
            if (sc1(SYS_CLOSE, fd) < 0) sc3(SYS_EXIT, 3, 0, 0);
            sc3(SYS_EXIT, 0, 0, 0);
            for (;;) {}
        }

        // ---- PARENT ----
        // status MUST be a 32-bit int: sys_waitpid copy_to_user's exactly
        // sizeof(int) bytes, so a 64-bit long would keep stale high bits and a
        // child that exits 0 would read back nonzero (false failure).
        int status = -1;
        long w = -1;
        int t;
        for (t = 0; t < 2000000 && w != cpid; t++) {
            w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
            if (w != cpid) sc0(SYS_YIELD);
        }
        if (w != cpid) {                // reap failed
            parent_live_ok = 0;
            if (first_fail_iter < 0) { first_fail_iter = i; first_fail_det = -2; }
            sc1(SYS_CLOSE, fd); break;
        }
        if (status != 0) {              // child could not use inherited fd (main: 2)
            child_inherit_ok = 0;
            if (first_fail_iter < 0) { first_fail_iter = i; first_fail_det = status; }
        }

        // The parent's fd MUST still be live after the child closed + exited.
        if (sc3(SYS_WRITE, fd, (long)"Q", 1) < 0) {
            parent_live_ok = 0;
            if (first_fail_iter < 0) { first_fail_iter = i; first_fail_det = -3; }
        }
        // Parent close AFTER child close must not double-free / corrupt refs.
        if (sc1(SYS_CLOSE, fd) < 0) {
            parent_live_ok = 0;
            if (first_fail_iter < 0) { first_fail_iter = i; first_fail_det = -4; }
        }
    }

    int ok = setup_ok && child_inherit_ok && parent_live_ok;
    out("FORKFDTEST: iters="); outnum(i);
    out(" setup_open=");        outnum(setup_ok);
    out(" child_inherited_fd="); outnum(child_inherit_ok);
    out(" parent_unpoisoned=");  outnum(parent_live_ok);
    if (!ok) { out(" first_fail_iter="); outnum(first_fail_iter);
               out(" detail=");          outnum(first_fail_det); }
    out("\n");
    out(ok ? "FORKFDTEST RESULT: PASS\n" : "FORKFDTEST RESULT: FAIL\n");
    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}

/*
 * negsock.c -- NEGSOCK regression probe (freestanding, ring 3, own _start).
 * ========================================================================
 *
 * Proves the cross-process socket-ownership gate on sys_sock_shutdown /
 * sys_sock_setsockopt / sys_sock_getsockopt (kernel/net/socket.c). Before the
 * fix those three syscalls were missing the sock_fd_owned() check every other
 * socket syscall has, so ANY process could name another process's raw
 * g_socks[] index and tear down / reconfigure a socket it does not own.
 *
 * Why forking proves non-ownership (verified by reading the kernel, not
 * assumed):
 *   - kernel/net/socket.c sock_socket() stamps `s->owner_pid = cur->pid` ONCE,
 *     at creation time, from the CREATING process. Ownership is never
 *     reassigned anywhere else in socket.c.
 *   - The socket "fd" returned by SYS_SOCKET is a raw index into the single
 *     global g_socks[] table -- it is NOT an entry in the per-process VFS fd
 *     table.
 *   - kernel/core/syscall/handlers.c sys_fork() inherits VMAs, the *regular
 *     file* fd table (vfs_dup_fd_table), signal dispositions and registers --
 *     it never touches g_socks[] or any socket's owner_pid at all. There is
 *     no "socket fd table" for fork to clone in the first place.
 *   - Therefore after fork() the child has NO entry of its own for the
 *     parent's socket and g_socks[N].owner_pid still equals the PARENT's pid.
 *     sock_fd_owned() in the child evaluates false for fd N, exactly the
 *     cross-process scenario the fix targets. The test is not vacuous.
 *
 * Protocol: parent creates a UDP socket (fd N), forks. The CHILD (a distinct
 * pid that does not own N) calls shutdown/setsockopt/getsockopt on N; on a
 * fixed kernel all three must be denied (return a negative SOCK_E* code, e.g.
 * SOCK_EBADF == -9). The child exits 0 iff ALL THREE were denied, 1 otherwise.
 * The parent waitpid()s and reports "NEGSOCK: PASS" only if the child's exit
 * code says every call was denied. On a regressed (pre-fix) kernel the
 * child's shutdown() SUCCEEDS (return >= 0) and tears down the parent's
 * socket out from under it -- the child exits 1 and the parent reports
 * "NEGSOCK: FAIL".
 */

#define SYS_EXIT            0
#define SYS_FORK            1
#define SYS_WRITE           3
#define SYS_WAITPID         6
#define SYS_YIELD           15
#define SYS_SOCKET          51
#define SYS_CLOSE_SK        55
#define SYS_SETSOCKOPT      125
#define SYS_GETSOCKOPT      126
#define SYS_SHUTDOWN        127

#define SOCK_DGRAM          2

#define SOL_SOCKET          1
#define SO_REUSEADDR        2
#define SO_TYPE             3

#define SHUT_RDWR           2

/* kernel/include/socket.h: #define SOCK_EBADF (-9). This is the EXACT value
 * sock_fd_owned()'s gate returns (checked first, before any other arg
 * validation) -- pinning to it, not just "any negative", proves we hit the
 * ownership gate specifically rather than some unrelated rejection path. */
#define SOCK_EBADF          (-9)

typedef unsigned long size_t;

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

void _start(void) {
    out("NEGSOCK: start\n");

    int udp = (int)sc6(SYS_SOCKET, SOCK_DGRAM, 0, 0, 0, 0);
    if (udp < 0) {
        out("NEGSOCK: FAIL (could not create socket)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        for (;;) sc0(SYS_YIELD);
    }

    long cpid = sc3(SYS_FORK, 0, 0, 0);
    if (cpid == 0) {
        /* ---- CHILD: does NOT own fd `udp` (see header comment). Every one
         * of these three calls must be denied on the fixed kernel. ---- */
        int denied = 1;

        long r1 = sc6(SYS_SHUTDOWN, udp, SHUT_RDWR, 0, 0, 0);
        if (r1 == SOCK_EBADF) out("NEGSOCK:   [child] shutdown denied EBADF (good)\n");
        else        { out("NEGSOCK:   [child] shutdown NOT denied EBADF (BAD)\n"); denied = 0; }

        int optval = 1;
        long r2 = sc6(SYS_SETSOCKOPT, udp, SOL_SOCKET, SO_REUSEADDR,
                      (long)&optval, sizeof(int));
        if (r2 == SOCK_EBADF) out("NEGSOCK:   [child] setsockopt denied EBADF (good)\n");
        else        { out("NEGSOCK:   [child] setsockopt NOT denied EBADF (BAD)\n"); denied = 0; }

        int optout = -1;
        long r3 = sc6(SYS_GETSOCKOPT, udp, SOL_SOCKET, SO_TYPE,
                      (long)&optout, sizeof(int));
        if (r3 == SOCK_EBADF) out("NEGSOCK:   [child] getsockopt denied EBADF (good)\n");
        else        { out("NEGSOCK:   [child] getsockopt NOT denied EBADF (BAD)\n"); denied = 0; }

        sc3(SYS_EXIT, denied ? 0 : 1, 0, 0);
        for (;;) sc0(SYS_YIELD);
    } else if (cpid > 0) {
        /* ---- PARENT: reap the child and check its verdict. ---- */
        int status = 0;
        long w = 0;
        for (int t = 0; t < 2000000 && w != cpid; t++) {
            w = sc3(SYS_WAITPID, cpid, (long)&status, 0);
            if (w != cpid) sc0(SYS_YIELD);
        }

        if (w != cpid) {
            out("NEGSOCK: FAIL (waitpid timeout reaping child)\n");
            sc3(SYS_EXIT, 1, 0, 0);
            for (;;) sc0(SYS_YIELD);
        }

        sc1(SYS_CLOSE_SK, udp); /* parent is the true owner; tidy up */

        if (status == 0) {
            out("NEGSOCK: PASS (cross-process shutdown/setsockopt/getsockopt all denied)\n");
            sc3(SYS_EXIT, 0, 0, 0);
        } else {
            out("NEGSOCK: FAIL (a cross-process socket op was NOT denied)\n");
            sc3(SYS_EXIT, 1, 0, 0);
        }
        for (;;) sc0(SYS_YIELD);
    } else {
        out("NEGSOCK: FAIL (fork failed)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        for (;;) sc0(SYS_YIELD);
    }
}

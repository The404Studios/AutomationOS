/* tool_mv -- TOOLSET-0 gated move/rename. argv=[src, dst].
 *
 * One destructive operation, fully policy-gated. The agent (sbin/agentd) is
 * HOSTILE TEXT, so BOTH endpoints are checked with path_write_allowed() before
 * any kernel call: src is destroyed by the rename and dst is overwritten, so
 * each must live in the writable allowlist (/tmp, /home, /usr/src) and be free
 * of traversal / protected-prefix tricks. If EITHER side is denied we print the
 * one-line outcome "DENY policy" and exit 2 -- no SYS_RENAME is issued.
 *
 * fd 1 is the capability channel the agent CAPTURES as the model-visible RESULT,
 * so the single-line outcome (DENY / MV / ERR) goes to fd 1. The kernel's
 * SYS_RENAME(35) returns EXACTLY 0 on success, <0 on error, so we test r!=0.
 *
 * FREESTANDING ring-3 ELF: NO libc, NO headers, single self-contained file.
 * crt0 provides _start and calls main(argc,argv); we never define _start. The
 * .o must carry ZERO fs:0x28 references (the build flags add
 * -fno-stack-protector; the orchestrator gates this).
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified vs kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE   3
#define SYS_RENAME  35   /* sc(35, src, dst, 0) -> 0 ok, <0 err */

#define FD_OUT      1    /* capability channel: the model-visible RESULT */

/* -----------------------------------------------------------------------
 * Inline syscall helper (3 args is all we need). Copied verbatim from the kit.
 * --------------------------------------------------------------------- */
static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding string output helpers (the kit's canonical forms).
 * --------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }

/* -----------------------------------------------------------------------
 * THE PATH GATE (copied VERBATIM from the kit; the model is hostile text).
 * Allow only writable scratch/project paths; reject traversal and any
 * protected system prefix. Deny by default.
 * --------------------------------------------------------------------- */
static int has_sub(const char *p, const char *s) {
    for (int i = 0; p[i]; i++) {
        int j = 0; while (s[j] && p[i + j] == s[j]) j++;
        if (!s[j]) return 1;
    }
    return 0;
}
static int starts(const char *p, const char *pre) {
    int j = 0; while (pre[j]) { if (p[j] != pre[j]) return 0; j++; } return 1;
}
static int path_write_allowed(const char *p) {
    if (!p || !p[0]) return 0;
    if (has_sub(p, "..")) return 0;                                  /* no traversal           */
    if (starts(p, "/boot") || starts(p, "/sbin") || starts(p, "/bin") ||
        starts(p, "/etc")  || has_sub(p, "kernel")) return 0;        /* protected system paths */
    if (starts(p, "/tmp") || starts(p, "/home") ||
        starts(p, "/usr/src")) return 1;                            /* writable allowlist     */
    return 0;                                                        /* deny by default        */
}

/* =======================================================================
 *  Entry point. crt0 parses argc/argv off the kernel-prepared stack and
 *  calls main(argc, argv), feeding the return value to SYS_EXIT.
 * ======================================================================= */
int main(int argc, char **argv) {
    /* Validate argv before any deref: need argv[1]=src and argv[2]=dst, both
     * non-empty. A malformed invocation is a policy failure (fail closed). */
    if (argc < 3 || !argv[1] || !argv[1][0] || !argv[2] || !argv[2][0]) {
        out(FD_OUT, "DENY policy\n");
        return 2;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    /* GATE BOTH endpoints: the rename destroys src and overwrites dst. */
    if (!path_write_allowed(src) || !path_write_allowed(dst)) {
        out(FD_OUT, "DENY policy\n");
        return 2;
    }

    /* The single destructive op. SYS_RENAME returns exactly 0 on success. */
    long r = sc(SYS_RENAME, (long)src, (long)dst, 0);
    if (r == 0) {
        out(FD_OUT, "MV ");  out(FD_OUT, src);  out(FD_OUT, " ");  out(FD_OUT, dst);
        out(FD_OUT, "\n");
        return 0;
    }

    out(FD_OUT, "ERR mv\n");
    return 1;
}

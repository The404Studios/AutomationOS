/*
 * tool_rm -- TOOLSET-0 gated delete_file. argv=[path].
 * ====================================================
 *
 * FREESTANDING ring-3 agent tool (NO libc, NO headers, single self-contained
 * .c). crt0 provides _start and calls main(argc,argv) -- we do NOT define
 * _start. The .o must carry ZERO fs:0x28 references (the orchestrator gates
 * this); -fno-stack-protector is added by the build flags.
 *
 * CONTRACT (the model that drives this tool is HOSTILE TEXT):
 *   1. validate argc/argv -- never deref a missing/empty path.
 *   2. GATE the path through path_write_allowed() (copied VERBATIM from the
 *      kit). This is the load-bearing defense: it rejects traversal ("..") and
 *      every protected system path (/boot /sbin /bin /etc + anything naming
 *      "kernel"), allowing ONLY scratch/project dirs (/tmp /home /usr/src).
 *      On a DENY we print "DENY policy <path>" to fd1 and return 2 -- no
 *      unlink syscall is ever issued for a denied path.
 *   3. on allow, r = sc(SYS_UNLINK, path, 0, 0). SYS_UNLINK returns EXACTLY 0
 *      on success, so we test (r != 0) for failure:
 *        r == 0  -> print "RM <path>" to fd1, return 0
 *        r != 0  -> print "ERR rm"    to fd1, return 1
 *
 * FD CONVENTION: fd 1 is the capability channel the agent (sbin/agentd)
 * captures as the RESULT the model reads -- we print the ONE-LINE OUTCOME
 * there ("RM <path>" / "DENY policy <path>" / "ERR rm") and nothing else.
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE   3
#define SYS_UNLINK  34   /* sc(34, path, 0, 0) -> EXACTLY 0 ok, <0 err */

#define FD_OUT      1    /* the capability channel (agent reads this) */

/* -----------------------------------------------------------------------
 * Inline syscall helper (copied verbatim from the ABI kit; 3 args suffice).
 * --------------------------------------------------------------------- */
static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding string output helpers (copied verbatim from the kit).
 * --------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }

/* -----------------------------------------------------------------------
 * THE PATH GATE -- copied VERBATIM from the kit. The model is hostile text;
 * this is the only thing standing between it and deleting system files.
 *   - empty path                 -> deny
 *   - any ".." substring         -> deny (traversal)
 *   - /boot /sbin /bin /etc or
 *     anything containing "kernel" -> deny (protected system paths)
 *   - /tmp /home /usr/src        -> allow (scratch / project dirs)
 *   - everything else            -> deny by default
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
    if (has_sub(p, "..")) return 0;
    if (starts(p, "/boot") || starts(p, "/sbin") || starts(p, "/bin") ||
        starts(p, "/etc") || has_sub(p, "kernel")) return 0;
    if (starts(p, "/tmp") || starts(p, "/home") || starts(p, "/usr/src")) return 1;
    return 0;
}

/* =======================================================================
 *  Entry point. crt0 parses argc/argv off the kernel-prepared stack and
 *  feeds our return value to SYS_EXIT.
 * ======================================================================= */
int main(int argc, char **argv) {
    /* Validate argv BEFORE any deref: a missing/empty path is a policy DENY. */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        out(FD_OUT, "DENY policy \n");
        return 2;
    }
    const char *path = argv[1];

    /* THE GATE: reject traversal + protected paths before touching the FS. */
    if (!path_write_allowed(path)) {
        out(FD_OUT, "DENY policy ");
        out(FD_OUT, path);
        out(FD_OUT, "\n");
        return 2;
    }

    /* Allowed: perform the delete. SYS_UNLINK returns EXACTLY 0 on success. */
    long r = sc(SYS_UNLINK, (long)path, 0, 0);
    if (r == 0) {
        out(FD_OUT, "RM ");
        out(FD_OUT, path);
        out(FD_OUT, "\n");
        return 0;
    }
    out(FD_OUT, "ERR rm\n");
    return 1;
}

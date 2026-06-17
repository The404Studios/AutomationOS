/* tool_mkdir -- TOOLSET-0 gated agent tool: make a directory.
 * ===========================================================
 *
 * FREESTANDING ring-3 ELF (NO libc, NO headers, single self-contained .c).
 * crt0 provides _start and calls main(argc, argv) -- we do NOT define _start.
 * The .o MUST carry ZERO fs:0x28 references (the orchestrator gates this; the
 * build adds -fno-stack-protector).
 *
 * argv = [path]. SYS_MKDIR(67) creates the directory (mode arg 0 => 0755).
 *
 * THE GATE (path_write_allowed): mkdir mutates filesystem state, so -- unlike
 * the read-only tools (tool_read/tool_ls/tool_stat) which only screen ".." --
 * this tool runs the FULL writable-path policy. The agent driving this tool is
 * HOSTILE TEXT: it may write/create ONLY under the scratch/project allowlist
 * (/tmp, /home, /usr/src); traversal ("..") and protected system trees
 * (/boot /sbin /bin /etc, anything containing "kernel") are denied by default.
 *
 * FD CONVENTION (CRITICAL): fd 1 is the capability channel the agent (sbin/agentd)
 * captures as the RESULT the model reads. We print exactly ONE outcome line to
 * fd 1 in every path:
 *   - policy denied : "DENY policy <path>"   (return 2)
 *   - success       : "MKDIR <path>"         (return 0)
 *   - syscall error : "ERR mkdir"            (return 1)
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE  3
#define SYS_MKDIR  67   /* sc(67, path, mode, 0) -> EXACTLY 0 on success, <0 err */

#define FD_OUT     1    /* capability channel: the agent reads this as RESULT   */

/* -----------------------------------------------------------------------
 * Inline syscall wrapper (3 args; copied verbatim from the ABI kit).
 * --------------------------------------------------------------------- */
static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding helpers.
 * --------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }

/* -----------------------------------------------------------------------
 * THE PATH GATE (copied VERBATIM from the ABI kit -- the model is hostile
 * text; every writable/destructive-path tool ships this exact policy).
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
    if (has_sub(p, "..")) return 0;                                  /* no traversal */
    if (starts(p, "/boot") || starts(p, "/sbin") || starts(p, "/bin") ||
        starts(p, "/etc")  || has_sub(p, "kernel")) return 0;        /* protected */
    if (starts(p, "/tmp") || starts(p, "/home") ||
        starts(p, "/usr/src")) return 1;                            /* allowlist */
    return 0;                                                        /* deny by default */
}

/* -----------------------------------------------------------------------
 * Entry point.
 *
 * crt0 parses argc/argv off the kernel-prepared stack, calls main(), and
 * feeds our return value to SYS_EXIT. We validate argv before any deref.
 * --------------------------------------------------------------------- */
int main(int argc, char **argv) {
    /* Validate argc/argv before touching argv[1] (never deref a missing arg). */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        out(FD_OUT, "DENY policy \n");   /* empty path is denied by the gate */
        return 2;
    }
    const char *path = argv[1];

    /* GATE: the agent may only mkdir under the scratch/project allowlist. */
    if (!path_write_allowed(path)) {
        out(FD_OUT, "DENY policy ");
        out(FD_OUT, path);
        out(FD_OUT, "\n");
        return 2;
    }

    /* mode arg 0 => kernel applies 0755. mkdir returns EXACTLY 0 on success. */
    long r = sc(SYS_MKDIR, (long)path, 0, 0);
    if (r == 0) {
        out(FD_OUT, "MKDIR ");
        out(FD_OUT, path);
        out(FD_OUT, "\n");
        return 0;
    }

    out(FD_OUT, "ERR mkdir\n");
    return 1;
}

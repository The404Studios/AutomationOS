/* tool_ps -- TOOLSET-0 ps (gated agent tool). NO ARGS.
 * ====================================================
 * Enumerates every live process via SYS_PROCLIST(44) into a static
 * procinfo_t[64] (the 64-byte ABI from userspace/apps/ps/ps.c) and prints
 * ONE compact line per process to fd 1:
 *
 *     <pid> <ppid> <state> <name>
 *
 * - state is a short word (READY/RUNNING/BLOCKED/...), not the raw number.
 * - name is read from the fixed 32-byte field and forced NUL-terminated.
 *
 * This is a READ-type tool, so the observation (the process listing) is the
 * RESULT the agent captures: it goes to fd 1 (the capability channel). Any
 * diagnostic (e.g. a failed syscall) goes to fd 2 and never pollutes fd 1.
 *
 * FREESTANDING ring-3 ELF: NO libc, NO headers, crt0 supplies _start and
 * calls main(argc,argv). Built with -fno-stack-protector; the .o MUST carry
 * ZERO fs:0x28 references (the orchestrator gates this).
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE     3
#define SYS_PROCLIST  44   /* sc(44, buf, max, 0) -> count, or <0 on error */

#define FD_OUT   1         /* capability channel: the RESULT the agent reads  */
#define FD_ERR   2         /* diagnostics only: never the observation         */

#define MAX_PROCS 64       /* static buffer cap; clamp the returned count too */

/* Process state constants (mirror userspace/lib/aictl/aictl.h + ps.c). */
#define PROC_STATE_CREATED    0
#define PROC_STATE_READY      1
#define PROC_STATE_RUNNING    2
#define PROC_STATE_BLOCKED    3
#define PROC_STATE_TERMINATED 4

typedef unsigned int       u32;
typedef unsigned long long u64;

/* -----------------------------------------------------------------------
 * 64-byte shallow process entry from SYS_PROCLIST (byte-for-byte == ps.c):
 *   off  0: u32 pid
 *   off  4: u32 parent_pid
 *   off  8: u32 state
 *   off 12: u32 flags
 *   off 16: char name[32]
 *   off 48: u64 cpu_ticks
 *   off 56: u64 ctx_switches
 *   total : 64 bytes
 * --------------------------------------------------------------------- */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;

/* Compile-time layout assertion -- a wrong size here is a hard build error. */
typedef char _procinfo_size_assert[sizeof(procinfo_t) == 64 ? 1 : -1];

/* -----------------------------------------------------------------------
 * Syscall wrapper (3 args is all this read-only tool needs).
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

/* Write an unsigned decimal directly to fd (small numbers only). */
static void out_u(int fd, u64 val) {
    char tmp[24];
    int  i = 0;
    if (val == 0) { out(fd, "0"); return; }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    char rev[24];
    int  j = 0;
    while (i-- > 0) rev[j++] = tmp[i];
    rev[j] = '\0';
    out(fd, rev);
}

/* Map a numeric state to its short word. */
static const char *state_name(u32 st) {
    switch (st) {
    case PROC_STATE_CREATED:    return "CREATED";
    case PROC_STATE_READY:      return "READY";
    case PROC_STATE_RUNNING:    return "RUNNING";
    case PROC_STATE_BLOCKED:    return "BLOCKED";
    case PROC_STATE_TERMINATED: return "TERMINATED";
    default:                    return "UNKNOWN";
    }
}

/* -----------------------------------------------------------------------
 * Entry point. NO args are consumed -- crt0 still passes argc/argv, which we
 * deliberately ignore (a read-only enumeration takes no parameters).
 * --------------------------------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    static procinfo_t procs[MAX_PROCS];
    long n = sc(SYS_PROCLIST, (long)procs, MAX_PROCS, 0);

    if (n < 0) {                       /* syscall failed: diagnostic to fd 2 */
        out(FD_ERR, "ERR proclist\n");
        return 2;
    }
    if (n > MAX_PROCS) n = MAX_PROCS;  /* defensive clamp against the buffer */

    for (long i = 0; i < n; i++) {
        /* Force NUL-termination of the fixed 32-byte name field before use. */
        char name[33];
        for (int j = 0; j < 32; j++) name[j] = procs[i].name[j];
        name[32] = '\0';

        /* One compact line: "<pid> <ppid> <state> <name>" -> fd 1 (RESULT). */
        out_u(FD_OUT, (u64)procs[i].pid);
        out(FD_OUT, " ");
        out_u(FD_OUT, (u64)procs[i].parent_pid);
        out(FD_OUT, " ");
        out(FD_OUT, state_name(procs[i].state));
        out(FD_OUT, " ");
        out(FD_OUT, name);
        out(FD_OUT, "\n");
    }
    return 0;
}

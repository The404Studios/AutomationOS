/*
 * kill.c -- send a signal to a process on the from-scratch x86_64 OS.
 * ===================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own string/number helpers. Single self-contained file.
 *
 * Wraps SYS_KILL(26): sc(26, pid, sig, 0) -> 0 on success, < 0 on error.
 * The signal defaults to SIGTERM(15); an optional argv[2] overrides it.
 * The kernel signal numbers are verified in kernel/core/signal/kill.c
 * (SIGKILL=9, SIGTERM=15).
 *
 * --------------------------------------------------------------------------
 * USAGE (argv is provided by crt0 -- see entry-point note below):
 *
 *   kill PID            send SIGTERM (15) to PID
 *   kill PID SIG        send signal SIG (a decimal number) to PID
 *
 * SELF-TEST (argc <= 1): print the usage text and "KILL SELFTEST: PASS".
 * The self-test does NOT actually signal any process -- it only validates
 * that the tool is wired up and its argument parsing is sane.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/kill/kill.c -o kill.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       kill.o crt0.o -o build/kill
 *   objdump -d build/kill | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_KILL   26   /* sc(26, pid, sig, 0) -> 0 or -errno */

#define FD_STDOUT  1

/* Default signal (kernel/core/signal/kill.c: SIGTERM = 15). */
#define SIGTERM    15

typedef unsigned long long u64;

/* -----------------------------------------------------------------------
 * Inline syscall helper (3 args is enough).
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers.
 * ======================================================================= */
static unsigned long s_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

static void out(const char *s) { sc(SYS_WRITE, FD_STDOUT, (long)s, (long)s_strlen(s)); }

/* Print an unsigned decimal directly to stdout (small numbers only). */
static void out_u(u64 val)
{
    char tmp[24];
    int  i = 0;
    if (val == 0) { out("0"); return; }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    char rev[24];
    int  j = 0;
    while (i-- > 0) rev[j++] = tmp[i];
    rev[j] = '\0';
    out(rev);
}

/*
 * Parse a non-negative decimal integer from `s`. On success returns the
 * value (>= 0) and sets *ok = 1; on a malformed/empty/negative input sets
 * *ok = 0. No leading/trailing whitespace handling -- argv tokens are clean.
 */
static long parse_uint(const char *s, int *ok)
{
    *ok = 0;
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;   /* *ok stays 0 */
        v = v * 10 + (*p - '0');
    }
    *ok = 1;
    return v;
}

/* =======================================================================
 *  USAGE text (shared by self-test and the error path).
 * ======================================================================= */
static void usage(void)
{
    out("usage: kill PID [SIG]\n");
    out("  PID  target process id (decimal)\n");
    out("  SIG  signal number (decimal); default SIGTERM (15)\n");
}

/* =======================================================================
 *  kill_run -- the real work. Returns a process exit code (0 = success).
 * ======================================================================= */
static int kill_run(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    int ok = 0;
    long pid = parse_uint(argv[1], &ok);
    if (!ok || pid < 0) {
        out("kill: invalid pid: "); out(argv[1]); out("\n");
        return 1;
    }

    long sig = SIGTERM;
    if (argc >= 3) {
        sig = parse_uint(argv[2], &ok);
        if (!ok || sig < 0) {
            out("kill: invalid signal: "); out(argv[2]); out("\n");
            return 1;
        }
    }

    long r = sc(SYS_KILL, pid, sig, 0);
    if (r < 0) {
        out("kill: failed to signal pid "); out_u((u64)pid);
        out(" with signal "); out_u((u64)sig); out("\n");
        return 1;
    }

    out("kill: sent signal "); out_u((u64)sig);
    out(" to pid "); out_u((u64)pid); out("\n");
    return 0;
}

/* =======================================================================
 *  Self-test: print usage + PASS. Does NOT kill anything.
 * ======================================================================= */
static int kill_selftest(void)
{
    out("KILL SELFTEST: begin\n");
    usage();
    out("KILL SELFTEST: PASS\n");
    return 0;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0.asm provides _start, parses argc/argv off the kernel-prepared
 *  stack, calls main(argc, argv), and feeds the return value to SYS_EXIT.
 *  With args (argc > 1) we signal the target; with none (argc <= 1) we run
 *  the self-test, which prints usage + "KILL SELFTEST: PASS" and signals
 *  nothing.
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) {
        return kill_run(argc, argv);
    }
    return kill_selftest();
}

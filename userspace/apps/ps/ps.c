/*
 * ps.c -- process status reporter for the from-scratch x86_64 OS.
 * ===============================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own string/number helpers. Single self-contained file.
 *
 * Enumerates every live process via SYS_PROCLIST(44) and prints a table:
 *
 *     PID   PPID  STATE       NAME
 *       1      0  RUNNING     init
 *       ...
 *
 * STATE is the named process state (see PROC_STATE_* below), matching the
 * 48-byte procinfo_t ABI in userspace/lib/aictl/aictl.h EXACTLY.
 *
 * --------------------------------------------------------------------------
 * USAGE (argv is provided by crt0 -- see entry-point note below):
 *
 *   ps                run the SELF-TEST (argc <= 1) and print the table
 *   ps -e | ps aux    print the full process table (any args => full listing)
 *
 * SELF-TEST: SYS_PROCLIST returning >= 1 process => "PS SELFTEST: PASS",
 * otherwise "PS SELFTEST: FAIL". The table is printed either way.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/ps/ps.c -o ps.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       ps.o crt0.o -o build/ps
 *   objdump -d build/ps | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Freestanding integer types (no stdint.h). Match aictl.h.
 * --------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef unsigned long long u64;

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT      0
#define SYS_WRITE     3
#define SYS_PROCLIST  44   /* sc(44, buf, max, 0) -> count or -errno */

#define FD_STDOUT     1
#define MAX_PROCS     128

/* Process state constants (mirror userspace/lib/aictl/aictl.h). */
#define PROC_STATE_CREATED    0
#define PROC_STATE_READY      1
#define PROC_STATE_RUNNING    2
#define PROC_STATE_BLOCKED    3
#define PROC_STATE_TERMINATED 4

/* -----------------------------------------------------------------------
 * 64-byte shallow process entry from SYS_PROCLIST (byte-for-byte aictl.h).
 *   offset  0: u32 pid
 *   offset  4: u32 parent_pid
 *   offset  8: u32 state
 *   offset 12: u32 flags
 *   offset 16: char name[32]
 *   offset 48: u64 cpu_ticks      (timer ticks observed while running)
 *   offset 56: u64 ctx_switches   (number of times dispatched)
 *   total: 64 bytes
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

/* Compile-time layout assertion (matches aictl.h). */
typedef char _procinfo_size_assert[sizeof(procinfo_t) == 64 ? 1 : -1];

/* -----------------------------------------------------------------------
 * Inline syscall helper (3 args is plenty here).
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
 *  Freestanding helpers: strings, number formatting, output buffer.
 * ======================================================================= */
static unsigned long s_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Output buffer (each app gets a tiny stack, so keep big things static). */
#define BUF_CAP 8192
static char g_buf[BUF_CAP];
static unsigned long g_pos;

static void buf_putc(char c)
{
    if (g_pos + 1 < BUF_CAP) g_buf[g_pos++] = c;
    g_buf[g_pos] = '\0';
}
static void buf_puts(const char *s)
{
    while (s && *s && g_pos + 1 < BUF_CAP) g_buf[g_pos++] = *s++;
    g_buf[g_pos] = '\0';
}

/* Append unsigned decimal. */
static void buf_putu(u64 val)
{
    char tmp[24];
    int  i = 0;
    if (val == 0) { buf_putc('0'); return; }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    while (i-- > 0) buf_putc(tmp[i]);
}

/* Append val right-justified in a field of `width` (space padded). */
static void buf_putu_pad(u64 val, int width)
{
    int digits = 1;
    u64 v = val;
    while (v >= 10ULL) { v /= 10ULL; digits++; }
    for (int i = digits; i < width; i++) buf_putc(' ');
    buf_putu(val);
}

/* Append a string left-justified in a field of `width` (space padded). */
static void buf_puts_pad(const char *s, int width)
{
    int n = (int)s_strlen(s);
    buf_puts(s);
    for (int i = n; i < width; i++) buf_putc(' ');
}

static void flush_out(void)
{
    sc(SYS_WRITE, FD_STDOUT, (long)g_buf, (long)g_pos);
    g_pos = 0;
    g_buf[0] = '\0';
}

/* Map a numeric state to its name. */
static const char *state_name(u32 st)
{
    switch (st) {
    case PROC_STATE_CREATED:    return "CREATED";
    case PROC_STATE_READY:      return "READY";
    case PROC_STATE_RUNNING:    return "RUNNING";
    case PROC_STATE_BLOCKED:    return "BLOCKED";
    case PROC_STATE_TERMINATED: return "TERMINATED";
    default:                    return "UNKNOWN";
    }
}

/* =======================================================================
 *  Render the process table from a fetched procinfo array.
 *  Returns the process count (>= 0) or the negative syscall error.
 * ======================================================================= */
static long ps_render(void)
{
    static procinfo_t procs[MAX_PROCS];
    long n = sc(SYS_PROCLIST, (long)procs, MAX_PROCS, 0);

    buf_puts("  PID   PPID  STATE          TICKS     CTXSW  NAME\n");
    buf_puts("  ----  ----  ----------  --------  --------  --------------------------------\n");

    if (n < 0) {
        buf_puts("  (process list unavailable)\n");
        return n;
    }
    if (n > MAX_PROCS) n = MAX_PROCS;   /* defensive clamp */

    for (long i = 0; i < n; i++) {
        buf_puts("  ");
        buf_putu_pad((u64)procs[i].pid, 4);
        buf_puts("  ");
        buf_putu_pad((u64)procs[i].parent_pid, 4);
        buf_puts("  ");
        buf_puts_pad(state_name(procs[i].state), 10);
        buf_puts("  ");
        /* Scheduler stats from the 64-byte SYS_PROCLIST record. */
        buf_putu_pad(procs[i].cpu_ticks, 8);
        buf_puts("  ");
        buf_putu_pad(procs[i].ctx_switches, 8);
        buf_puts("  ");

        /* Force NUL-termination of the fixed 32-byte name field. */
        char name[33];
        int j;
        for (j = 0; j < 32; j++) name[j] = procs[i].name[j];
        name[32] = '\0';
        buf_puts(name);
        buf_putc('\n');
    }
    if (n == 0) buf_puts("  (no processes)\n");
    return n;
}

/* =======================================================================
 *  Self-test: enumerate; >= 1 process => PASS.
 * ======================================================================= */
static int ps_selftest(void)
{
    buf_puts("PS SELFTEST: begin\n");
    long n = ps_render();
    if (n >= 1) {
        buf_puts("PS SELFTEST: PASS\n");
        flush_out();
        return 0;
    }
    buf_puts("PS SELFTEST: FAIL\n");
    flush_out();
    return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0.asm provides _start, parses argc/argv off the kernel-prepared
 *  stack, calls main(argc, argv), and feeds the return value to SYS_EXIT.
 *  With args (argc > 1) we print the full table; with none (argc <= 1) we
 *  run the self-test, which still prints "PS SELFTEST: PASS/FAIL".
 * ======================================================================= */
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        (void)ps_render();
        flush_out();
        return 0;
    }
    return ps_selftest();
}

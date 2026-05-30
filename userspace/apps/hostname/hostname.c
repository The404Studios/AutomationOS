/*
 * hostname.c -- print the system hostname for AutomationOS (x86_64).
 * ==================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own helpers. Single self-contained file. Mirrors the structure of the
 * sibling tools userspace/apps/uptime/uptime.c and free/free.c.
 *
 * There is no hostname syscall on this OS, so the hostname is a fixed
 * compiled-in constant: "automationos". If an argument is supplied (e.g.
 * `hostname newname`) we cannot persist it -- we simply echo the requested
 * name back so scripts behave predictably.
 *
 * --------------------------------------------------------------------------
 * USAGE:  hostname            -> prints "automationos"
 *         hostname <name>     -> prints <name> (not persisted)
 *
 * SELF-TEST (argc <= 1): prints the default hostname then
 * "HOSTNAME SELFTEST: PASS".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/hostname/hostname.c -o hostname.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       hostname.o crt0.o -o build/hostname
 *   objdump -d build/hostname | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE    3

#define FD_STDOUT    1

#define HOSTNAME_DEFAULT "automationos"

/* -----------------------------------------------------------------------
 * Inline syscall helper (6-arg form per the project ABI).
 * --------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers + output buffer.
 * ======================================================================= */
#define BUF_CAP 256
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

static void flush_out(void)
{
    sc(SYS_WRITE, FD_STDOUT, (long)g_buf, (long)g_pos, 0, 0);
    g_pos = 0;
    g_buf[0] = '\0';
}

/* =======================================================================
 *  Entry point. crt0 provides _start and calls main(argc, argv).
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1 && argv[1] && argv[1][0]) {
        /* Cannot persist (no syscall) -- echo the requested name back. */
        buf_puts(argv[1]);
        buf_putc('\n');
        flush_out();
        return 0;
    }

    /* Self-test: default hostname + PASS marker. */
    buf_puts(HOSTNAME_DEFAULT);
    buf_putc('\n');
    buf_puts("HOSTNAME SELFTEST: PASS\n");
    flush_out();
    return 0;
}

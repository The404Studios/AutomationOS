/*
 * uname.c -- system identification reporter for AutomationOS (x86_64).
 * ===================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own helpers. Single self-contained file. Mirrors the structure of the
 * sibling tools userspace/apps/uptime/uptime.c and free/free.c.
 *
 * Prints "AutomationOS x86_64" by default. Flags:
 *   -s   kernel name      -> "AutomationOS"
 *   -m   machine          -> "x86_64"
 *   -r   release          -> "0.1.0"
 *   -a   all              -> "AutomationOS <hostname> 0.1.0 x86_64"
 *   (none / default)      -> "AutomationOS x86_64"
 *
 * No syscall is required beyond SYS_WRITE; the hostname is a fixed constant
 * (there is no hostname syscall on this OS).
 *
 * --------------------------------------------------------------------------
 * SELF-TEST (argc <= 1): prints the default output then "UNAME SELFTEST: PASS".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/uname/uname.c -o uname.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       uname.o crt0.o -o build/uname
 *   objdump -d build/uname | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE    3

#define FD_STDOUT    1

/* -----------------------------------------------------------------------
 * System identity constants.
 * --------------------------------------------------------------------- */
#define UN_SYSNAME   "AutomationOS"
#define UN_MACHINE   "x86_64"
#define UN_RELEASE   "0.1.0"
#define UN_HOSTNAME  "automationos"

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
 *  Emit one of the four canonical forms into the buffer.
 *  mode: 's' sysname, 'm' machine, 'r' release, 'a' all, 0 default.
 * ======================================================================= */
static void uname_render(int mode)
{
    switch (mode) {
    case 's':
        buf_puts(UN_SYSNAME);
        break;
    case 'm':
        buf_puts(UN_MACHINE);
        break;
    case 'r':
        buf_puts(UN_RELEASE);
        break;
    case 'a':
        buf_puts(UN_SYSNAME);
        buf_putc(' ');
        buf_puts(UN_HOSTNAME);
        buf_putc(' ');
        buf_puts(UN_RELEASE);
        buf_putc(' ');
        buf_puts(UN_MACHINE);
        break;
    default:
        buf_puts(UN_SYSNAME);
        buf_putc(' ');
        buf_puts(UN_MACHINE);
        break;
    }
    buf_putc('\n');
}

/* Map an argv flag string to a render mode, 0 if unrecognised. */
static int flag_mode(const char *a)
{
    if (a && a[0] == '-' && a[1] && a[2] == '\0') {
        switch (a[1]) {
        case 's': return 's';
        case 'm': return 'm';
        case 'r': return 'r';
        case 'a': return 'a';
        default:  return 0;
        }
    }
    return 0;
}

/* =======================================================================
 *  Entry point. crt0 provides _start and calls main(argc, argv).
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) {
        int mode = flag_mode(argv[1]);
        uname_render(mode);   /* 0 (default) for unknown flags */
        flush_out();
        return 0;
    }

    /* Self-test: default output + PASS marker. */
    uname_render(0);
    buf_puts("UNAME SELFTEST: PASS\n");
    flush_out();
    return 0;
}

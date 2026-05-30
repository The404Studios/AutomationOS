/*
 * beep.c -- play a tone through the HDA audio driver (freestanding, ring 3).
 * =========================================================================
 *
 * Tiny no-libc userspace program that asks the kernel to play a tone via a
 * proposed SYS_BEEP syscall:
 *
 *     SYS_BEEP(freq_hz, ms)  ->  0 on success, negative on error
 *
 * SYS_BEEP is NOT yet wired into the kernel syscall table. The integrator must
 * register it (see the AUDIO ENGINEER report). Number 41 is the next free slot
 * after SYS_GET_TICKS_MS (40) in kernel/include/syscall.h. Until it is wired,
 * the call returns -ENOTSUP (-1) and this program prints a notice and exits 1.
 *
 * Usage: this is a placeholder/demo. It plays A5 (880 Hz) for 250 ms, then
 * a 440 Hz "A4" for 250 ms.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/beep/beep.c -o /tmp/beep.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld /tmp/beep.o -o /tmp/beep.elf
 *   objdump -d /tmp/beep.elf | grep fs:0x28    # MUST be empty
 *
 * Serial output (via SYS_WRITE on fd 1):
 *   [BEEP] start
 *   [BEEP] ok      (when SYS_BEEP is wired)
 *   [BEEP] unsupported (SYS_BEEP not wired yet)
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (must match kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_BEEP   41   /* PROPOSED: not yet in kernel syscall.h / table */

/* Two-arg syscall helper. RAX=number, RDI=a1, RSI=a2; clobbers per SYSV. */
static inline long sc2(long n, long a1, long a2)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2)
                 : "rcx", "r11", "rdx", "memory");
    return r;
}

/* Three-arg syscall helper (for SYS_WRITE: fd, buf, len). */
static inline long sc3(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc3(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

static long beep(unsigned freq_hz, unsigned ms)
{
    return sc2(SYS_BEEP, (long)freq_hz, (long)ms);
}

/*
 * _start - entry point. The kernel jumps here in ring 3 with a user stack.
 */
void _start(void)
{
    serial_print("[BEEP] start\n");

    long rc = beep(880, 250);     /* A5 */
    if (rc < 0) {
        /* SYS_BEEP not wired yet (or no audio hardware). */
        serial_print("[BEEP] unsupported (SYS_BEEP not wired or no HDA)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        __builtin_unreachable();
    }

    beep(440, 250);               /* A4 */

    serial_print("[BEEP] ok\n");
    sc3(SYS_EXIT, 0, 0, 0);
    __builtin_unreachable();
}

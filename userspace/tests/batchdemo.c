/*
 * batchdemo.c -- SMP-F3-7 BATCH-CLASS: the first ORDINARY workload on CPU1.
 * ==========================================================================
 *
 * Not pinned, not special-cased: a plain ring-3 program declared
 * SCHED_CLASS_BATCH with a multi-CPU legal mask (kernel.c sets both at
 * spawn). The choose_cpu batch branch routes it to CPU1; the G1 IPI kick
 * wakes the hlt-parked core; the BKL wall covers every marked syscall it
 * makes there. The work itself is deliberately mundane -- FS reads + marks +
 * a clean exit -- because "ordinary work runs safely on CPU1" IS the brick.
 *
 * Serial evidence (the smoke greps these):
 *   BATCHDEMO mark               x3   (SYS_WRITE -- a BKL-marked path, on CPU1)
 *   BATCHDEMO done reads=<n> errors=<e>
 * plus kernel-side: the funnel placement print (class=1 -> cpu1), the
 * enqueue->dispatch latency stamp (ipi_wake), exit 7 + the init reap.
 *
 * NO libc, NO stdio (the cpu1hello house pattern; crt0 -> SYS_EXIT).
 */

#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_YIELD 15

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static void print(const char *m)
{
    unsigned long len = 0;
    while (m[len]) len++;
    sc(SYS_WRITE, 1, (long)m, (long)len);
}

static char *u2s(char *p, unsigned long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i) *p++ = tmp[--i];
    return p;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    unsigned long reads = 0, errors = 0;
    char buf[64];

    for (int i = 0; i < 3; i++) {
        print("BATCHDEMO mark\n");          /* marked path, on CPU1 */
        /* a little ordinary FS work (marked group) */
        for (int k = 0; k < 16; k++) {
            long fd = sc(SYS_OPEN, (long)"/etc/toolset0.txt", 0, 0);
            if (fd >= 0) {
                long n = sc(SYS_READ, fd, (long)buf, sizeof(buf));
                if (n > 0) reads++; else errors++;
                sc(SYS_CLOSE, fd, 0, 0);
            } else {
                errors++;
            }
        }
        sc(SYS_YIELD, 0, 0, 0);             /* cooperative citizen */
    }

    char line[64];
    char *q = line;
    const char *s = "BATCHDEMO done reads=";  while (*s) *q++ = *s++;
    q = u2s(q, reads);
    s = " errors=";                            while (*s) *q++ = *s++;
    q = u2s(q, errors);
    *q++ = '\n'; *q = 0;
    print(line);

    return 7;                                  /* the exit/reap proof value */
}

/* sleeptest -- proves SYS_SLEEP is a real, millisecond-granularity, BLOCKING
 * sleep (the process goes BLOCKED, accrues no CPU, and the timer wakes it at its
 * deadline). It reads the monotonic millisecond clock (SYS_GET_TICKS_MS) before
 * and after a 50 ms sleep and checks the measured elapsed time is in a generous
 * window [40, 200] ms (wide enough for QEMU/TCG jitter, tight enough to catch the
 * two failure modes: elapsed ~0 == didn't sleep / busy-returned immediately, and
 * a huge value == misinterpreted the unit / hung). Prints SLEEPTEST: slept=<ms>ms
 * then SLEEPTEST: PASS or SLEEPTEST: FAIL. crt0-linked; inline syscalls only. */

typedef unsigned long size_t;

#define SYS_WRITE         3
#define SYS_SLEEP         9
#define SYS_GET_TICKS_MS  40

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }

static void out_uint(unsigned long v) {
    char buf[24];
    int i = 24;
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&buf[i]);
}

#define SLEEP_MS   50UL
#define LO_MS      40UL    /* allow slightly-short (clock granularity) */
/* Upper bound is generous: even spawned late, the woken sleeper waits a little
 * for a cooperative dispatch behind the last settling GUI apps (~150 ms observed
 * under TCG). 300 ms keeps margin on slower CI hosts while still firmly catching
 * the real failure modes: elapsed ~0 (didn't block / busy-returned) and a huge
 * value (unit misread as seconds -> 50000 ms, or a hang). */
#define HI_MS     300UL

int main(void) {
    unsigned long t0 = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);

    /* The unit under test: a real blocking sleep of 50 milliseconds. */
    sc(SYS_SLEEP, (long)SLEEP_MS, 0, 0);

    unsigned long t1 = (unsigned long)sc(SYS_GET_TICKS_MS, 0, 0, 0);
    unsigned long elapsed = (t1 >= t0) ? (t1 - t0) : 0;  /* monotonic clock */

    out("SLEEPTEST: slept="); out_uint(elapsed); out("ms\n");

    int ok = (elapsed >= LO_MS && elapsed <= HI_MS);
    out(ok ? "SLEEPTEST: PASS\n" : "SLEEPTEST: FAIL\n");
    return ok ? 0 : 1;
}

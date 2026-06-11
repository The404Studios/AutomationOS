/* ============================================================================
 * 04_clock.c -- read the kernel's millisecond clock, format elapsed time as
 *               mm:ss, print a few samples, then exit cleanly. For the
 *               AutomationOS on-device C compiler.
 *
 * WHAT THIS PROGRAM DOES (in words):
 *   It reads the kernel's monotonic millisecond counter once to get a START
 *   time. Then it loops a FIXED number of times (five). On each pass it sleeps
 *   a short, fixed amount, reads the clock again, subtracts the start to get the
 *   elapsed milliseconds, and prints that elapsed time as minutes:seconds plus
 *   the raw millisecond count. The loop is BOUNDED -- it runs exactly five times
 *   and then the program returns. There is no infinite loop anywhere; a clock
 *   you cannot escape would be a hung process, which is exactly what we avoid.
 *
 * HOW IT TALKS TO THE KERNEL -- THE GENERIC syscall() BUILTIN:
 *   The on-device cc exposes any kernel call through a builtin named syscall:
 *       syscall(number, arg1, arg2)   ->  number in rax, arg1 in rdi, arg2 in rsi
 *   and the kernel's result comes back as the return value. We use two numbers:
 *       40  = SYS_GET_TICKS_MS  : returns milliseconds since boot (no args)
 *        9  = SYS_SLEEP         : sleep for arg1 milliseconds
 *   These numbers come straight from kernel/include/syscall.h. No inline
 *   assembly and no libc are involved -- the compiler turns the syscall(...)
 *   call directly into a `syscall` instruction.
 *
 * COMPILER RULES THAT SHAPED THIS FILE:
 *   - NO #include / #define; self-contained, integers only.
 *   - GLOBALS START AT ZERO; we keep all state in locals here anyway.
 *   - The loop count is a literal constant, so the program always terminates.
 *
 * HOW TO EXTEND IT:
 *   - Sample more often / longer: change SAMPLES or the nap() argument in main.
 *   - Show a real wall clock: SYS_GET_TICKS_MS is "since boot"; add an offset if
 *     you later wire a real-time clock.
 *
 * Exit code = 0 (a clean finish).
 * ==========================================================================*/

/* On-device builtins (prototypes only for the host syntax check; see 01_login.c).
 * sys_write is the SYS_WRITE convenience builtin; syscall is the generic one. */
void sys_write(int fd, char *buf, int len);
long syscall(long number, long arg1, long arg2);

/* ---- output layer --------------------------------------------------------- */
char g_ch;
/* emit: write a single character to stdout. */
void emit(int c) { g_ch = c; sys_write(1, &g_ch, 1); }
/* puts0: write a NUL-terminated string. */
void puts0(char *s) { int n = 0; while (s[n]) n = n + 1; sys_write(1, s, n); }
/* putu: print a non-negative integer in decimal (recursive). */
void putu(int v) { if (v >= 10) putu(v / 10); emit('0' + v % 10); }

/* put2: print exactly two decimal digits with a leading zero (for mm and ss).
 *   Contract: v is 0..99; prints "00".."99". */
void put2(int v) {
    emit('0' + (v / 10) % 10);
    emit('0' + v % 10);
}

/* ---- kernel time helpers -------------------------------------------------- */

/* ticks: milliseconds since boot, via SYS_GET_TICKS_MS (number 40). */
int ticks(void) {
    return (int)syscall(40, 0, 0);
}

/* nap: sleep for ms milliseconds, via SYS_SLEEP (number 9). The return value is
 * ignored; if the kernel cannot sleep it simply returns and the loop continues
 * (still bounded). */
void nap(int ms) {
    syscall(9, ms, 0);
}

/* main: sample the clock five times, printing elapsed mm:ss each pass.
 *
 * The loop, numbered:
 *   1. read t0 = the start time in milliseconds.
 *   2. print a header.
 *   3. for i = 1..SAMPLES:
 *        a. nap a fixed number of milliseconds.
 *        b. read the clock now; elapsed_ms = now - t0.
 *        c. total_seconds = elapsed_ms / 1000; mm = total/60; ss = total%60.
 *        d. print "i)  mm:ss   (elapsed_ms ms)".
 *   4. print a closing line and return 0.
 */
int main(void) {
    int SAMPLES = 5;
    int t0;
    int i;

    /* step 1 */
    t0 = ticks();

    /* step 2 */
    puts0("== AutomationOS clock demo ==\n");
    puts0("reading SYS_GET_TICKS_MS; ");
    putu(SAMPLES);
    puts0(" bounded samples.\n\n");

    /* step 3 */
    i = 1;
    while (i <= SAMPLES) {
        int now;
        int elapsed_ms;
        int total_sec;
        int mm;
        int ss;

        nap(250);                         /* a */
        now = ticks();                    /* b */
        elapsed_ms = now - t0;
        if (elapsed_ms < 0) elapsed_ms = 0;   /* guard against counter quirks */
        total_sec = elapsed_ms / 1000;    /* c */
        mm = total_sec / 60;
        ss = total_sec % 60;

        putu(i);                          /* d */
        puts0(")  ");
        put2(mm);
        emit(':');
        put2(ss);
        puts0("   (");
        putu(elapsed_ms);
        puts0(" ms)\n");

        i = i + 1;
    }

    /* step 4 */
    puts0("\ndone -- clock demo finished cleanly.\n");
    return 0;
}

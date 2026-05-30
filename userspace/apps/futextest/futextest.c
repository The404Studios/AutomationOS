/*
 * futextest.c — Freestanding ring-3 test for SYS_FUTEX (syscall 70)
 *
 * Build flags (no fs:0x28 canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * ABI tested (kernel/core/syscall/futex.c):
 *   sys_futex(uaddr, op, val, timeout, uaddr2, val3)
 *     FUTEX_WAIT (0): if *uaddr != val  → returns EAGAIN (-11) immediately
 *                     if *uaddr == val  → blocks (NOT tested here)
 *     FUTEX_WAKE (1): wakes up to val waiters; returns count woken (0 if none)
 *     FUTEX_WAIT_TIMEOUT (2): not implemented → returns ENOTSUP (-95)
 *     unknown op:     returns EINVAL (-22)
 *     misaligned uaddr: returns EINVAL (-22) from futex_validate_addr
 *
 * All paths exercised here return immediately — no blocking risk.
 */

/* ---- minimal types (no standard headers) -------------------------------- */
typedef unsigned long long  uint64_t;
typedef long long           int64_t;
typedef unsigned int        uint32_t;
typedef unsigned char       uint8_t;

/* ---- syscall numbers ---------------------------------------------------- */
#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_YIELD   15
#define SYS_FUTEX   70

/* ---- futex op codes (from futex.c) -------------------------------------- */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_WAIT_TIMEOUT  2

/* ---- errno values (from kernel/include/errno.h, negative) --------------- */
#define EAGAIN    (-11)
#define EINVAL    (-22)
#define ENOTSUP   (-95)

/* ---- 6-argument syscall wrapper ----------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile(
        "syscall"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return r;
}

/* ---- minimal string helpers --------------------------------------------- */
static uint64_t slen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_fd(const char *s)
{
    sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0, 0);
}

/* decimal integer → fixed buffer; returns pointer into buf */
static const char *itoa_buf(int64_t v, char *buf, int bufsz)
{
    char *end = buf + bufsz - 1;
    *end = '\0';
    char *p = end;
    int neg = (v < 0);
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    if (u == 0) {
        *--p = '0';
    } else {
        while (u) {
            *--p = '0' + (int)(u % 10);
            u /= 10;
        }
    }
    if (neg) *--p = '-';
    return p;
}

static void puts_long(int64_t v)
{
    char buf[32];
    puts_fd(itoa_buf(v, buf, sizeof buf));
}

/* ---- test state --------------------------------------------------------- */
static int g_pass;
static int g_fail;

static void check(const char *label, int64_t got, int64_t want)
{
    puts_fd("[FUTEXTEST]   ");
    puts_fd(label);
    puts_fd(": got=");
    puts_long(got);
    puts_fd(" want=");
    puts_long(want);
    if (got == want) {
        puts_fd(" => OK\n");
        g_pass++;
    } else {
        puts_fd(" => MISMATCH\n");
        g_fail++;
    }
}

/* ---- futex word --------------------------------------------------------- */
/*
 * Must be 4-byte aligned; declared volatile so the compiler does not cache
 * the value and so the kernel's atomic load sees the real memory word.
 */
static volatile uint32_t futex_word;

/* ---- test implementations ----------------------------------------------- */

/*
 * T1: FUTEX_WAIT with mismatched value.
 * Set futex_word = 0, then call FUTEX_WAIT with val = 1.
 * The kernel reads *uaddr (== 0), sees 0 != 1, and returns EAGAIN immediately.
 * Expected: EAGAIN (-11).
 */
static void test_wait_mismatch(void)
{
    puts_fd("[FUTEXTEST] T1: FUTEX_WAIT value-mismatch (should return EAGAIN)\n");
    futex_word = 0;
    /* op=FUTEX_WAIT=0, val=1 — does not match *uaddr which is 0 */
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT, 1, 0, 0);
    check("FUTEX_WAIT(word=0, val=1)", r, EAGAIN);
}

/*
 * T2: FUTEX_WAIT with mismatched value (lock-held scenario).
 * Set futex_word = 1, call FUTEX_WAIT with val = 2 (wrong expected value).
 * Expected: EAGAIN (-11).
 */
static void test_wait_mismatch2(void)
{
    puts_fd("[FUTEXTEST] T2: FUTEX_WAIT value-mismatch variant 2 (word=1 val=2)\n");
    futex_word = 1;
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT, 2, 0, 0);
    check("FUTEX_WAIT(word=1, val=2)", r, EAGAIN);
    futex_word = 0; /* reset */
}

/*
 * T3: FUTEX_WAKE with no waiters.
 * No process is sleeping on this address, so the kernel wakes 0 waiters.
 * Expected: 0.
 */
static void test_wake_no_waiters(void)
{
    puts_fd("[FUTEXTEST] T3: FUTEX_WAKE with no waiters (should return 0)\n");
    futex_word = 0;
    /* val=1 means "wake up to 1 waiter" — there are none */
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE, 1, 0, 0);
    check("FUTEX_WAKE(no waiters, val=1)", r, 0);
}

/*
 * T4: FUTEX_WAKE with val=0 (wake zero waiters).
 * The kernel loop runs 0 iterations and returns 0.
 * Expected: 0.
 */
static void test_wake_zero(void)
{
    puts_fd("[FUTEXTEST] T4: FUTEX_WAKE with val=0 (wake zero waiters)\n");
    futex_word = 0;
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE, 0, 0, 0);
    check("FUTEX_WAKE(val=0)", r, 0);
}

/*
 * T5: FUTEX_WAIT_TIMEOUT (op=2) — not yet implemented.
 * The kernel prints a warning and returns ENOTSUP (-95).
 * Expected: ENOTSUP (-95).
 */
static void test_wait_timeout_unimpl(void)
{
    puts_fd("[FUTEXTEST] T5: FUTEX_WAIT_TIMEOUT (unimplemented, should return ENOTSUP)\n");
    futex_word = 0;
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_TIMEOUT, 0, 0, 0);
    check("FUTEX_WAIT_TIMEOUT", r, ENOTSUP);
}

/*
 * T6: Unknown op code.
 * The kernel's switch falls to default and returns EINVAL (-22).
 * Expected: EINVAL (-22).
 */
static void test_invalid_op(void)
{
    puts_fd("[FUTEXTEST] T6: invalid op=99 (should return EINVAL)\n");
    futex_word = 0;
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, 99, 0, 0, 0);
    check("FUTEX_op=99", r, EINVAL);
}

/*
 * T7: Misaligned address (not 4-byte aligned).
 * futex_validate_addr() rejects it and returns EINVAL (-22).
 * Take the address of futex_word and add 1 to guarantee misalignment.
 * Expected: EINVAL (-22).
 */
static void test_misaligned_addr(void)
{
    puts_fd("[FUTEXTEST] T7: misaligned uaddr (should return EINVAL)\n");
    /* futex_word is uint32_t-aligned; +1 makes it unaligned */
    volatile uint8_t *misaligned = (volatile uint8_t *)&futex_word + 1;
    int64_t r = sc(SYS_FUTEX, (long)misaligned, FUTEX_WAIT, 0, 0, 0);
    check("FUTEX_WAIT(misaligned)", r, EINVAL);
}

/*
 * T8: FUTEX_WAKE broadcast: val=INT32_MAX triggers wq_wake_all().
 * With no waiters it still returns 0.
 * Expected: 0.
 */
static void test_wake_broadcast(void)
{
    puts_fd("[FUTEXTEST] T8: FUTEX_WAKE broadcast (INT32_MAX, no waiters)\n");
    futex_word = 0;
    /* INT32_MAX = 0x7fffffff; kernel checks nr_wake == INT32_MAX for wq_wake_all */
    int64_t r = sc(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE, 0x7fffffff, 0, 0);
    check("FUTEX_WAKE(INT32_MAX, no waiters)", r, 0);
}

/* ---- entry point -------------------------------------------------------- */
void _start(void)
{
    g_pass = 0;
    g_fail = 0;

    puts_fd("[FUTEXTEST] SYS_FUTEX=70 non-blocking path exerciser\n");
    puts_fd("[FUTEXTEST] ==========================================\n");

    test_wait_mismatch();
    test_wait_mismatch2();
    test_wake_no_waiters();
    test_wake_zero();
    test_wait_timeout_unimpl();
    test_invalid_op();
    test_misaligned_addr();
    test_wake_broadcast();

    puts_fd("[FUTEXTEST] ------------------------------------------\n");
    puts_fd("[FUTEXTEST] passed=");
    puts_long(g_pass);
    puts_fd("  failed=");
    puts_long(g_fail);
    puts_fd("\n");

    if (g_fail == 0) {
        puts_fd("FUTEXTEST: PASS\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0);
    } else {
        puts_fd("FUTEXTEST: FAIL unexpected return values\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0);
    }

    /* unreachable — silence the compiler */
    while (1) {}
}

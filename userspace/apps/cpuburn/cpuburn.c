/* cpuburn -- a freestanding CPU burner that NEVER yields. It exists to prove the
 * GATED preemptive scheduler (PREEMPT=1 build) actually time-slices userspace: a
 * process that loops forever without ever calling SYS_YIELD/sleep/block can only
 * share the CPU if the TIMER preempts it. On the cooperative kernel only the
 * FIRST such burner would ever run (it never returns control); on the preemptive
 * kernel all of them round-robin -- that's the whole point of the stress test.
 *
 * The loop mixes INTEGER work (an accumulating counter + a cheap LCG) with
 * FLOAT/SSE work (a running 2x2 float matmul + a dot-product reduction) so every
 * preemptive context switch must correctly save/restore the XMM register file as
 * well as the GP registers. Inputs feed through `volatile` so the compiler can't
 * fold the math away. Every ~40M iterations it emits ONE heartbeat line via
 * SYS_WRITE(3); heartbeats are periodic, not spammy, so distinct ids interleaving
 * in the serial log is direct evidence of fair preemptive scheduling.
 *
 * Modeled on argvtest.c: crt0-linked main(argc,argv), inline syscalls only. */

/*
 * Two gcc-15 quirks are neutralised at TU scope (same as asteroids.c et al.):
 *  - "no-stack-protector": Arch's gcc self-spec re-injects -fstack-protector-strong
 *    AFTER the build's single -fno-stack-protector for TUs with local arrays whose
 *    address escapes (our out_uint's buf, main's matmul arrays). That emits a
 *    %fs:0x28 canary referencing __stack_chk_fail, which does NOT exist in this
 *    freestanding ring-3 link (link error + trips the build's fs:0x28 gate).
 *  - "no-tree-loop-distribute-patterns": at -O2 gcc can rewrite the hand-rolled
 *    slen() byte loop into a call to libc strlen, which doesn't exist freestanding.
 * Disabling both at file scope keeps every function self-contained.
 */
#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

typedef unsigned long size_t;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(3 /*SYS_WRITE*/, 1, (long)s, (long)slen(s)); }

/* Emit an unsigned decimal directly to fd 3, one digit at a time. Avoids any
 * caller-supplied stack buffer (which makes -fstack-protector-strong emit a
 * __stack_chk_fail call that won't link in this freestanding, nostdlib build).
 * A local fixed array written by index, like floattest's out_uint, stays
 * canary-clean here. */
static void out_uint(unsigned long v) {
    char buf[24];
    int i = 24;
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&buf[i]);
}

int main(int argc, char** argv) {
    /* id from argv[1] (spawn_args("sbin/cpuburn","0")); default "?" if absent. */
    const char* id = (argc >= 2 && argv && argv[1] && argv[1][0]) ? argv[1] : "?";

    out("[CPUBURN] starting id=");
    out(id);
    out("\n");

    /* volatile seeds => the SSE/int work is genuinely computed at runtime and
     * cannot be constant-folded out of the loop. */
    volatile float Vseed[4] = {1.5f, 2.5f, 3.5f, 4.5f};

    unsigned long acc   = 0;           /* integer accumulator                 */
    unsigned long lcg   = 0x9E3779B1u; /* cheap integer churn (LCG)           */
    unsigned long beats = 0;           /* heartbeat counter                   */

    /* running 2x2 float matmul state (the XMM-stressing part) */
    float A[2][2] = {{Vseed[0], Vseed[1]}, {Vseed[2], Vseed[3]}};
    float B[2][2] = {{Vseed[3], Vseed[2]}, {Vseed[1], Vseed[0]}};
    float facc    = 0.0f;              /* keeps a float live across switches   */

    /* Iterations between heartbeats. Tuned so that under the PREEMPTIVE kernel on
     * QEMU -- where the CPU is emulated (slow) AND time-sliced across 6 burners --
     * each burner still emits a heartbeat every couple of seconds: frequent enough
     * to PROVE fair interleaving within a bounded boot window, but not spammy.
     * (On real hardware this is sub-millisecond; the stress value is fairness, not
     * throughput.) */
    const unsigned long PERIOD = 1500000UL;
    unsigned long i = 0;

    for (;;) {              /* FOREVER -- never yields, never blocks. */
        /* --- integer work --- */
        lcg = lcg * 6364136223846793005UL + 1442695040888963407UL;
        acc += (lcg >> 33) ^ i;

        /* --- float/SSE work: C = A*B, then renormalize + reduce into facc --- */
        float C[2][2];
        for (int r = 0; r < 2; r++)
            for (int cc = 0; cc < 2; cc++) {
                float s = 0.0f;
                for (int k = 0; k < 2; k++) s += A[r][k] * B[k][cc];
                C[r][cc] = s;
            }
        /* dot-product style reduction across the result */
        float dot = C[0][0] * 0.5f + C[0][1] * 0.25f
                  + C[1][0] * 0.125f + C[1][1] * 0.0625f;
        /* Fold dot into facc but KEEP facc BOUNDED every iteration. A naive
         * `facc += dot` grows without limit and eventually becomes +Inf; then any
         * "while (n >= 100) n -= 100" renormalize loop SPINS FOREVER (Inf-100==Inf)
         * and the burner wedges at one instruction. So we wrap with a SINGLE
         * bounded subtraction (no unbounded loop, no Inf) -- facc stays in [0,100)
         * while still carrying a real cross-iteration float dependency. */
        facc += dot;
        if (facc >= 100.0f) facc -= 100.0f;  /* single step, never Inf/NaN */
        /* Rescale A from the bounded facc: still depends on prior iterations
         * (genuine cross-switch XMM state) but provably finite forever. */
        A[0][0] = 1.0f + facc * 0.01f;       /* in [1,2)          */
        A[1][1] = 2.0f - facc * 0.01f;       /* in (1,2]          */

        i++;
        if (i >= PERIOD) {
            beats++;
            /* one heartbeat line:
             * [CPUBURN id=<id>] beat <n> acc=<int> f=<intpart-of-float>
             * NOTE: emitted as a sequence of small writes; since each write is a
             * single syscall this is the per-burner heartbeat (once per PERIOD
             * iterations, so not spammy), and distinct ids interleaving in the
             * serial log is the fairness evidence we're after. */
            out("[CPUBURN id=");
            out(id);
            out("] beat ");
            out_uint(beats);
            out(" acc=");
            out_uint(acc);
            out(" f=");
            out_uint((unsigned long)facc);   /* integer part of running float */
            out("\n");
            i = 0;
        }
    }
    /* unreachable */
    return 0;
}

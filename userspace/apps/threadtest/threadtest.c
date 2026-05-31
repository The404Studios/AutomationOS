// threadtest -- verifies REAL THREADS: schedulable tasks that SHARE the parent's
// address space (CR3) but have independent kernel/user stacks, registers, and FPU
// state. Self-contained (no libs): talks to the kernel directly via `syscall`,
// prints to fd 1 (serial), exits. The smoke harness greps for "THREADTEST: PASS".
//
// THE THREE PROOFS (each would FAIL under a broken thread implementation):
//
//   (1) SHARED MEMORY: all 4 threads atomically add a known amount to ONE shared
//       global counter. If threads did NOT share the address space, each would
//       mutate a private copy and the final sum would be wrong. PASS requires
//       counter == 4 * PER_THREAD_ADD.
//
//   (2) INDEPENDENT STACKS: each thread computes its result into a STACK-LOCAL
//       variable first (with a yield in between to force interleaving), then
//       publishes it into the SHARED result[idx] slot. If the threads aliased a
//       single stack, the locals would clobber each other and the published
//       indices would collide -> result[i] != i for some i.
//
//   (3) INDEPENDENT FPU: each thread does an SSE float computation into a stack
//       local across a yield (forcing >=1 context switch mid-computation), then
//       stores it into shared result_f[idx]. If FPU/XMM state were NOT saved and
//       restored per thread on context switch, the interleaved float math would
//       corrupt across threads -> result_f[i] would not match its expected value.

typedef unsigned long size_t;

#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { syscall3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void out_num(long v) {
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i++] = '-';
    char r[24]; int j = 0;
    while (i > 0) r[j++] = b[--i];
    r[j] = '\0';
    out(r);
}

// Thread wrappers (inline; no libc link).
static int thread_create(void (*entry)(void*), void* arg, void* stack_top) {
    return (int)syscall3(SYS_THREAD_CREATE, (long)entry, (long)arg, (long)stack_top);
}
static void thread_exit(int rv) { syscall3(SYS_THREAD_EXIT, rv, 0, 0); for (;;) {} }
static int thread_join(int tid, int* rv) {
    return (int)syscall3(SYS_THREAD_JOIN, tid, (long)rv, 0);
}
static void yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }

// ── SHARED state (lives in the one address space all threads share) ─────────
#define NTHREADS      4
#define PER_THREAD_ADD 1000

// (1) shared counter: each thread atomically adds PER_THREAD_ADD.
static volatile long shared_counter = 0;

// (2) shared result slots: each thread writes its index here.
static volatile int  result[NTHREADS];

// (3) shared float results: each thread writes an SSE-computed float here.
static volatile float result_f[NTHREADS];

// Per-thread user stacks: static 16-aligned arrays (one per thread). 16 KiB each
// is plenty for this tiny entry. Aligned so the kernel's 16-align of the TOP
// keeps us aligned. These are DISTINCT regions -> independent stacks.
#define TSTACK_SIZE (16 * 1024)
static __attribute__((aligned(16))) unsigned char tstack[NTHREADS][TSTACK_SIZE];

// The expected float for thread i: a deliberately fiddly SSE computation that an
// independent scalar reference can reproduce. f(i) = ((i+1)*1.5 + 2.25) * 3.0.
static float expected_f(int i) {
    float x = (float)(i + 1) * 1.5f;
    x = x + 2.25f;
    x = x * 3.0f;
    return x;
}

// Thread entry. arg encodes the thread index (0..3). Each thread:
//   - atomically bumps the shared counter (proof 1),
//   - computes its index into a STACK local across a yield, then publishes it
//     into result[idx] (proof 2),
//   - computes its float into a STACK local across a yield, then publishes it
//     into result_f[idx] (proof 3),
//   - terminates via thread_exit(idx).
static void worker(void* arg) {
    int idx = (int)(long)arg;

    // (1) shared memory: atomic add to the shared global counter.
    __atomic_add_fetch(&shared_counter, PER_THREAD_ADD, __ATOMIC_SEQ_CST);

    // (2) independent stack: keep the index in a stack local, yield (forcing the
    // scheduler to interleave other threads that are doing the SAME with THEIR
    // own index), then publish. If stacks aliased, `my_index` would be clobbered
    // by a sibling between the store and the publish, and result[idx] would be
    // wrong / collide.
    volatile int my_index = idx;
    yield();
    yield();
    result[idx] = my_index;

    // (3) independent FPU: compute the float in a stack local (XMM registers),
    // yield mid-way to force >=1 context switch (which must save/restore XMM),
    // finish the computation, then publish. Corrupted FPU save/restore across the
    // switch would make result_f[idx] wrong.
    volatile float acc = (float)(idx + 1) * 1.5f;
    yield();
    acc = acc + 2.25f;
    yield();
    acc = acc * 3.0f;
    result_f[idx] = acc;

    thread_exit(idx);
}

static int feq(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 0.001f;
}

void _start(void) {
    out("THREADTEST: start (4 threads; shared-mem + independent-stack + independent-FPU)\n");

    for (int i = 0; i < NTHREADS; i++) {
        result[i] = -1;
        result_f[i] = -1.0f;
    }

    int tids[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        // Pass the TOP of each thread's distinct stack (it grows down).
        void* stack_top = (void*)(tstack[i] + TSTACK_SIZE);
        tids[i] = thread_create(worker, (void*)(long)i, stack_top);
        if (tids[i] <= 0) {
            out("THREADTEST: FAIL(thread_create returned ");
            out_num(tids[i]);
            out(" for idx ");
            out_num(i);
            out(")\n");
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) {}
        }
    }

    // Join all 4 threads, collecting each retval (which should equal its index).
    for (int i = 0; i < NTHREADS; i++) {
        int rv = -777;
        int jr = thread_join(tids[i], &rv);
        if (jr != 0) {
            out("THREADTEST: FAIL(thread_join idx ");
            out_num(i); out(" -> "); out_num(jr); out(")\n");
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) {}
        }
        if (rv != i) {
            out("THREADTEST: FAIL(join retval idx ");
            out_num(i); out(" got "); out_num(rv); out(")\n");
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) {}
        }
    }

    // ── Verify the three proofs ─────────────────────────────────────────────
    // Proof 1: shared counter accumulated every thread's contribution.
    long expect_counter = (long)NTHREADS * PER_THREAD_ADD;
    if (shared_counter != expect_counter) {
        out("THREADTEST: FAIL(shared counter = ");
        out_num(shared_counter);
        out(", expected ");
        out_num(expect_counter);
        out(") -- shared memory broken\n");
        syscall3(SYS_EXIT, 1, 0, 0);
        for (;;) {}
    }

    // Proof 2: each result[i] == i (independent stacks, no clobber/collision).
    for (int i = 0; i < NTHREADS; i++) {
        if (result[i] != i) {
            out("THREADTEST: FAIL(result[");
            out_num(i); out("] = "); out_num(result[i]);
            out(", expected "); out_num(i);
            out(") -- stacks aliased\n");
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) {}
        }
    }

    // Proof 3: each result_f[i] matches its expected SSE value (independent FPU).
    for (int i = 0; i < NTHREADS; i++) {
        if (!feq(result_f[i], expected_f(i))) {
            out("THREADTEST: FAIL(result_f[");
            out_num(i); out("] mismatch idx "); out_num(i);
            out(") -- FPU not isolated across context switches\n");
            syscall3(SYS_EXIT, 1, 0, 0);
            for (;;) {}
        }
    }

    out("THREADTEST: shared_counter="); out_num(shared_counter);
    out(" result=[");
    for (int i = 0; i < NTHREADS; i++) { out_num(result[i]); if (i < NTHREADS-1) out(","); }
    out("] fpu-ok\n");
    out("THREADTEST: PASS (shared-mem + independent-stack + independent-FPU, 4 threads)\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}

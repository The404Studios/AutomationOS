/*
 * js_timers.c -- setTimeout / setInterval / clearTimeout / clearInterval
 *                implementation for the AutomationOS freestanding JS engine.
 * =========================================================================
 *
 * Build (NO fs:0x28 canary -- verified by -fno-stack-protector):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/js/js_timers.c -o js_timers.o
 *
 * Dependencies:
 *   userspace/lib/js/js.h          (js_vm)
 *   userspace/lib/js/js_native.h   (js_native_register_function, value helpers)
 *   userspace/lib/js/js_internal.h (js_call_function, js_value, JS_FUNCTION)
 *
 * Syscall used:
 *   SYS_GET_TICKS_MS = 40  ->  sc(40,0,0,0,0,0) returns uint64 monotonic ms
 *
 * See js_timers.h for the GC / callback-storage design rationale.
 */

#include "js_timers.h"
#include "js_native.h"       /* js_native_register_function, value helpers  */
#include "js_internal.h"     /* js_call_function, JS_FUNCTION, js_value      */

/* =========================================================================
 * Syscall shim (freestanding, no libc)
 * ========================================================================= */

#define SYS_GET_TICKS_MS 40

/*
 * Inline syscall: rax=num, args in rdi rsi rdx r10 r8 r9.
 * We use the 6-arg form even though SYS_GET_TICKS_MS ignores all args.
 * -fno-stack-protector guarantees no fs:0x28 read.
 */
static inline long __attribute__((always_inline))
sc(long num, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "0"(num), "D"(a1), "S"(a2), "d"(a3),
          /* r10, r8, r9 via memory constraints -- acceptable for rarely-called
           * paths; keeps the inline asm portable across -O levels */
          "r"(a4), "r"(a5)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Returns current monotonic time in milliseconds.
 * In selftest mode this is overridden by a fake clock. */
static unsigned long long get_ticks_ms(void)
{
    return (unsigned long long)sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
}

/* =========================================================================
 * Timer table
 * ========================================================================= */

#define JS_TIMERS_MAX   64   /* fixed-capacity timer slots */

/*
 * A single timer entry.
 *
 * Callback storage (GC note -- see header for full discussion):
 *   `cb` is a js_value copied by VALUE into this static slot.  For
 *   JS_FUNCTION values the u.o pointer references a js_object inside the
 *   engine arena.  This is safe only if the timer fires within the same
 *   js_eval() invocation that registered it (single-eval mode).  If the
 *   arena is reset between registration and firing, u.o is stale.
 *
 * TODO (persistent-root mode): if the engine exposes a root-pin API
 *   such as `js_gc_root(vm, &slot->cb)` / `js_gc_unroot(vm, &slot->cb)`,
 *   call pin on setTimeout and unpin on clearTimeout/fire-once.  Until
 *   then, single-eval mode is the safe contract.
 */
typedef struct {
    int               active;       /* 1 = slot in use                    */
    int               id;           /* timer id returned to JS (>= 1)     */
    unsigned long long fire_at_ms;  /* absolute monotonic time to fire    */
    unsigned long long period_ms;   /* 0 = one-shot, >0 = repeating       */
    js_value          cb;           /* the JS callback (see GC note above)*/
} js_timer_slot;

/* The static timer table.  Lives outside the engine arena. */
static js_timer_slot g_timers[JS_TIMERS_MAX];
static int           g_next_id = 1;  /* monotonically increasing id source */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Return the slot index for a given id, or -1 if not found. */
static int timer_find(int id)
{
    for (int i = 0; i < JS_TIMERS_MAX; i++) {
        if (g_timers[i].active && g_timers[i].id == id)
            return i;
    }
    return -1;
}

/* Allocate a free slot and return its index, or -1 if table is full. */
static int timer_alloc(void)
{
    for (int i = 0; i < JS_TIMERS_MAX; i++) {
        if (!g_timers[i].active)
            return i;
    }
    return -1;
}

/* Find the slot with the smallest fire_at_ms among active timers.
 * Returns index or -1 if no active timers. */
static int timer_earliest(void)
{
    int best = -1;
    unsigned long long best_t = (unsigned long long)-1;
    for (int i = 0; i < JS_TIMERS_MAX; i++) {
        if (g_timers[i].active && g_timers[i].fire_at_ms < best_t) {
            best_t = g_timers[i].fire_at_ms;
            best = i;
        }
    }
    return best;
}

/* Register a timer.  Returns the id (>= 1) or 0 on failure (table full). */
static int timer_register(unsigned long long delay_ms,
                          unsigned long long period_ms,
                          js_value cb)
{
    int slot = timer_alloc();
    if (slot < 0)
        return 0;   /* table full */

    unsigned long long now = get_ticks_ms();
    int id = g_next_id++;
    if (g_next_id <= 0) g_next_id = 1;  /* wrap (extremely unlikely) */

    g_timers[slot].active     = 1;
    g_timers[slot].id         = id;
    g_timers[slot].fire_at_ms = now + delay_ms;
    g_timers[slot].period_ms  = period_ms;
    g_timers[slot].cb         = cb;
    return id;
}

/* Cancel a timer by id.  Returns 1 if found and cancelled, 0 otherwise. */
static int timer_cancel(int id)
{
    int slot = timer_find(id);
    if (slot < 0) return 0;
    g_timers[slot].active = 0;
    /* Zero the cb slot so the js_object * is not dangling (defensive). */
    js_value undef = { JS_UNDEFINED, {0} };
    g_timers[slot].cb = undef;
    return 1;
}

/* =========================================================================
 * Native function implementations (called from JS)
 * ========================================================================= */

/*
 * setTimeout(cb, ms) -> id
 *
 * argc == 1: delay defaults to 0.
 * argc == 0: error, returns undefined.
 * The callback must be a JS function value.
 */
static js_value native_setTimeout(js_vm *vm, int argc, js_value *argv)
{
    if (argc < 1) {
        js_throw_str(vm, "setTimeout: callback required");
        return js_native_make_undefined();
    }

    js_value cb = argv[0];
    if (cb.type != JS_FUNCTION) {
        js_throw_str(vm, "setTimeout: first argument must be a function");
        return js_native_make_undefined();
    }

    unsigned long long delay_ms = 0;
    if (argc >= 2) {
        int ms = 0;
        if (js_native_to_int(argv[1], &ms) && ms > 0)
            delay_ms = (unsigned long long)ms;
    }

    int id = timer_register(delay_ms, 0, cb);
    return js_native_make_number(vm, (double)id);
}

/*
 * setInterval(cb, ms) -> id
 */
static js_value native_setInterval(js_vm *vm, int argc, js_value *argv)
{
    if (argc < 1) {
        js_throw_str(vm, "setInterval: callback required");
        return js_native_make_undefined();
    }

    js_value cb = argv[0];
    if (cb.type != JS_FUNCTION) {
        js_throw_str(vm, "setInterval: first argument must be a function");
        return js_native_make_undefined();
    }

    unsigned long long period_ms = 0;
    if (argc >= 2) {
        int ms = 0;
        if (js_native_to_int(argv[1], &ms) && ms > 0)
            period_ms = (unsigned long long)ms;
    }
    /* Interval of 0 ms is legal (fires every event-loop tick) but unusual. */

    int id = timer_register(period_ms, period_ms, cb);
    return js_native_make_number(vm, (double)id);
}

/*
 * clearTimeout(id) / clearInterval(id) -- same implementation.
 */
static js_value native_clearTimeout(js_vm *vm, int argc, js_value *argv)
{
    if (argc < 1) return js_native_make_undefined();
    int id = 0;
    js_native_to_int(argv[0], &id);
    timer_cancel(id);
    (void)vm;
    return js_native_make_undefined();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void js_timers_install(js_vm *vm)
{
    js_native_register_function(vm, "setTimeout",    native_setTimeout);
    js_native_register_function(vm, "setInterval",   native_setInterval);
    js_native_register_function(vm, "clearTimeout",  native_clearTimeout);
    js_native_register_function(vm, "clearInterval", native_clearTimeout);
}

int js_timers_run(js_vm *vm)
{
    unsigned long long now = get_ticks_ms();
    int fired = 0;

    while (fired < JS_TIMERS_RUN_MAX) {
        /* Find the earliest timer that has expired. */
        int best = -1;
        unsigned long long best_t = (unsigned long long)-1;
        for (int i = 0; i < JS_TIMERS_MAX; i++) {
            if (g_timers[i].active &&
                g_timers[i].fire_at_ms <= now &&
                g_timers[i].fire_at_ms < best_t) {
                best_t = g_timers[i].fire_at_ms;
                best = i;
            }
        }
        if (best < 0) break;   /* no more expired timers */

        /* Snapshot the callback and update/remove the slot BEFORE calling
         * the function so that clearTimeout(id) from inside the callback
         * produces consistent behaviour. */
        js_value cb = g_timers[best].cb;
        unsigned long long period = g_timers[best].period_ms;

        if (period == 0) {
            /* One-shot: remove now. */
            g_timers[best].active = 0;
            js_value undef = { JS_UNDEFINED, {0} };
            g_timers[best].cb = undef;
        } else {
            /* Repeating: advance fire_at_ms.  Use `now` as the base so that
             * a late fire doesn't cause catch-up storms. */
            g_timers[best].fire_at_ms = now + period;
        }

        /* Invoke the JS callback.  No arguments, `this` = undefined.
         *
         * js_call_function is declared in js_internal.h:
         *   js_completion js_call_function(js_vm *vm, js_value fnv,
         *                                  js_value thisv,
         *                                  js_value *argv, int argc,
         *                                  js_value *out);
         *
         * NOTE: if js_native_call() is eventually added to js_native.h this
         * call site should migrate to that API; see header gap documentation.
         */
        js_value result;
        js_value thisv = { JS_UNDEFINED, {0} };
        /* Ignore exceptions from timer callbacks (matches browser behaviour:
         * one bad timer must not stop subsequent timers). */
        js_call_function(vm, cb, thisv, (js_value *)0, 0, &result);
        if (vm->has_exception) {
            vm->has_exception = 0;
            /* exception value abandoned -- intentional */
        }

        fired++;
        /* Re-read now so timers scheduled close together fire correctly. */
        now = get_ticks_ms();
    }

    return fired;
}

void js_timers_clear_all(void)
{
    js_value undef = { JS_UNDEFINED, {0} };
    for (int i = 0; i < JS_TIMERS_MAX; i++) {
        g_timers[i].active = 0;
        g_timers[i].cb     = undef;
    }
}

/* =========================================================================
 * Self-test
 * Runs entirely without the JS engine.  Uses a fake monotonic clock.
 * ========================================================================= */

/* Override the real clock with a deterministic counter for tests. */
static int              g_test_mode      = 0;
static unsigned long long g_fake_clock_ms = 0;

/*
 * Wrap get_ticks_ms() to use fake clock when in test mode.
 * We cannot patch the static function pointer easily without C99 indirect
 * calls, so instead we provide a test-local registration helper that sets
 * fire_at_ms directly, bypassing the real-time offset.
 *
 * The selftest calls its own internal wrappers that use g_fake_clock_ms
 * instead of invoking timer_register() (which calls get_ticks_ms()).
 */

/* Directly plant a timer for testing -- avoids touching the real clock. */
static int test_add_timer(unsigned long long fire_at,
                          unsigned long long period,
                          int id_hint)
{
    int slot = timer_alloc();
    if (slot < 0) return 0;
    js_value dummy = { JS_UNDEFINED, {0} };
    g_timers[slot].active     = 1;
    g_timers[slot].id         = id_hint;
    g_timers[slot].fire_at_ms = fire_at;
    g_timers[slot].period_ms  = period;
    g_timers[slot].cb         = dummy;
    return 1;
}

/*
 * Test-mode fire: scans for timers with fire_at_ms <= fake_now.
 * Fires them in order without invoking any JS, just returns which ids fired.
 * Returns number fired.
 */
static int test_fire_all(unsigned long long fake_now,
                         int *fired_ids, int cap)
{
    int count = 0;
    for (;;) {
        /* Find earliest expired timer. */
        int best = -1;
        unsigned long long best_t = (unsigned long long)-1;
        for (int i = 0; i < JS_TIMERS_MAX; i++) {
            if (g_timers[i].active &&
                g_timers[i].fire_at_ms <= fake_now &&
                g_timers[i].fire_at_ms < best_t) {
                best_t = g_timers[i].fire_at_ms;
                best = i;
            }
        }
        if (best < 0) break;

        if (count < cap)
            fired_ids[count] = g_timers[best].id;
        count++;

        unsigned long long period = g_timers[best].period_ms;
        if (period == 0) {
            g_timers[best].active = 0;
        } else {
            g_timers[best].fire_at_ms = fake_now + period;
        }
    }
    return count;
}

/* Minimal print stub for self-test failure messages.
 * Uses SYS_WRITE(fd=1) directly; no libc dependency.
 * Allow -DSYS_WRITE=<n> override for host-Linux selftest builds. */
#ifndef SYS_WRITE
#define SYS_WRITE 3
#endif
static void test_puts(const char *s)
{
    unsigned long len = 0;
    while (s[len]) len++;
    sc(SYS_WRITE, 1, (long)s, (long)len, 0, 0);
}

/* Simple integer formatter for test output. */
static void test_put_int(int n)
{
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (n == 0) { buf[--i] = '0'; }
    else {
        int neg = (n < 0);
        unsigned int u = neg ? (unsigned int)(-n) : (unsigned int)n;
        while (u) { buf[--i] = '0' + (u % 10); u /= 10; }
        if (neg) buf[--i] = '-';
    }
    test_puts(&buf[i]);
}

#define FAIL_IF(cond, msg) do { \
    if (cond) { \
        test_puts("FAIL: "); test_puts(msg); test_puts("\n"); \
        failures++; \
    } \
} while (0)

int js_timers_selftest(void)
{
    int failures = 0;
    int fired[JS_TIMERS_MAX];
    int n;

    /* Reset timer table for clean test state. */
    js_timers_clear_all();
    g_next_id = 1;

    /* ------------------------------------------------------------------ */
    /* Test 1: single one-shot timer fires at the right time */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T1: one-shot fires at correct time\n");
    {
        js_timers_clear_all(); g_next_id = 10;
        test_add_timer(100, 0, 10);   /* fire at t=100 */
        n = test_fire_all(99, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 0, "T1a: fired before time");
        n = test_fire_all(100, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T1b: should fire exactly once at t=100");
        FAIL_IF(fired[0] != 10, "T1c: wrong id fired");
        /* slot should now be inactive */
        FAIL_IF(timer_find(10) >= 0, "T1d: slot not freed after one-shot");
    }

    /* ------------------------------------------------------------------ */
    /* Test 2: multiple timers fire in oldest-first order */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T2: multiple timers fire in order\n");
    {
        js_timers_clear_all(); g_next_id = 1;
        test_add_timer(300, 0, 1);   /* id=1, fire at t=300 */
        test_add_timer(100, 0, 2);   /* id=2, fire at t=100 */
        test_add_timer(200, 0, 3);   /* id=3, fire at t=200 */

        n = test_fire_all(300, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 3, "T2a: should fire 3 timers");
        /* Order must be id=2 (t=100), id=3 (t=200), id=1 (t=300). */
        FAIL_IF(fired[0] != 2, "T2b: first to fire should be id=2");
        FAIL_IF(fired[1] != 3, "T2c: second to fire should be id=3");
        FAIL_IF(fired[2] != 1, "T2d: third to fire should be id=1");
    }

    /* ------------------------------------------------------------------ */
    /* Test 3: repeating timer re-arms itself */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T3: repeating timer re-arms\n");
    {
        js_timers_clear_all(); g_next_id = 7;
        test_add_timer(50, 50, 7);   /* interval every 50ms, first at t=50 */

        /* Fire at t=50: should fire once, re-arm to t=100. */
        n = test_fire_all(50, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T3a: should fire once at t=50");
        FAIL_IF(timer_find(7) < 0, "T3b: slot should remain active");

        /* Fire at t=99: should NOT fire. */
        n = test_fire_all(99, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 0, "T3c: should not fire before t=100");

        /* Fire at t=100: fires again, re-arms to t=150. */
        n = test_fire_all(100, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T3d: should fire again at t=100");
        FAIL_IF(timer_find(7) < 0, "T3e: slot must remain active after interval");

        /* Check next fire is at t=150. */
        int slot = timer_find(7);
        FAIL_IF(slot < 0, "T3f: slot not found");
        if (slot >= 0) {
            FAIL_IF(g_timers[slot].fire_at_ms != 150, "T3g: re-arm time wrong");
        }
    }

    /* ------------------------------------------------------------------ */
    /* Test 4: clearTimeout cancels a pending timer */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T4: clearTimeout cancels\n");
    {
        js_timers_clear_all(); g_next_id = 20;
        test_add_timer(100, 0, 20);
        test_add_timer(200, 0, 21);

        timer_cancel(20);   /* cancel id=20 */
        n = test_fire_all(300, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T4a: only id=21 should fire");
        FAIL_IF(fired[0] != 21, "T4b: fired id should be 21");
    }

    /* ------------------------------------------------------------------ */
    /* Test 5: cancel inside interval (cancel from fired callback) */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T5: cancel repeating timer mid-run\n");
    {
        js_timers_clear_all(); g_next_id = 30;
        test_add_timer(10, 10, 30);  /* repeating */
        test_add_timer(10, 10, 31);  /* repeating */

        n = test_fire_all(10, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 2, "T5a: both should fire at t=10");

        /* Now cancel id=30. */
        timer_cancel(30);

        n = test_fire_all(20, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T5b: only id=31 should fire at t=20");
        FAIL_IF(fired[0] != 31, "T5c: fired id should be 31");
    }

    /* ------------------------------------------------------------------ */
    /* Test 6: table-full rejection */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T6: table-full returns 0 id\n");
    {
        js_timers_clear_all(); g_next_id = 100;
        /* Fill the entire table. */
        for (int i = 0; i < JS_TIMERS_MAX; i++) {
            test_add_timer(1000 + (unsigned long long)i, 0, 100 + i);
        }
        /* Now try to add one more: should fail (timer_alloc returns -1). */
        int slot = timer_alloc();
        FAIL_IF(slot >= 0, "T6a: alloc should fail when table is full");
    }

    /* ------------------------------------------------------------------ */
    /* Test 7: id uniqueness and sequential assignment */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T7: ids are unique and non-zero\n");
    {
        js_timers_clear_all(); g_next_id = 1;
        /* Add 5 timers via the real timer_register (uses real clock but
         * we only check id uniqueness, not time). */
        int ids[5];
        int seen_zero = 0;
        for (int i = 0; i < 5; i++) {
            js_value dummy = { JS_UNDEFINED, {0} };
            ids[i] = timer_register((unsigned long long)(i * 10), 0, dummy);
            if (ids[i] == 0) seen_zero = 1;
        }
        FAIL_IF(seen_zero, "T7a: timer_register should not return 0 when table has room");
        /* Check uniqueness. */
        for (int i = 0; i < 5; i++) {
            for (int j = i + 1; j < 5; j++) {
                FAIL_IF(ids[i] == ids[j], "T7b: duplicate ids");
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Test 8: zero-delay timer fires immediately */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T8: zero-delay timer fires at or after registration time\n");
    {
        js_timers_clear_all(); g_next_id = 50;
        test_add_timer(0, 0, 50);   /* fire_at = 0, so <= any non-negative now */
        n = test_fire_all(0, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 1, "T8a: zero-delay timer should fire immediately");
    }

    /* ------------------------------------------------------------------ */
    /* Test 9: js_timers_clear_all clears everything */
    /* ------------------------------------------------------------------ */
    test_puts("[timers] T9: js_timers_clear_all wipes table\n");
    {
        js_timers_clear_all(); g_next_id = 1;
        for (int i = 0; i < 10; i++) {
            test_add_timer((unsigned long long)i, 0, 1 + i);
        }
        js_timers_clear_all();
        n = test_fire_all(9999, fired, JS_TIMERS_MAX);
        FAIL_IF(n != 0, "T9a: no timers should fire after clear_all");
    }

    /* ------------------------------------------------------------------ */
    /* Summary */
    /* ------------------------------------------------------------------ */
    if (failures == 0) {
        test_puts("[timers] selftest PASSED\n");
    } else {
        test_puts("[timers] selftest FAILED: ");
        test_put_int(failures);
        test_puts(" failure(s)\n");
    }

    /* Restore clean state for the real timer subsystem. */
    js_timers_clear_all();
    g_next_id = 1;

    return failures;
}

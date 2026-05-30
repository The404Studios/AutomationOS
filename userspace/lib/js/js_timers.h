/*
 * js_timers.h -- setTimeout / setInterval / clearTimeout / clearInterval
 *                for the AutomationOS freestanding JS engine.
 * ======================================================================
 *
 * Build constraints (matches the rest of the JS engine):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *   NO fs:0x28 stack canary.
 *
 * ------------------------------------------------------------------
 *  Browser / event-loop contract
 * ------------------------------------------------------------------
 *  1. Embed calls js_timers_install(vm) once, after js_new() and
 *     js_native_register_function() are available.  This wires
 *     setTimeout / setInterval / clearTimeout / clearInterval into
 *     the JS global environment.
 *
 *  2. The host event loop calls js_timers_run(vm) periodically
 *     (e.g. on every iteration of its poll/select/idle loop).
 *     js_timers_run() fires up to JS_TIMERS_RUN_MAX callbacks per
 *     call, oldest-first (lowest fire_at_ms first).  It returns the
 *     number of callbacks actually invoked.
 *
 *  3. On page unload or VM teardown, call js_timers_clear_all() to
 *     cancel everything and release callback slots.
 *
 * ------------------------------------------------------------------
 *  GC / callback-value storage strategy (IMPORTANT -- read this)
 * ------------------------------------------------------------------
 *  The JS engine uses a bump-pointer arena that is RESET to a
 *  checkpoint after every top-level js_eval() call (see js.h).  Any
 *  js_value stored from a previous eval is a dangling reference into
 *  reclaimed arena memory once the next eval begins.
 *
 *  This implementation therefore stores timer callbacks as js_value
 *  structs in a STATIC table (js_timer_slot, declared in js_timers.c)
 *  that lives entirely outside the engine arena.  The js_value struct
 *  is a small tagged union (type + union { int, double, pointer }).
 *  For function values the relevant field is the js_object * pointer
 *  (type == JS_FUNCTION, u.o pointing into the arena).
 *
 *  Trade-off:
 *    - PRO: zero arena overhead; no GC registration protocol needed.
 *    - CON: if the engine arena is reset between the timer being
 *      registered and it being fired (i.e. the callback lives in the
 *      arena of eval A, but fires during eval B), the js_object *
 *      pointer inside the stored js_value is stale.  The arena reset
 *      does NOT zero old memory -- it just moves the bump pointer --
 *      so stale pointers may work by luck if eval B hasn't overwritten
 *      those bytes yet.  This is UNDEFINED BEHAVIOR.
 *
 *  Safe usage modes (pick one):
 *    (a) Single-eval mode: all timers are registered AND fired within
 *        the same js_eval() invocation.  The host loop must call
 *        js_timers_run(vm) from inside js_eval (or from a native
 *        callback invoked during eval).  This is the SAFE mode.
 *
 *    (b) Persistent-root mode (not yet implemented): the engine would
 *        need to expose a "pin this js_value across arena reset" API
 *        (e.g. a root set that js_arena_reset() skips).  See TODO
 *        comment in js_timers.c for the hook point.
 *
 *  In practice, for AutomationOS scripts that run short top-level
 *  evals and drain the timer queue before the next eval, mode (a)
 *  is correct and the behavior is well-defined.
 */

#ifndef JS_TIMERS_H
#define JS_TIMERS_H

#include "js.h"       /* js_vm */

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum timers fired per js_timers_run() call.  Prevents starvation
 * of the event loop if many timers expire at once. */
#define JS_TIMERS_RUN_MAX   32

/*
 * js_timers_install -- wire setTimeout/setInterval/clearTimeout/clearInterval
 * into the JS global environment.  Must be called after js_new() and before
 * js_eval().  Safe to call again after js_new() (re-registers to the fresh
 * global env); the static timer table is NOT cleared -- call js_timers_clear_all()
 * explicitly if you want a fresh timer state.
 */
void js_timers_install(js_vm *vm);

/*
 * js_timers_run -- fire elapsed timers, oldest first.
 *
 * Queries the monotonic clock (SYS_GET_TICKS_MS = 40), scans the timer
 * table, and invokes any callback whose fire_at_ms <= now.
 *
 * One-shot (setTimeout) timers are removed after firing.
 * Repeating (setInterval) timers have their fire_at_ms advanced by
 * period_ms and remain active.
 *
 * At most JS_TIMERS_RUN_MAX callbacks are fired per call.  Call again
 * if the return value equals JS_TIMERS_RUN_MAX and there may be more.
 *
 * Returns the number of callbacks fired (>= 0).
 */
int js_timers_run(js_vm *vm);

/*
 * js_timers_clear_all -- cancel and remove every pending timer.
 * Does NOT invoke any callbacks.  Typically called on page unload or
 * VM teardown.
 */
void js_timers_clear_all(void);

/*
 * js_timers_selftest -- pure logic test with a fake clock.
 * No JS engine required.  Exercises: add, fire, repeat, cancel,
 * ordering, id reuse, table-full handling.
 * Returns 0 on pass, number of failures on error.
 */
int js_timers_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* JS_TIMERS_H */

/*
 * js_console.h -- full console API extension for the AutomationOS JS engine.
 * ==========================================================================
 *
 * Extends the built-in `console` object (which already provides .log via
 * js_builtin.c) with the full common DevTools surface:
 *
 *   console.log(...)      space-separated args + '\n'.  Preserved.
 *   console.info(...)     same as log, prefix "[info] "
 *   console.debug(...)    same as log, prefix "[debug] "
 *   console.warn(...)     prefix "[warn] "
 *   console.error(...)    prefix "[error] "
 *   console.group(label)  indent++, print label
 *   console.groupEnd()    indent--
 *   console.time(label)   record start tick (SYS_GET_TICKS_MS=40)
 *   console.timeEnd(label)print elapsed ms, remove timer
 *   console.count(label)  per-label counter, prints "label: N"
 *   console.dir(obj)      JSON-style object dump
 *   console.assert(c,msg) if !c: print "Assertion failed: msg"
 *   console.table(arr)    ASCII table, header from first object's keys
 *
 * Format specifiers handled by log/info/warn/error/debug:
 *   %s   string coercion
 *   %d   integer (truncated to int64)
 *   %f   double via js_dtoa
 *   %o   object dump (same as console.dir on that argument)
 *   %%   literal '%'
 *
 * NOT handled (treated as literal text): %i %x %c %O (documented skip).
 *
 * ------------------------------------------------------------------
 *  Sink / indentation strategy
 * ------------------------------------------------------------------
 *  All output goes through vm->emit (the same sink js_set_print wires).
 *  A module-static indent level (0..JS_CON_MAX_INDENT) is maintained;
 *  it is SHARED across all console.group / groupEnd calls for this VM
 *  (there is only one static VM instance anyway).
 *
 *  The sink is temporarily replaceable for the self-test: pass a
 *  capture-buffer function pointer to js_console_set_test_sink().
 *
 * ------------------------------------------------------------------
 *  Timer / counter state
 * ------------------------------------------------------------------
 *  Both the timer map (console.time) and counter map (console.count)
 *  are static arrays of fixed size (JS_CON_MAX_TIMERS /
 *  JS_CON_MAX_COUNTERS). Entries are identified by an interned key
 *  (plain C string copy, up to JS_CON_KEY_MAX bytes).
 *
 * ------------------------------------------------------------------
 *  Build (no fs:0x28 canary):
 *    gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *        -fno-stack-protector -fno-pic -fno-pie \
 *        -mno-red-zone -mstackrealign -O2 \
 *        -c userspace/lib/js/js_console.c -o js_console.o
 * ------------------------------------------------------------------
 */

#ifndef JS_CONSOLE_H
#define JS_CONSOLE_H

#include "js.h"   /* js_vm */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Tunables                                                           */
/* ------------------------------------------------------------------ */
#define JS_CON_MAX_INDENT    16   /* maximum group nesting depth        */
#define JS_CON_MAX_TIMERS    32   /* simultaneous console.time() labels */
#define JS_CON_MAX_COUNTERS  64   /* simultaneous console.count() labels*/
#define JS_CON_KEY_MAX       64   /* max label length (incl. NUL)       */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * js_console_install -- replace (extend) the built-in `console` binding.
 *
 * Must be called after js_new() / js_eval() returns and before the next
 * js_eval() that should see the full console surface.  Because js_eval()
 * resets the arena and re-installs builtins (which installs the minimal
 * log/error/warn/info), this function must be re-called after each
 * js_eval() that needs the extended surface.
 *
 * The recommended pattern:
 *   js_vm *vm = js_new();
 *   js_set_print(vm, my_emit);
 *   js_console_install(vm);
 *   js_eval(vm, src, len, out, cap);
 *   // next eval:
 *   js_console_install(vm);   // <-- re-install after arena reset
 *   js_eval(vm, ...);
 *
 * Re-entrant: safe to call multiple times (idempotent for a given VM state).
 */
void js_console_install(js_vm *vm);

/*
 * js_console_selftest -- verify the console implementation.
 *
 * Temporarily replaces the print sink with an internal capture buffer,
 * runs a battery of console calls (via js_eval), and checks the output
 * byte-for-byte against expected strings (documented tolerances below).
 *
 * Tolerances:
 *   - console.time / timeEnd: the elapsed value is checked to be a
 *     non-negative decimal integer followed by "ms".  The exact value
 *     is not checked (timing is non-deterministic).
 *   - console.table: column widths adjust to content; the selftest uses
 *     a fixed dataset where widths are predictable.
 *
 * Returns 0 on pass, > 0 on the number of failures.
 */
int js_console_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* JS_CONSOLE_H */

/*
 * js.h -- public embedding API for the AutomationOS JavaScript engine.
 * ===================================================================
 *
 * A from-scratch, freestanding ES5-subset tree-walking JavaScript interpreter
 * for the AutomationOS ring-3 userspace. STRICTLY freestanding:
 *
 *   - NO libc, NO stdio, NO malloc, NO standard headers.
 *   - All memory comes from a large static arena owned by the engine
 *     (see js_value.c). One js_vm == one arena. No external allocator.
 *   - All string/number/char helpers (length, compare, double parse/format,
 *     math) are implemented internally -- nothing external is referenced.
 *
 * Build (objdump must show NO `fs:0x28` stack canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/js/js_lex.c    -o js_lex.o
 *       (same for js_parse.c js_value.c js_interp.c js_builtin.c)
 *
 * ---------------------------------------------------------------------------
 *  Allocator design (summary; full detail in js_value.c)
 * ---------------------------------------------------------------------------
 *   A single ~6 MB static byte arena per VM serves ALL engine allocations:
 *   AST nodes, value cells (objects/arrays/strings/functions), environment
 *   records, property slots, and scratch string buffers. Allocation is a
 *   bump pointer (no per-object free). The arena is RESET to a checkpoint
 *   after each top-level js_eval()/script run, so long-running shells reclaim
 *   everything between statements. There is NO garbage collector: within a
 *   single run the engine never frees, which is correct for batch script
 *   execution and bounded REPL turns. If the arena fills, allocation returns
 *   a controlled out-of-memory error (the eval fails cleanly, no crash).
 *
 * ---------------------------------------------------------------------------
 *  Embedding model
 * ---------------------------------------------------------------------------
 *   js_vm *vm = js_new();                 // get the (static) VM instance
 *   js_set_print(vm, my_emit);            // wire console.log -> SYS_WRITE
 *   char out[256];
 *   int rc = js_eval(vm, src, len, out, sizeof out);
 *   // rc == 0: out holds the stringified completion value
 *   // rc <  0: out holds an error message
 */

#ifndef JS_H
#define JS_H

/* Opaque VM handle. The concrete struct lives in js_internal.h. */
typedef struct js_vm js_vm;

/*
 * js_new -- return a pointer to the engine's VM instance.
 *
 * The VM (and its multi-megabyte arena) lives in static storage, so this does
 * NOT allocate from any heap. Calling js_new() again returns the SAME instance
 * after re-initializing it (arena reset, builtins re-installed); use it to get
 * a fresh global environment. Returns 0 only if internal init fails.
 */
js_vm *js_new(void);

/*
 * js_eval -- compile and run a source string.
 *
 *   vm         : VM from js_new().
 *   src, len   : the script text (need not be NUL-terminated; len is bytes).
 *   out_result : caller buffer; on return holds either the stringified
 *                completion value (rc==0) or an error message (rc<0),
 *                always NUL-terminated (truncated to fit out_cap).
 *   out_cap    : capacity of out_result in bytes (must be >= 1).
 *
 * Returns 0 on success, <0 on parse error or uncaught runtime error/exception.
 *
 * The completion value is the value of the last evaluated expression
 * statement (REPL semantics), or "undefined" if the program ends on a
 * non-expression statement.
 */
int js_eval(js_vm *vm, const char *src, unsigned long len,
            char *out_result, unsigned long out_cap);

/*
 * js_eval_keep_env -- like js_eval but does NOT reset the arena, the global
 * environment, the native-class registry, or the intern table before running.
 *
 * Use this when the embedder has registered native globals (e.g. the DOM's
 * `document`) that the script must see at top level. js_eval's reset wipes
 * those between calls; this variant preserves them.
 *
 * The completion value still lives in the arena, which grows monotonically
 * across calls until the embedder explicitly calls js_new() again.
 *
 * Same return convention as js_eval (0 = ok, <0 = error; out_result holds
 * the stringified completion or error message).
 */
int js_eval_keep_env(js_vm *vm, const char *src, unsigned long len,
                     char *out_result, unsigned long out_cap);

/*
 * js_set_print -- install the sink used by console.log / console.error.
 *
 * `emit` is called with a UTF-8 byte run `s` of length `n` (no implicit
 * newline; the engine appends '\n' after each console.* call as its own
 * emit). The js CLI app wires this to SYS_WRITE(fd 1). If never set, console
 * output is silently discarded.
 */
void js_set_print(js_vm *vm, void (*emit)(const char *s, unsigned long n));

/*
 * js_selftest -- run the embedded conformance battery.
 *
 * Evaluates a series of scripts and asserts their stringified results
 * (arithmetic precedence, loops, closures, strings, objects, arrays, JSON,
 * Math, coercion, etc). On any failure it reports (via the most recently
 * set print sink, or a default fd-1 sink) which case failed and what was
 * produced vs. expected.
 *
 * Returns 0 if every case passes, otherwise the number of failures (>0),
 * or a negative value on internal error.
 */
int js_selftest(void);

#endif /* JS_H */

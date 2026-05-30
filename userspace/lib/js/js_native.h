/*
 * js_native.h -- "native object" extension for the AutomationOS JS engine.
 * ========================================================================
 *
 * Lets embedders (DOM bindings, FFI shims, etc.) expose C-backed objects
 * whose property gets/sets/method calls dispatch into C. Methods are
 * installed as ordinary own-properties on the wrapper (the engine's
 * existing function-call path is reused, so e.g. `el.appendChild(child)`
 * works without any new call machinery). Property GET (e.g. `el.tagName`)
 * and SET (e.g. `el.id = "x"`) dispatch to optional C callbacks declared
 * in a js_native_class descriptor.
 *
 * Memory model: the WRAPPER OBJECT lives in the JS arena (one per
 * js_eval lifetime). The thing it WRAPS (the void *self_ptr) is owned by
 * the embedder and must outlive the arena reset (use malloc/static).
 *
 * IMPORTANT lifecycle note: js_eval() resets the arena AND the native-
 * class registry. So an embedder must call:
 *    js_native_register_class(vm, ...);
 *    js_native_register_global_value(vm, ...);
 * AFTER each js_new()/js_eval() for which they should be visible.
 *
 * This header pulls in js.h + (minimally) js_internal.h to use js_value.
 */
#ifndef JS_NATIVE_H
#define JS_NATIVE_H

#include "js.h"
#include "js_internal.h"   /* js_value, js_vm, js_object              */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Callback signatures                                                */
/* ------------------------------------------------------------------ */
/* A native method bound to a wrapper. `self_ptr` is the void* that was
 * passed to js_native_wrap. `argv[0..argc)` are the JS arguments. The
 * return value becomes the JS return value. To throw, call
 * js_throw_str(vm, ...) and return js_native_make_undefined(). */
typedef js_value (*js_native_method)(js_vm *vm, void *self_ptr,
                                     int argc, js_value *argv);

/* Property getter. Receives the property name; returns the JS value to
 * yield. Return js_native_make_undefined() to signal "not my property"
 * (engine then falls back to own props + prototype chain).
 *
 * IMPORTANT: if the legitimate value of a property is the JS undefined,
 * the engine will treat it as "not handled". DOM bindings are written
 * so this isn't a problem (missing DOM attrs surface as empty strings,
 * absent nodes as null). */
typedef js_value (*js_native_get)(js_vm *vm, void *self_ptr, const char *prop);

/* Property setter. Returns 0 = handled OK, <0 = error (engine throws).
 * Anything OTHER than these two means "not my property" -- return any
 * positive value to delegate to the engine's normal own-prop store.
 * Recommended convention: return 0 if you recognised the prop, 1 if
 * you didn't. */
typedef int      (*js_native_set)(js_vm *vm, void *self_ptr,
                                  const char *prop, js_value val);

/* ------------------------------------------------------------------ */
/*  Class descriptor                                                   */
/* ------------------------------------------------------------------ */
/* Method entry as exposed publicly. The anonymous struct in the public
 * field of js_native_class uses the SAME layout. */
typedef struct {
    const char       *name;
    js_native_method  fn;
} js_native_method_entry;

typedef struct {
    const char     *name;        /* class display name, e.g. "Element"  */
    js_native_get   get;         /* may be NULL                          */
    js_native_set   set;         /* may be NULL  (NULL == read-only)    */
    /* NULL-terminated method table (last entry: { NULL, NULL }).
     * The struct layout MUST match js_native_method_entry above. */
    const struct { const char *name; js_native_method fn; } *methods;
} js_native_class;

/* ------------------------------------------------------------------ */
/*  Registry                                                           */
/* ------------------------------------------------------------------ */
/* Returns the class id (>=1) to pass to js_native_wrap, or <0 on error
 * (registry full, etc.). The js_native_class pointer must remain valid
 * for the lifetime of the VM (static storage recommended). */
int       js_native_register_class(js_vm *vm, const js_native_class *cls);

/* Wrap a C pointer into a JS-visible object that routes through `cls`.
 * The wrapper inherits methods from the class's method table as own-
 * properties (no shared prototype, kept simple). Returns JS undefined on
 * failure (arena OOM or bad class id). */
js_value  js_native_wrap(js_vm *vm, int class_id, void *ptr);

/* If `v` is a native wrapper, returns the underlying pointer; else NULL. */
void     *js_native_unwrap(js_value v);

/* Convenience: returns the wrapper's class id, or 0 if `v` is not a
 * native wrapper. */
int       js_native_class_id(js_value v);

/* ------------------------------------------------------------------ */
/*  Global-binding helpers                                             */
/* ------------------------------------------------------------------ */
/* Bind a top-level JS variable to a C function (no `this`/class). */
void      js_native_register_function(js_vm *vm, const char *name,
                                      js_value (*fn)(js_vm *, int argc,
                                                     js_value *argv));

/* Bind a top-level JS variable to an arbitrary JS value (e.g. a wrapped
 * native object such as `document`). */
void      js_native_register_global_value(js_vm *vm, const char *name,
                                          js_value v);

/* ------------------------------------------------------------------ */
/*  Value helpers (convenience for method/getter implementations)     */
/* ------------------------------------------------------------------ */
js_value  js_native_make_string(js_vm *vm, const char *s);
js_value  js_native_make_number(js_vm *vm, double n);
js_value  js_native_make_bool(js_vm *vm, int b);
js_value  js_native_make_undefined(void);
js_value  js_native_make_null(void);

int       js_native_to_int(js_value v, int *out);     /* 1=ok 0=fail */

/* Returns a NUL-terminated C string view of `v` (converted via ToString
 * if needed). The pointer is allocated in the VM ARENA and is valid only
 * until the next js_eval/arena reset. Returns NULL on OOM. */
const char *js_native_to_cstr(js_vm *vm, js_value v);

int       js_native_is_string(js_value v);
int       js_native_is_number(js_value v);
int       js_native_is_null_or_undefined(js_value v);

#ifdef __cplusplus
}
#endif

#endif /* JS_NATIVE_H */

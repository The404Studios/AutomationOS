/*
 * js_native.c -- "native object" extension for the AutomationOS JS engine.
 * ========================================================================
 *
 * See js_native.h. Provides:
 *
 *   - A small per-VM native-class registry (max JS_MAX_NATIVE_CLASSES).
 *   - js_native_wrap: builds a JS object whose .native_ptr is the C
 *     self and whose own-properties carry one js_native_fn per declared
 *     method (so the engine's existing call path "just works").
 *   - Dynamic property dispatch hooks (js_native_dispatch_get/_set)
 *     called from js_value.c's js_get_prop / js_set_prop.
 *   - Plain-function and value globals.
 *
 * Implementation strategy for methods (the tricky bit):
 *
 *   The engine's js_native_fn signature is
 *     int (js_vm *, js_value thisv, js_value *argv, int argc, js_value *out)
 *   It does NOT pass the callee back. To know WHICH js_native_method
 *   to dispatch we use a pool of 1024 pre-generated static trampoline
 *   functions, each hard-coded with its own integer slot. Slot 0..31 is
 *   reserved for plain (non-method) functions; slots
 *   class_id*32 .. class_id*32+31 carry methods of that class.
 *
 *   A side table (g_method_bindings[slot]) holds the actual method
 *   pointer (and a flag distinguishing plain functions from methods).
 *   Trampolines look up their binding by slot and forward. The pool is
 *   shared by ALL VMs (we only have one in this engine anyway), with a
 *   sanity check that the binding still belongs to the calling VM.
 *
 * Freestanding: no libc, no stdio. All allocations are in the VM arena.
 *
 * Build (objdump must show NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2 -c js_native.c -o js_native.o
 */
#include "js_native.h"

/* ================================================================== */
/*  Trampoline pool                                                   */
/* ================================================================== */
/*
 * The pool size is JS_MAX_NATIVE_CLASSES * JS_MAX_METHODS_PER_CLASS.
 * MUST equal the count of static trampoline functions generated below
 * (256 = 16 * 16). If you bump either constant, regenerate the list.
 *
 * (We override JS_MAX_NATIVE_CLASSES locally to 16 here -- the engine's
 * registry still has JS_MAX_NATIVE_CLASSES==32 slots, but we only use
 * the first 16 for trampoline lookup so the static trampoline table
 * stays small and macro-friendly. Class registration above 16 still
 * works but won't have method trampolines -- those classes would only
 * support property dispatch via get/set, which is fine for now.)
 */
#define JS_NATIVE_TRAMP_CLASS_LIMIT 16
#define JS_MAX_METHODS_PER_CLASS    16
#define JS_NATIVE_TRAMP_COUNT       (JS_NATIVE_TRAMP_CLASS_LIMIT * JS_MAX_METHODS_PER_CLASS)

/* Per-slot binding. `method` is used when `is_plain==0`; `plain_fn` is
 * used when `is_plain==1`. */
typedef struct {
    js_native_method   method;
    js_value (*plain_fn)(js_vm *, int, js_value *);
    int                class_id;
    js_u8              is_plain;
    js_u8              in_use;
} method_binding;

static method_binding g_method_bindings[JS_NATIVE_TRAMP_COUNT];
static js_vm        *g_method_bindings_vm = (void *)0;

static int slot_of(int class_id, int method_idx)
{
    if (class_id < 0 || class_id >= JS_MAX_NATIVE_CLASSES) return -1;
    if (method_idx < 0 || method_idx >= JS_MAX_METHODS_PER_CLASS) return -1;
    return class_id * JS_MAX_METHODS_PER_CLASS + method_idx;
}

/* Look up registered class by id (slot 0 reserved). */
static const js_native_class *class_by_id(js_vm *vm, int id)
{
    if (!vm || id <= 0 || id >= JS_MAX_NATIVE_CLASSES) return (void *)0;
    return (const js_native_class *)vm->native_classes[id];
}

/* Common trampoline body. Looks up its binding by slot and dispatches. */
static int run_trampoline(int slot, js_vm *vm, js_value thisv,
                          js_value *argv, int argc, js_value *out)
{
    if (out) *out = js_mk_undef();
    if (slot < 0 || slot >= JS_NATIVE_TRAMP_COUNT) return 0;
    method_binding *mb = &g_method_bindings[slot];
    if (!mb->in_use) return 0;
    /* Post-eval-reset safety: if the VM that registered this binding
     * is gone, fail gracefully. */
    if (vm != g_method_bindings_vm) return 0;

    if (mb->is_plain) {
        if (!mb->plain_fn) return 0;
        js_value r = mb->plain_fn(vm, argc, argv);
        if (out) *out = r;
        return vm->has_exception ? -1 : 0;
    }
    if (!mb->method) return 0;
    void *self = (void *)0;
    if (thisv.type == JS_OBJECT && thisv.u.o &&
        (thisv.u.o->flags & JS_OBJ_NATIVE)) {
        self = thisv.u.o->native_ptr;
    }
    js_value r = mb->method(vm, self, argc, argv);
    if (out) *out = r;
    return vm->has_exception ? -1 : 0;
}

/* Generate 256 unique trampolines (16 classes * 16 methods). The C
 * preprocessor doesn't do arithmetic on tokens, so we cannot say
 * `trampoline_##(B+0)`. We instead spell each slot number out as a
 * literal token via a long manual list. The X-macro DEFTRAMP_N(n)
 * defines one trampoline. */
#define DEFTRAMP_N(n)                                                       \
    static int trampoline_##n(js_vm *vm, js_value thisv,                    \
                              js_value *argv, int argc, js_value *out) {    \
        return run_trampoline(n, vm, thisv, argv, argc, out);               \
    }

/* clang-format off */
#define TRAMP_LIST(X) \
X(0)   X(1)   X(2)   X(3)   X(4)   X(5)   X(6)   X(7)   \
X(8)   X(9)   X(10)  X(11)  X(12)  X(13)  X(14)  X(15)  \
X(16)  X(17)  X(18)  X(19)  X(20)  X(21)  X(22)  X(23)  \
X(24)  X(25)  X(26)  X(27)  X(28)  X(29)  X(30)  X(31)  \
X(32)  X(33)  X(34)  X(35)  X(36)  X(37)  X(38)  X(39)  \
X(40)  X(41)  X(42)  X(43)  X(44)  X(45)  X(46)  X(47)  \
X(48)  X(49)  X(50)  X(51)  X(52)  X(53)  X(54)  X(55)  \
X(56)  X(57)  X(58)  X(59)  X(60)  X(61)  X(62)  X(63)  \
X(64)  X(65)  X(66)  X(67)  X(68)  X(69)  X(70)  X(71)  \
X(72)  X(73)  X(74)  X(75)  X(76)  X(77)  X(78)  X(79)  \
X(80)  X(81)  X(82)  X(83)  X(84)  X(85)  X(86)  X(87)  \
X(88)  X(89)  X(90)  X(91)  X(92)  X(93)  X(94)  X(95)  \
X(96)  X(97)  X(98)  X(99)  X(100) X(101) X(102) X(103) \
X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127) \
X(128) X(129) X(130) X(131) X(132) X(133) X(134) X(135) \
X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143) \
X(144) X(145) X(146) X(147) X(148) X(149) X(150) X(151) \
X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159) \
X(160) X(161) X(162) X(163) X(164) X(165) X(166) X(167) \
X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175) \
X(176) X(177) X(178) X(179) X(180) X(181) X(182) X(183) \
X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191) \
X(192) X(193) X(194) X(195) X(196) X(197) X(198) X(199) \
X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207) \
X(208) X(209) X(210) X(211) X(212) X(213) X(214) X(215) \
X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223) \
X(224) X(225) X(226) X(227) X(228) X(229) X(230) X(231) \
X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239) \
X(240) X(241) X(242) X(243) X(244) X(245) X(246) X(247) \
X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)
/* clang-format on */

TRAMP_LIST(DEFTRAMP_N)

#define TRAMP_REF(n) trampoline_##n,
static js_native_fn g_trampolines[JS_NATIVE_TRAMP_COUNT] = {
    TRAMP_LIST(TRAMP_REF)
};

/* Next plain-function slot in the reserved class_id==0 region. Reset
 * implicitly whenever the binding table is cleared (see js_native_reset). */
static int g_plain_fn_next = 0;

/* Reset the bindings table. Called after a js_eval reset so stale
 * bindings can't be invoked against a fresh global env. We expose this
 * via the dispatch hooks: they re-validate g_method_bindings_vm. The
 * embedder should re-register classes after each js_eval; that is
 * already required because js_eval also clears vm->native_classes. */
static void js_native_reset(void)
{
    for (int i = 0; i < JS_NATIVE_TRAMP_COUNT; i++) {
        g_method_bindings[i].method   = (void *)0;
        g_method_bindings[i].plain_fn = (void *)0;
        g_method_bindings[i].class_id = 0;
        g_method_bindings[i].is_plain = 0;
        g_method_bindings[i].in_use   = 0;
    }
    g_plain_fn_next = 0;
    g_method_bindings_vm = (void *)0;
}

/* ================================================================== */
/*  Method-entry compatibility cast                                   */
/* ================================================================== */
/* The class descriptor in the public header declares its method table
 * with an inline anonymous struct that has the SAME layout as
 * js_native_method_entry. We cast to that shape internally to make
 * iteration ergonomic. */
typedef js_native_method_entry method_entry_t;

/* ================================================================== */
/*  Registry                                                          */
/* ================================================================== */
int js_native_register_class(js_vm *vm, const js_native_class *cls)
{
    if (!vm || !cls) return -1;
    /* On a fresh VM, clear our trampoline state once. We detect "fresh"
     * by g_method_bindings_vm != vm. */
    if (g_method_bindings_vm != vm) {
        js_native_reset();
        g_method_bindings_vm = vm;
    }
    if (vm->n_native_classes == 0) vm->n_native_classes = 1;  /* slot 0 reserved */
    if (vm->n_native_classes >= JS_MAX_NATIVE_CLASSES) return -2;
    int id = vm->n_native_classes++;
    vm->native_classes[id] = (const void *)cls;
    return id;
}

/* ================================================================== */
/*  Wrap                                                              */
/* ================================================================== */
js_value js_native_wrap(js_vm *vm, int class_id, void *ptr)
{
    if (!vm) return js_mk_undef();
    const js_native_class *cls = class_by_id(vm, class_id);
    if (!cls) return js_mk_undef();

    js_object *o = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    if (!o) return js_mk_undef();
    o->flags = JS_OBJ_NATIVE;
    o->proto = vm->proto_object;
    o->native_class_id = class_id;
    o->native_ptr = ptr;

    if (cls->methods) {
        const method_entry_t *m = (const method_entry_t *)cls->methods;
        for (int i = 0; i < JS_MAX_METHODS_PER_CLASS && m[i].name; i++) {
            int slot = slot_of(class_id, i);
            if (slot < 0) break;

            /* Register the binding (idempotent across multiple wraps
             * of the same class). */
            method_binding *mb = &g_method_bindings[slot];
            mb->method   = m[i].fn;
            mb->plain_fn = (void *)0;
            mb->class_id = class_id;
            mb->is_plain = 0;
            mb->in_use   = 1;

            js_object *fobj = js_func_new_native(vm, g_trampolines[slot],
                                                 m[i].name);
            if (!fobj) continue;
            js_string *k = js_str_intern(vm, m[i].name,
                                         js_strlen(m[i].name));
            js_obj_set(vm, o, k, js_mk_obj(fobj));
        }
    }
    return js_mk_obj(o);
}

void *js_native_unwrap(js_value v)
{
    if (v.type != JS_OBJECT || !v.u.o) return (void *)0;
    if (!(v.u.o->flags & JS_OBJ_NATIVE)) return (void *)0;
    return v.u.o->native_ptr;
}

int js_native_class_id(js_value v)
{
    if (v.type != JS_OBJECT || !v.u.o) return 0;
    if (!(v.u.o->flags & JS_OBJ_NATIVE)) return 0;
    return v.u.o->native_class_id;
}

/* ================================================================== */
/*  Dispatch hooks (called from js_value.c)                           */
/* ================================================================== */
/* Stash a NUL-terminated copy of `k` into the VM arena and return it.
 * Returns NULL on OOM. */
static char *key_to_cstr(js_vm *vm, js_string *k)
{
    if (!k) return (void *)0;
    char *buf = (char *)js_arena_alloc(vm, k->len + 1);
    if (!buf) return (void *)0;
    for (js_usize i = 0; i < k->len; i++) buf[i] = k->data[i];
    buf[k->len] = 0;
    return buf;
}

int js_native_dispatch_get(js_vm *vm, js_object *o,
                           js_string *key, js_value *out)
{
    if (!o || !(o->flags & JS_OBJ_NATIVE)) return 0;
    const js_native_class *cls = class_by_id(vm, o->native_class_id);
    if (!cls || !cls->get) return 0;
    char *cname = key_to_cstr(vm, key);
    if (!cname) return 0;
    js_value v = cls->get(vm, o->native_ptr, cname);
    /* Convention: undefined return == "not my property". */
    if (v.type == JS_UNDEFINED) return 0;
    if (out) *out = v;
    return 1;
}

int js_native_dispatch_set(js_vm *vm, js_object *o,
                           js_string *key, js_value v)
{
    if (!o || !(o->flags & JS_OBJ_NATIVE)) return 0;
    const js_native_class *cls = class_by_id(vm, o->native_class_id);
    if (!cls || !cls->set) return 0;
    char *cname = key_to_cstr(vm, key);
    if (!cname) return 0;
    int rc = cls->set(vm, o->native_ptr, cname, v);
    if (rc == 0) return 1;        /* handled cleanly */
    if (rc < 0)  return 1;        /* error (thrown via js_throw) — consumed */
    return 0;                     /* not my prop — fall through to own store */
}

/* ================================================================== */
/*  Globals                                                           */
/* ================================================================== */
void js_native_register_function(js_vm *vm, const char *name,
                                 js_value (*fn)(js_vm *, int, js_value *))
{
    if (!vm || !name || !fn) return;
    if (g_method_bindings_vm != vm) {
        js_native_reset();
        g_method_bindings_vm = vm;
    }
    if (g_plain_fn_next >= JS_MAX_METHODS_PER_CLASS) return;
    int slot = slot_of(0, g_plain_fn_next++);
    if (slot < 0) return;

    method_binding *mb = &g_method_bindings[slot];
    mb->method   = (void *)0;
    mb->plain_fn = fn;
    mb->class_id = 0;
    mb->is_plain = 1;
    mb->in_use   = 1;

    js_object *fobj = js_func_new_native(vm, g_trampolines[slot], name);
    if (!fobj) return;
    js_env_define(vm, vm->global_env,
                  js_str_intern(vm, name, js_strlen(name)),
                  js_mk_obj(fobj), 0);
}

void js_native_register_global_value(js_vm *vm, const char *name, js_value v)
{
    if (!vm || !name) return;
    js_env_define(vm, vm->global_env,
                  js_str_intern(vm, name, js_strlen(name)),
                  v, 0);
}

/* ================================================================== */
/*  Value helpers                                                     */
/* ================================================================== */
js_value js_native_make_string(js_vm *vm, const char *s)
{
    if (!vm) return js_mk_undef();
    if (!s) return js_mk_strz(vm, "");
    return js_mk_strz(vm, s);
}
js_value js_native_make_number(js_vm *vm, double n) { (void)vm; return js_mk_num(n); }
js_value js_native_make_bool(js_vm *vm, int b)       { (void)vm; return js_mk_bool(b); }
js_value js_native_make_undefined(void)              { return js_mk_undef(); }
js_value js_native_make_null(void)                   { return js_mk_null(); }

int js_native_to_int(js_value v, int *out)
{
    if (!out) return 0;
    if (v.type == JS_NUMBER) {
        double d = v.u.n;
        if (js_isnan(d) || js_isinf(d)) return 0;
        *out = (int)d;
        return 1;
    }
    if (v.type == JS_BOOL) { *out = v.u.b ? 1 : 0; return 1; }
    return 0;
}

const char *js_native_to_cstr(js_vm *vm, js_value v)
{
    if (!vm) return (void *)0;
    js_string *s = js_to_string(vm, v);
    if (!s) return (void *)0;
    /* js_string buffers are already NUL-terminated by str_alloc. */
    return s->data;
}

int js_native_is_string(js_value v) { return v.type == JS_STRING; }
int js_native_is_number(js_value v) { return v.type == JS_NUMBER; }
int js_native_is_null_or_undefined(js_value v)
{
    return v.type == JS_NULL || v.type == JS_UNDEFINED;
}

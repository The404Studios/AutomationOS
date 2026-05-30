/*
 * js_value.c -- arena allocator, values, strings, objects, arrays, coercions.
 * ==========================================================================
 *
 * The memory foundation of the engine. Everything here is freestanding: no
 * libc, no malloc. All allocation goes through a single bump arena that lives
 * inside the js_vm (in BSS). See js.h / js_internal.h for the design rationale.
 *
 * Also home to the JS value coercion rules (ToNumber/ToString/ToBoolean,
 * strict & loose equality, typeof) and the IEEE-754 double parse/format
 * routines, which are hand-rolled because there is no libc strtod/printf.
 */

#include "js_internal.h"

/* ================================================================== */
/*  Tiny libc-free helpers                                            */
/* ================================================================== */
js_usize js_strlen(const char *s)
{
    js_usize n = 0;
    while (s[n]) n++;
    return n;
}

void js_memcpy(void *d, const void *s, js_usize n)
{
    js_u8 *dd = (js_u8 *)d;
    const js_u8 *ss = (const js_u8 *)s;
    for (js_usize i = 0; i < n; i++) dd[i] = ss[i];
}

void js_memset(void *d, int c, js_usize n)
{
    js_u8 *dd = (js_u8 *)d;
    for (js_usize i = 0; i < n; i++) dd[i] = (js_u8)c;
}

int js_memcmp(const void *a, const void *b, js_usize n)
{
    const js_u8 *aa = (const js_u8 *)a, *bb = (const js_u8 *)b;
    for (js_usize i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    }
    return 0;
}

/* ================================================================== */
/*  IEEE-754 NaN / Infinity (no math.h)                               */
/* ================================================================== */
static double bits_to_double(js_u64 bits)
{
    union { js_u64 u; double d; } v;
    v.u = bits;
    return v.d;
}
static js_u64 double_to_bits(double d)
{
    union { js_u64 u; double d; } v;
    v.d = d;
    return v.u;
}

double js_nan(void)        { return bits_to_double(0x7FF8000000000000ULL); }
double js_inf(int neg)     { return bits_to_double(neg ? 0xFFF0000000000000ULL
                                                       : 0x7FF0000000000000ULL); }

int js_isnan(double x)
{
    js_u64 b = double_to_bits(x);
    js_u64 exp = (b >> 52) & 0x7FF;
    js_u64 man = b & 0xFFFFFFFFFFFFFULL;
    return exp == 0x7FF && man != 0;
}
int js_isinf(double x)
{
    js_u64 b = double_to_bits(x);
    js_u64 exp = (b >> 52) & 0x7FF;
    js_u64 man = b & 0xFFFFFFFFFFFFFULL;
    return exp == 0x7FF && man == 0;
}
int js_isfinite(double x) { return !js_isnan(x) && !js_isinf(x); }

/* ================================================================== */
/*  Arena allocator                                                   */
/* ================================================================== */
void *js_arena_alloc(js_vm *vm, js_usize n)
{
    /* 16-byte align every allocation. */
    js_usize aligned = (n + 15u) & ~(js_usize)15u;
    if (aligned < n) { vm->oom = 1; return NULL; }   /* overflow */
    if (vm->arena_used + aligned > vm->arena_cap) {
        vm->oom = 1;
        return NULL;
    }
    void *p = vm->arena + vm->arena_used;
    vm->arena_used += aligned;
    js_memset(p, 0, aligned);
    return p;
}

void js_arena_mark(js_vm *vm) { vm->arena_mark = vm->arena_used; }
void js_arena_reset(js_vm *vm) { vm->arena_used = vm->arena_mark; vm->oom = 0; }

/* ================================================================== */
/*  String hashing + intern table                                    */
/* ================================================================== */
js_u32 js_str_hash(const char *s, js_usize len)
{
    js_u32 h = 2166136261u;        /* FNV-1a */
    for (js_usize i = 0; i < len; i++) {
        h ^= (js_u8)s[i];
        h *= 16777619u;
    }
    return h;
}

static js_string *str_alloc(js_vm *vm, const char *s, js_usize len)
{
    js_string *str = (js_string *)js_arena_alloc(vm, sizeof(js_string) + len + 1);
    if (!str) return NULL;
    str->len = len;
    if (len && s) js_memcpy(str->data, s, len);
    str->data[len] = 0;
    str->hash = js_str_hash(str->data, len);
    return str;
}

js_string *js_str_new(js_vm *vm, const char *s, js_usize len)
{
    return str_alloc(vm, s, len);
}

js_string *js_str_newz(js_vm *vm, const char *s)
{
    return str_alloc(vm, s, js_strlen(s));
}

int js_str_eq(js_string *a, js_string *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    if (a->hash != b->hash) return 0;
    return js_memcmp(a->data, b->data, a->len) == 0;
}

/*
 * Intern table: dedupe identifiers and short keys so property lookups can
 * compare by pointer when both are interned. Open-addressed, linear probe.
 * Interned strings persist across arena resets only if interned before the
 * mark; after reset we keep the table but stale (post-mark) entries are
 * cleared by re-init in js_new(). Keys created during a run are interned in
 * the post-mark region and dropped on reset.
 */
js_string *js_str_intern(js_vm *vm, const char *s, js_usize len)
{
    js_u32 h = js_str_hash(s, len);
    js_usize mask = JS_MAX_INTERN - 1;
    js_usize i = h & mask;
    for (js_usize probe = 0; probe < JS_MAX_INTERN; probe++) {
        js_string *e = vm->intern[i];
        if (!e) {
            js_string *ns = str_alloc(vm, s, len);
            if (!ns) return NULL;
            vm->intern[i] = ns;
            vm->nintern++;
            return ns;
        }
        if (e->hash == h && e->len == len &&
            js_memcmp(e->data, s, len) == 0) {
            return e;
        }
        i = (i + 1) & mask;
    }
    /* table full: fall back to a non-interned string (still correct). */
    return str_alloc(vm, s, len);
}

js_string *js_str_concat(js_vm *vm, js_string *a, js_string *b)
{
    js_usize la = a ? a->len : 0, lb = b ? b->len : 0;
    js_string *r = (js_string *)js_arena_alloc(vm, sizeof(js_string) + la + lb + 1);
    if (!r) return NULL;
    r->len = la + lb;
    if (la) js_memcpy(r->data, a->data, la);
    if (lb) js_memcpy(r->data + la, b->data, lb);
    r->data[la + lb] = 0;
    r->hash = js_str_hash(r->data, r->len);
    return r;
}

/* ================================================================== */
/*  Value constructors                                               */
/* ================================================================== */
js_value js_mk_undef(void) { js_value v; v.type = JS_UNDEFINED; v.u.n = 0; return v; }
js_value js_mk_null(void)  { js_value v; v.type = JS_NULL; v.u.n = 0; return v; }
js_value js_mk_bool(int b) { js_value v; v.type = JS_BOOL; v.u.b = b ? 1 : 0; return v; }
js_value js_mk_num(double n){ js_value v; v.type = JS_NUMBER; v.u.n = n; return v; }
js_value js_mk_str(js_string *s){ js_value v; v.type = JS_STRING; v.u.s = s; return v; }
js_value js_mk_obj(js_object *o)
{
    js_value v;
    if (!o) return js_mk_undef();
    if (o->flags & JS_OBJ_FUNCTION) v.type = JS_FUNCTION;
    else if (o->flags & JS_OBJ_ARRAY) v.type = JS_ARRAY;
    else v.type = JS_OBJECT;
    v.u.o = o;
    return v;
}
js_value js_mk_strz(js_vm *vm, const char *s)
{
    js_string *str = js_str_newz(vm, s);
    return str ? js_mk_str(str) : js_mk_undef();
}

/* ================================================================== */
/*  Object / array / function creation                               */
/* ================================================================== */
js_object *js_object_new(js_vm *vm)
{
    js_object *o = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    if (!o) return NULL;
    o->proto = vm->proto_object;   /* may be NULL early in bootstrap */
    return o;
}

js_object *js_array_new(js_vm *vm)
{
    js_object *o = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    if (!o) return NULL;
    o->flags = JS_OBJ_ARRAY;
    o->proto = vm->proto_array;
    return o;
}

js_object *js_func_new_native(js_vm *vm, js_native_fn fn, const char *name)
{
    js_object *o = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    if (!o) return NULL;
    o->flags = JS_OBJ_FUNCTION;
    o->proto = vm->proto_function;
    o->fn = (js_func *)js_arena_alloc(vm, sizeof(js_func));
    if (!o->fn) return NULL;
    o->fn->native = fn;
    o->fn->native_name = name;
    return o;
}

/* ================================================================== */
/*  Property store (open-addressed linear probe over js_prop[])       */
/* ================================================================== */
static int prop_grow(js_vm *vm, js_object *o, js_usize want)
{
    js_usize newcap = o->cap ? o->cap * 2 : 8;
    while (newcap < want) newcap *= 2;
    js_prop *np = (js_prop *)js_arena_alloc(vm, newcap * sizeof(js_prop));
    if (!np) return -1;
    /* rehash existing live entries */
    js_usize mask = newcap - 1;
    for (js_usize i = 0; i < o->cap; i++) {
        js_prop *p = &o->props[i];
        if (!p->key) continue;
        js_usize j = p->key->hash & mask;
        while (np[j].key) j = (j + 1) & mask;
        np[j] = *p;
    }
    o->props = np;
    o->cap = newcap;
    return 0;
}

static js_prop *prop_find(js_object *o, js_string *key)
{
    if (!o->cap) return NULL;
    js_usize mask = o->cap - 1;
    js_usize i = key->hash & mask;
    for (js_usize probe = 0; probe < o->cap; probe++) {
        js_prop *p = &o->props[i];
        if (!p->key) return NULL;        /* empty -> not present */
        if (p->key == key || js_str_eq(p->key, key)) return p;
        i = (i + 1) & mask;
    }
    return NULL;
}

int js_obj_has_own(js_object *o, js_string *key)
{
    return prop_find(o, key) != NULL;
}

int js_obj_get(js_vm *vm, js_object *o, js_string *key, js_value *out)
{
    (void)vm;
    js_prop *p = prop_find(o, key);
    if (p) { *out = p->val; return 1; }
    *out = js_mk_undef();
    return 0;
}

int js_obj_set(js_vm *vm, js_object *o, js_string *key, js_value v)
{
    js_prop *p = prop_find(o, key);
    if (p) { p->val = v; return 0; }
    /* insert */
    if ((o->nprops + 1) * 3 >= o->cap * 2) {  /* load factor ~0.66 */
        if (prop_grow(vm, o, o->nprops + 1) != 0) return -1;
    }
    js_usize mask = o->cap - 1;
    js_usize i = key->hash & mask;
    while (o->props[i].key) i = (i + 1) & mask;
    o->props[i].key = key;
    o->props[i].val = v;
    o->props[i].enumerable = 1;
    o->props[i].order = o->order_seq++;
    o->nprops++;
    return 0;
}

/* Collect live props in insertion order (ascending `order`). Small-object
 * friendly selection sort over a temporary copy of live slot pointers. */
js_usize js_obj_ordered(js_object *o, js_prop **out, js_usize cap)
{
    js_usize n = 0;
    for (js_usize i = 0; i < o->cap && n < cap; i++) {
        if (o->props[i].key) out[n++] = &o->props[i];
    }
    /* insertion sort by .order (stable, n is small for typical objects) */
    for (js_usize i = 1; i < n; i++) {
        js_prop *k = out[i];
        js_isize j = (js_isize)i - 1;
        while (j >= 0 && out[j]->order > k->order) { out[j+1] = out[j]; j--; }
        out[j+1] = k;
    }
    return n;
}

int js_obj_delete(js_object *o, js_string *key)
{
    js_prop *p = prop_find(o, key);
    if (!p) return 0;
    /* simple tombstone: mark empty and re-probe the rest of the cluster */
    p->key = NULL;
    o->nprops--;
    /* reinsert following cluster to keep probe invariant */
    js_usize mask = o->cap - 1;
    js_usize i = (js_usize)(p - o->props);
    i = (i + 1) & mask;
    while (o->props[i].key) {
        js_prop moved = o->props[i];
        o->props[i].key = NULL;
        o->nprops--;
        js_usize j = moved.key->hash & mask;
        while (o->props[j].key) j = (j + 1) & mask;
        o->props[j] = moved;
        o->nprops++;
        i = (i + 1) & mask;
    }
    return 1;
}

/* ================================================================== */
/*  Array element store                                               */
/* ================================================================== */
static int arr_grow(js_vm *vm, js_object *a, js_usize want)
{
    if (want <= a->ecap) return 0;
    js_usize newcap = a->ecap ? a->ecap * 2 : 8;
    while (newcap < want) newcap *= 2;
    js_value *ne = (js_value *)js_arena_alloc(vm, newcap * sizeof(js_value));
    if (!ne) return -1;
    for (js_usize i = 0; i < a->length; i++) ne[i] = a->elems[i];
    for (js_usize i = a->length; i < newcap; i++) ne[i] = js_mk_undef();
    a->elems = ne;
    a->ecap = newcap;
    return 0;
}

int js_arr_push(js_vm *vm, js_object *a, js_value v)
{
    if (arr_grow(vm, a, a->length + 1) != 0) return -1;
    a->elems[a->length++] = v;
    return 0;
}

js_value js_arr_get(js_object *a, js_usize i)
{
    if (i < a->length) return a->elems[i];
    return js_mk_undef();
}

int js_arr_set(js_vm *vm, js_object *a, js_usize i, js_value v)
{
    if (i >= a->length) {
        if (arr_grow(vm, a, i + 1) != 0) return -1;
        for (js_usize k = a->length; k < i; k++) a->elems[k] = js_mk_undef();
        a->length = i + 1;
    }
    a->elems[i] = v;
    return 0;
}

/* ================================================================== */
/*  Number <-> index helper                                          */
/* ================================================================== */
/* Parse a key string as an array index. Returns 1 and *idx if it is a
 * canonical non-negative integer string ("0".."4294967294"). */
static int key_as_index(js_string *key, js_usize *idx)
{
    if (!key->len) return 0;
    if (key->len > 1 && key->data[0] == '0') return 0; /* no leading zero */
    js_usize v = 0;
    for (js_usize i = 0; i < key->len; i++) {
        char c = key->data[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (js_usize)(c - '0');
    }
    *idx = v;
    return 1;
}

/* ================================================================== */
/*  Generic property get/set (handles arrays, length, prototype chain) */
/* ================================================================== */
static js_string *intern_lit(js_vm *vm, const char *s)
{
    return js_str_intern(vm, s, js_strlen(s));
}

int js_get_prop(js_vm *vm, js_value obj, js_string *key, js_value *out)
{
    *out = js_mk_undef();

    if (obj.type == JS_STRING) {
        /* string .length and indexed access + proto methods */
        if (key->len == 6 && js_memcmp(key->data, "length", 6) == 0) {
            *out = js_mk_num((double)obj.u.s->len);
            return 1;
        }
        js_usize idx;
        if (key_as_index(key, &idx)) {
            if (idx < obj.u.s->len) {
                *out = js_mk_str(js_str_new(vm, obj.u.s->data + idx, 1));
                return 1;
            }
            return 0;
        }
        if (vm->proto_string) return js_obj_get(vm, vm->proto_string, key, out);
        return 0;
    }

    if (obj.type == JS_NUMBER) {
        if (vm->proto_number) return js_obj_get(vm, vm->proto_number, key, out);
        return 0;
    }

    if (obj.type != JS_OBJECT && obj.type != JS_ARRAY &&
        obj.type != JS_FUNCTION) {
        return 0;
    }

    js_object *o = obj.u.o;

    if (o->flags & JS_OBJ_ARRAY) {
        if (key->len == 6 && js_memcmp(key->data, "length", 6) == 0) {
            *out = js_mk_num((double)o->length);
            return 1;
        }
        js_usize idx;
        if (key_as_index(key, &idx)) {
            if (idx < o->length) { *out = o->elems[idx]; return 1; }
            return 0;
        }
    }

    /* native-object dynamic property (DOM bindings etc.); see
     * lib/js/js_native.c. Tried before own-prop so getters for live
     * DOM properties (id, textContent, ...) work even when there is
     * no JS-side cached copy. */
    if ((o->flags & JS_OBJ_NATIVE) &&
        js_native_dispatch_get(vm, o, key, out)) {
        return 1;
    }

    /* own property */
    if (js_obj_get(vm, o, key, out)) return 1;

    /* prototype chain (builtin methods) */
    js_object *p = o->proto;
    int guard = 0;
    while (p && guard++ < 16) {
        if (js_obj_get(vm, p, key, out)) return 1;
        p = p->proto;
    }
    *out = js_mk_undef();
    return 0;
}

int js_set_prop(js_vm *vm, js_value obj, js_string *key, js_value v)
{
    if (obj.type != JS_OBJECT && obj.type != JS_ARRAY &&
        obj.type != JS_FUNCTION) {
        return 0;  /* setting on primitives is a no-op (non-strict) */
    }
    js_object *o = obj.u.o;

    if (o->flags & JS_OBJ_ARRAY) {
        if (key->len == 6 && js_memcmp(key->data, "length", 6) == 0) {
            double dn = js_to_number(vm, v);
            js_usize nl = (js_usize)dn;
            if (nl < o->length) o->length = nl;
            else if (nl > o->length) { arr_grow(vm, o, nl); o->length = nl; }
            return 0;
        }
        js_usize idx;
        if (key_as_index(key, &idx)) {
            return js_arr_set(vm, o, idx, v);
        }
    }

    /* native-object dynamic setter; see lib/js/js_native.c. */
    if ((o->flags & JS_OBJ_NATIVE) &&
        js_native_dispatch_set(vm, o, key, v)) {
        return 0;
    }

    (void)intern_lit;
    return js_obj_set(vm, o, key, v);
}

/* ================================================================== */
/*  Coercions                                                         */
/* ================================================================== */
int js_truthy(js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED:
    case JS_NULL:     return 0;
    case JS_BOOL:     return v.u.b;
    case JS_NUMBER:   return !(v.u.n == 0.0 || js_isnan(v.u.n));
    case JS_STRING:   return v.u.s && v.u.s->len > 0;
    default:          return 1;   /* objects/arrays/functions */
    }
}

const char *js_typeof(js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED: return "undefined";
    case JS_NULL:      return "object";   /* historical JS quirk */
    case JS_BOOL:      return "boolean";
    case JS_NUMBER:    return "number";
    case JS_STRING:    return "string";
    case JS_FUNCTION:  return "function";
    default:           return "object";
    }
}

double js_to_number(js_vm *vm, js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED: return js_nan();
    case JS_NULL:      return 0.0;
    case JS_BOOL:      return v.u.b ? 1.0 : 0.0;
    case JS_NUMBER:    return v.u.n;
    case JS_STRING: {
        /* trim whitespace, empty => 0 */
        js_string *s = v.u.s;
        js_usize i = 0, j = s->len;
        while (i < j && (s->data[i]==' '||s->data[i]=='\t'||s->data[i]=='\n'||
                         s->data[i]=='\r')) i++;
        while (j > i && (s->data[j-1]==' '||s->data[j-1]=='\t'||
                         s->data[j-1]=='\n'||s->data[j-1]=='\r')) j--;
        if (i == j) return 0.0;
        int ok = 0;
        double d = js_parse_double(s->data + i, j - i, &ok);
        return ok ? d : js_nan();
    }
    default: {
        /* object -> primitive: arrays join, others NaN-ish.
         * We approximate via ToString then ToNumber. */
        js_string *s = js_to_string(vm, v);
        if (!s || s->len == 0) return v.type == JS_ARRAY ? 0.0 : js_nan();
        int ok = 0;
        double d = js_parse_double(s->data, s->len, &ok);
        return ok ? d : js_nan();
    }
    }
}

/* forward decl for array/object stringification */
static js_string *obj_to_string(js_vm *vm, js_value v);

js_string *js_to_string(js_vm *vm, js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED: return js_str_newz(vm, "undefined");
    case JS_NULL:      return js_str_newz(vm, "null");
    case JS_BOOL:      return js_str_newz(vm, v.u.b ? "true" : "false");
    case JS_NUMBER:    return js_num_to_str(vm, v.u.n);
    case JS_STRING:    return v.u.s;
    default:           return obj_to_string(vm, v);
    }
}

/* Default ToString for objects/arrays/functions. */
static js_string *obj_to_string(js_vm *vm, js_value v)
{
    if (v.type == JS_ARRAY) {
        /* Array.prototype.toString == join(",") */
        js_object *a = v.u.o;
        js_string *acc = js_str_newz(vm, "");
        for (js_usize i = 0; i < a->length; i++) {
            if (i) acc = js_str_concat(vm, acc, js_str_newz(vm, ","));
            js_value e = a->elems[i];
            if (e.type == JS_UNDEFINED || e.type == JS_NULL) continue;
            acc = js_str_concat(vm, acc, js_to_string(vm, e));
        }
        return acc;
    }
    if (v.type == JS_FUNCTION) {
        return js_str_newz(vm, "function () { [native code] }");
    }
    /* plain object */
    return js_str_newz(vm, "[object Object]");
}

/* ================================================================== */
/*  Console / display stringification (JSON-ish for objects)         */
/* ================================================================== */
static js_string *display_rec(js_vm *vm, js_value v, int depth, int quote_str);

static js_string *display_array(js_vm *vm, js_value v, int depth)
{
    js_object *a = v.u.o;
    js_string *acc = js_str_newz(vm, "[ ");
    if (a->length == 0) return js_str_newz(vm, "[]");
    for (js_usize i = 0; i < a->length; i++) {
        if (i) acc = js_str_concat(vm, acc, js_str_newz(vm, ", "));
        acc = js_str_concat(vm, acc, display_rec(vm, a->elems[i], depth+1, 1));
    }
    acc = js_str_concat(vm, acc, js_str_newz(vm, " ]"));
    return acc;
}

static js_string *display_object(js_vm *vm, js_value v, int depth)
{
    js_object *o = v.u.o;
    js_string *acc = js_str_newz(vm, "{ ");
    js_prop *ord[256];
    js_usize n = js_obj_ordered(o, ord, 256);
    int first = 1;
    for (js_usize i = 0; i < n; i++) {
        js_prop *p = ord[i];
        if (!p->enumerable) continue;
        if (!first) acc = js_str_concat(vm, acc, js_str_newz(vm, ", "));
        first = 0;
        acc = js_str_concat(vm, acc, p->key);
        acc = js_str_concat(vm, acc, js_str_newz(vm, ": "));
        acc = js_str_concat(vm, acc, display_rec(vm, p->val, depth+1, 1));
    }
    if (first) return js_str_newz(vm, "{}");
    acc = js_str_concat(vm, acc, js_str_newz(vm, " }"));
    return acc;
}

static js_string *display_rec(js_vm *vm, js_value v, int depth, int quote_str)
{
    if (depth > 6) return js_str_newz(vm, "...");
    switch (v.type) {
    case JS_STRING:
        if (quote_str) {
            js_string *q = js_str_newz(vm, "'");
            q = js_str_concat(vm, q, v.u.s);
            q = js_str_concat(vm, q, js_str_newz(vm, "'"));
            return q;
        }
        return v.u.s;
    case JS_ARRAY:    return display_array(vm, v, depth);
    case JS_OBJECT:   return display_object(vm, v, depth);
    case JS_FUNCTION: {
        js_func *f = v.u.o->fn;
        const char *nm = f && f->name ? f->name->data
                       : (f && f->native_name ? f->native_name : "anonymous");
        js_string *s = js_str_newz(vm, "[Function: ");
        s = js_str_concat(vm, s, js_str_newz(vm, nm));
        s = js_str_concat(vm, s, js_str_newz(vm, "]"));
        return s;
    }
    default: return js_to_string(vm, v);
    }
}

js_string *js_value_to_display(js_vm *vm, js_value v)
{
    /* top-level: strings are shown raw (no quotes), as console.log does */
    return display_rec(vm, v, 0, 0);
}

/* ================================================================== */
/*  Equality                                                          */
/* ================================================================== */
int js_strict_eq(js_value a, js_value b)
{
    if (a.type != b.type) {
        /* number/number already same type; JS_OBJECT vs JS_ARRAY vs
         * JS_FUNCTION are all "object" type for ===, compare identity */
        int aobj = (a.type==JS_OBJECT||a.type==JS_ARRAY||a.type==JS_FUNCTION);
        int bobj = (b.type==JS_OBJECT||b.type==JS_ARRAY||b.type==JS_FUNCTION);
        if (aobj && bobj) return a.u.o == b.u.o;
        return 0;
    }
    switch (a.type) {
    case JS_UNDEFINED:
    case JS_NULL:    return 1;
    case JS_BOOL:    return a.u.b == b.u.b;
    case JS_NUMBER:
        if (js_isnan(a.u.n) || js_isnan(b.u.n)) return 0;
        return a.u.n == b.u.n;
    case JS_STRING:  return js_str_eq(a.u.s, b.u.s);
    default:         return a.u.o == b.u.o;   /* identity */
    }
}

int js_loose_eq(js_vm *vm, js_value a, js_value b)
{
    if (a.type == b.type) return js_strict_eq(a, b);

    /* null == undefined */
    if ((a.type==JS_NULL && b.type==JS_UNDEFINED) ||
        (a.type==JS_UNDEFINED && b.type==JS_NULL)) return 1;
    if (a.type==JS_NULL || a.type==JS_UNDEFINED ||
        b.type==JS_NULL || b.type==JS_UNDEFINED) return 0;

    /* number vs string -> compare as numbers */
    if (a.type==JS_NUMBER && b.type==JS_STRING)
        return a.u.n == js_to_number(vm, b);
    if (a.type==JS_STRING && b.type==JS_NUMBER)
        return js_to_number(vm, a) == b.u.n;

    /* boolean -> number then re-compare */
    if (a.type==JS_BOOL) { a = js_mk_num(a.u.b?1:0); return js_loose_eq(vm,a,b); }
    if (b.type==JS_BOOL) { b = js_mk_num(b.u.b?1:0); return js_loose_eq(vm,a,b); }

    /* object vs primitive -> ToPrimitive(object) (use ToString) */
    int aobj = (a.type==JS_OBJECT||a.type==JS_ARRAY||a.type==JS_FUNCTION);
    int bobj = (b.type==JS_OBJECT||b.type==JS_ARRAY||b.type==JS_FUNCTION);
    if (aobj && !bobj) { a = js_mk_str(js_to_string(vm,a)); return js_loose_eq(vm,a,b); }
    if (bobj && !aobj) { b = js_mk_str(js_to_string(vm,b)); return js_loose_eq(vm,a,b); }
    return 0;
}

/* ================================================================== */
/*  Double parsing  (decimal, hex, exponent, +/- inf/nan keywords)    */
/* ================================================================== */
double js_parse_double(const char *s, js_usize len, int *ok)
{
    *ok = 0;
    js_usize i = 0;
    int neg = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = (s[i]=='-'); i++; }

    /* Infinity keyword */
    if (i + 8 <= len && js_memcmp(s+i, "Infinity", 8) == 0) {
        if (i + 8 == len) { *ok = 1; return js_inf(neg); }
    }

    /* hex 0x.. (integer only) */
    if (i + 1 < len && s[i] == '0' && (s[i+1]=='x' || s[i+1]=='X')) {
        i += 2;
        if (i >= len) return 0.0;
        double v = 0.0;
        for (; i < len; i++) {
            char c = s[i];
            int d;
            if (c>='0'&&c<='9') d = c-'0';
            else if (c>='a'&&c<='f') d = c-'a'+10;
            else if (c>='A'&&c<='F') d = c-'A'+10;
            else return 0.0;   /* invalid hex */
            v = v * 16.0 + d;
        }
        *ok = 1;
        return neg ? -v : v;
    }

    double mant = 0.0;
    int any_digits = 0;
    /* integer part */
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        mant = mant * 10.0 + (s[i]-'0');
        i++; any_digits = 1;
    }
    /* fraction */
    int frac_digits = 0;
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            mant = mant * 10.0 + (s[i]-'0');
            frac_digits++;
            i++; any_digits = 1;
        }
    }
    if (!any_digits) return 0.0;

    int exp = -frac_digits;
    /* exponent */
    if (i < len && (s[i]=='e' || s[i]=='E')) {
        i++;
        int esign = 1, eval = 0, edig = 0;
        if (i < len && (s[i]=='+'||s[i]=='-')) { if (s[i]=='-') esign=-1; i++; }
        while (i < len && s[i]>='0' && s[i]<='9') {
            eval = eval*10 + (s[i]-'0'); i++; edig = 1;
        }
        if (!edig) return 0.0;
        exp += esign * eval;
    }

    if (i != len) return 0.0;   /* trailing garbage */

    /* scale mantissa by 10^exp using repeated multiply (bounded). */
    double result = mant;
    int e = exp;
    if (e > 0) {
        while (e >= 19) { result *= 1e19; e -= 19; }
        while (e-- > 0) result *= 10.0;
    } else if (e < 0) {
        e = -e;
        while (e >= 19) { result /= 1e19; e -= 19; }
        while (e-- > 0) result /= 10.0;
    }
    *ok = 1;
    return neg ? -result : result;
}

/* ================================================================== */
/*  Double formatting (dtoa)                                          */
/* ================================================================== */
/*
 * A pragmatic double->string that matches JS number printing for the common
 * cases the engine produces: integers print without a decimal point; finite
 * non-integers print up to 15 significant digits with trailing zeros trimmed;
 * NaN/Infinity handled; very large/small magnitudes fall back to scientific.
 *
 * Returns the number of chars written (excluding NUL).
 */
static js_usize u64_to_str(js_u64 v, char *buf)
{
    char tmp[24];
    js_usize n = 0;
    if (v == 0) { buf[0]='0'; buf[1]=0; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (js_usize i = 0; i < n; i++) buf[i] = tmp[n-1-i];
    buf[n] = 0;
    return n;
}

js_usize js_dtoa(double n, char *buf, js_usize cap)
{
    if (cap < 32) { if (cap) buf[0]=0; return 0; }

    if (js_isnan(n)) { js_memcpy(buf,"NaN",4); return 3; }
    if (js_isinf(n)) {
        if (n < 0) { js_memcpy(buf,"-Infinity",10); return 9; }
        js_memcpy(buf,"Infinity",9); return 8;
    }

    js_usize pos = 0;
    if (n == 0.0) { buf[0]='0'; buf[1]=0; return 1; }
    if (n < 0) { buf[pos++]='-'; n = -n; }

    /* Integer fast path (exact for |n| < 2^53). */
    if (n < 9.007199254740992e15 && n == (double)(js_u64)n) {
        js_usize w = u64_to_str((js_u64)n, buf + pos);
        return pos + w;
    }

    /*
     * General path: produce up to 15 significant digits.
     * Determine decimal exponent by scaling into [1,10).
     */
    int e10 = 0;
    double m = n;
    while (m >= 10.0) { m /= 10.0; e10++; }
    while (m < 1.0)   { m *= 10.0; e10--; }

    /* Decide between fixed and scientific (JS uses sci for e<-6 or e>=21). */
    int use_sci = (e10 < -6 || e10 >= 21);

    /* Extract 15 significant digits via repeated scaling. */
    char digits[20];
    int ndig = 15;
    /* round: add 0.5 ulp at the last digit position */
    double scale = m;
    for (int i = 0; i < ndig; i++) {
        int d = (int)scale;
        if (d > 9) d = 9;
        if (d < 0) d = 0;
        digits[i] = (char)('0' + d);
        scale = (scale - d) * 10.0;
    }
    /* round last digit */
    if (scale >= 5.0) {
        int i = ndig - 1;
        for (;;) {
            if (digits[i] < '9') { digits[i]++; break; }
            digits[i] = '0';
            if (i == 0) {
                /* carry out: shift, e.g. 9.99..->10.0 */
                for (int k = ndig-1; k > 0; k--) digits[k] = digits[k-1];
                digits[0] = '1';
                e10++;
                break;
            }
            i--;
        }
    }
    /* trim trailing zeros */
    while (ndig > 1 && digits[ndig-1] == '0') ndig--;

    if (!use_sci) {
        if (e10 >= 0) {
            /* digits before point = e10+1 */
            int ip = e10 + 1;
            for (int i = 0; i < ip; i++)
                buf[pos++] = (i < ndig) ? digits[i] : '0';
            if (ndig > ip) {
                buf[pos++] = '.';
                for (int i = ip; i < ndig; i++) buf[pos++] = digits[i];
            }
        } else {
            buf[pos++] = '0';
            buf[pos++] = '.';
            for (int i = 0; i < -e10 - 1; i++) buf[pos++] = '0';
            for (int i = 0; i < ndig; i++) buf[pos++] = digits[i];
        }
    } else {
        /* scientific: d.dddde+XX */
        buf[pos++] = digits[0];
        if (ndig > 1) {
            buf[pos++] = '.';
            for (int i = 1; i < ndig; i++) buf[pos++] = digits[i];
        }
        buf[pos++] = 'e';
        buf[pos++] = (e10 < 0) ? '-' : '+';
        int ae = e10 < 0 ? -e10 : e10;
        char ebuf[8];
        js_usize en = u64_to_str((js_u64)ae, ebuf);
        for (js_usize i = 0; i < en; i++) buf[pos++] = ebuf[i];
    }
    buf[pos] = 0;
    return pos;
}

js_string *js_num_to_str(js_vm *vm, double n)
{
    char buf[JS_NUMBUF_CAP];
    js_usize len = js_dtoa(n, buf, sizeof(buf));
    return js_str_new(vm, buf, len);
}

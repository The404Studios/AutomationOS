/*
 * js_interp.c -- the tree-walking evaluator.
 * ==========================================
 *
 * Walks the AST produced by js_parse.c, evaluating expressions and executing
 * statements with lexical scoping and real closures (functions capture the
 * environment in which they were defined).
 *
 * Control flow is propagated by js_completion: CMP_NORMAL/RETURN/BREAK/
 * CONTINUE/THROW. The "current value" for RETURN/THROW lives in *out (return)
 * or vm->exception (throw). Break/continue unwind loops; throw unwinds to the
 * nearest try/catch (js_eval_node for NODE_TRY) or to the top.
 *
 * Environments are chained scope records; variable lookup walks parents.
 * Function declarations are hoisted within their containing block/function.
 */

#include "js_internal.h"

/* ------------------------------------------------------------------ */
/*  Environments                                                       */
/* ------------------------------------------------------------------ */
js_env *js_env_new(js_vm *vm, js_env *parent)
{
    js_env *e = (js_env *)js_arena_alloc(vm, sizeof(js_env));
    if (!e) return NULL;
    e->parent = parent;
    return e;
}

static js_binding *env_find_local(js_env *e, js_string *name)
{
    for (js_usize i = 0; i < e->nbinds; i++) {
        if (js_str_eq(e->binds[i].name, name)) return &e->binds[i];
    }
    return NULL;
}

int js_env_define(js_vm *vm, js_env *e, js_string *name, js_value v, int is_const)
{
    js_binding *b = env_find_local(e, name);
    if (b) { b->val = v; b->is_const = (js_u8)is_const; return 0; }  /* redeclare */
    if (e->nbinds >= e->cap) {
        js_usize nc = e->cap ? e->cap * 2 : 8;
        js_binding *nb = (js_binding *)js_arena_alloc(vm, nc * sizeof(js_binding));
        if (!nb) return -1;
        for (js_usize i = 0; i < e->nbinds; i++) nb[i] = e->binds[i];
        e->binds = nb;
        e->cap = nc;
    }
    e->binds[e->nbinds].name = name;
    e->binds[e->nbinds].val = v;
    e->binds[e->nbinds].is_const = (js_u8)is_const;
    e->nbinds++;
    return 0;
}

int js_env_get(js_env *e, js_string *name, js_value *out)
{
    for (js_env *s = e; s; s = s->parent) {
        js_binding *b = env_find_local(s, name);
        if (b) { *out = b->val; return 1; }
    }
    *out = js_mk_undef();
    return 0;
}

int js_env_set(js_vm *vm, js_env *e, js_string *name, js_value v)
{
    for (js_env *s = e; s; s = s->parent) {
        js_binding *b = env_find_local(s, name);
        if (b) {
            if (b->is_const) { js_throw_str(vm, "Assignment to constant variable"); return -1; }
            b->val = v;
            return 1;
        }
    }
    /* implicit global (non-strict) */
    js_env *g = e;
    while (g->parent) g = g->parent;
    js_env_define(vm, g, name, v, 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Throwing                                                           */
/* ------------------------------------------------------------------ */
void js_throw(js_vm *vm, js_value v)
{
    vm->exception = v;
    vm->has_exception = 1;
}

void js_throw_str(js_vm *vm, const char *msg)
{
    /* build an Error-like object: { message: msg, name: "Error" } */
    js_object *o = js_object_new(vm);
    if (o) {
        o->flags |= JS_OBJ_ERROR;
        js_obj_set(vm, o, js_str_intern(vm, "message", 7), js_mk_strz(vm, msg));
        js_obj_set(vm, o, js_str_intern(vm, "name", 4), js_mk_strz(vm, "Error"));
        js_throw(vm, js_mk_obj(o));
    } else {
        js_throw(vm, js_mk_strz(vm, msg));
    }
}

/* ------------------------------------------------------------------ */
/*  Numeric / bitwise helpers                                          */
/* ------------------------------------------------------------------ */
static js_i32 to_int32(double d)
{
    if (js_isnan(d) || js_isinf(d)) return 0;
    /* truncate toward zero, mod 2^32 */
    double t = d < 0 ? -js_math_floor(-d) : js_math_floor(d);
    /* reduce modulo 2^32 */
    double m = t;
    double two32 = 4294967296.0;
    m = m - js_math_floor(m / two32) * two32;
    if (m < 0) m += two32;
    js_u32 u = (js_u32)m;
    return (js_i32)u;
}
static js_u32 to_uint32(double d)
{
    return (js_u32)to_int32(d);
}

/* ------------------------------------------------------------------ */
/*  Forward                                                            */
/* ------------------------------------------------------------------ */
static js_completion eval_block(js_vm *vm, js_node *block, js_env *env,
                                js_value *out, int new_scope);
static js_completion eval_expr(js_vm *vm, js_node *n, js_env *env, js_value *out);

/* hoist function declarations (and var names) in a statement list */
static void hoist(js_vm *vm, js_node **stmts, int n, js_env *env);

/* ------------------------------------------------------------------ */
/*  Make a script function value (closure)                            */
/* ------------------------------------------------------------------ */
static js_value make_closure(js_vm *vm, js_node *fn, js_env *env, int is_arrow)
{
    js_object *o = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    if (!o) return js_mk_undef();
    o->flags = JS_OBJ_FUNCTION;
    o->proto = vm->proto_function;
    o->fn = (js_func *)js_arena_alloc(vm, sizeof(js_func));
    if (!o->fn) return js_mk_undef();
    o->fn->params = fn->a;
    o->fn->body = fn->b_;
    o->fn->closure = env;
    o->fn->name = fn->str;
    o->fn->is_arrow = is_arrow;
    o->fn->nparams = fn->a ? fn->a->nkids : 0;
    /*
     * Give ordinary (non-arrow) functions a `.prototype` object up front so
     * that `Ctor.prototype.method = ...` mutates a real, persistent object --
     * the same one `new Ctor()` installs as the instance proto and that
     * `instanceof` walks. Arrow functions are never constructors, so they get
     * none. Created lazily-safe: if proto_object isn't up yet (early bootstrap)
     * the fresh object simply has a NULL proto, which is fine.
     */
    if (!is_arrow) {
        js_object *proto = js_object_new(vm);
        if (proto) {
            js_string *kp = js_str_intern(vm, "prototype", 9);
            js_obj_set(vm, o, kp, js_mk_obj(proto));
            /* `prototype` is non-enumerable (matches ES): keep it out of
             * Object.keys(fn) / for-in / JSON.stringify(fn). */
            js_prop *ord[8];
            js_usize cnt = js_obj_ordered(o, ord, 8);
            for (js_usize i = 0; i < cnt; i++)
                if (js_str_eq(ord[i]->key, kp)) { ord[i]->enumerable = 0; break; }
        }
    }
    return js_mk_obj(o);
}

/* ------------------------------------------------------------------ */
/*  Assignment target resolution                                      */
/* ------------------------------------------------------------------ */
/* Evaluate the compound value for op-assign given current + rhs. */
static js_value apply_binop_value(js_vm *vm, js_tok_kind op, js_value a, js_value b);

static js_completion assign_to(js_vm *vm, js_node *target, js_value val,
                               js_env *env)
{
    if (target->kind == NODE_IDENT) {
        js_env_set(vm, env, target->str, val);
        if (vm->has_exception) return CMP_THROW;
        return CMP_NORMAL;
    }
    if (target->kind == NODE_MEMBER) {
        js_value obj;
        js_completion c = eval_expr(vm, target->a, env, &obj);
        if (c != CMP_NORMAL) return c;
        js_string *key;
        if (target->flag & MEMBER_COMPUTED) {
            js_value kv;
            c = eval_expr(vm, target->b_, env, &kv);
            if (c != CMP_NORMAL) return c;
            key = js_to_string(vm, kv);
        } else {
            key = target->str;
        }
        js_set_prop(vm, obj, key, val);
        if (vm->has_exception) return CMP_THROW;
        return CMP_NORMAL;
    }
    js_throw_str(vm, "Invalid assignment target");
    return CMP_THROW;
}

/* ------------------------------------------------------------------ */
/*  Function .prototype helper (for `new` and `instanceof`)            */
/* ------------------------------------------------------------------ */
/*
 * Return the object stored as fn's own "prototype" property, lazily creating a
 * fresh plain object the first time it is needed. This lets user constructors
 * participate in prototype-based inheritance: `Ctor.prototype.method = ...`
 * mutates the SAME object that `new Ctor()` installs as the instance's proto,
 * and that `x instanceof Ctor` walks. Returns NULL if fn is not a function or
 * allocation fails.
 */
static js_object *func_get_prototype(js_vm *vm, js_value fn)
{
    if (fn.type != JS_FUNCTION) return NULL;
    js_object *fo = fn.u.o;
    js_string *kp = js_str_intern(vm, "prototype", 9);
    js_value pv;
    if (js_obj_get(vm, fo, kp, &pv) &&
        (pv.type == JS_OBJECT || pv.type == JS_ARRAY || pv.type == JS_FUNCTION)) {
        return pv.u.o;
    }
    /* create a fresh prototype object on demand */
    js_object *proto = js_object_new(vm);
    if (!proto) return NULL;
    js_obj_set(vm, fo, kp, js_mk_obj(proto));
    return proto;
}

/* ------------------------------------------------------------------ */
/*  Binary operations                                                  */
/* ------------------------------------------------------------------ */
static js_value apply_binop_value(js_vm *vm, js_tok_kind op, js_value a, js_value b)
{
    switch (op) {
    case T_PLUS: {
        /* if either operand is a string -> concat */
        if (a.type == JS_STRING || b.type == JS_STRING ||
            a.type == JS_OBJECT || b.type == JS_OBJECT ||
            a.type == JS_ARRAY  || b.type == JS_ARRAY) {
            /* ToPrimitive: arrays/objects -> string; then if either is string concat */
            js_value pa = a, pb = b;
            if (a.type==JS_OBJECT||a.type==JS_ARRAY||a.type==JS_FUNCTION)
                pa = js_mk_str(js_to_string(vm, a));
            if (b.type==JS_OBJECT||b.type==JS_ARRAY||b.type==JS_FUNCTION)
                pb = js_mk_str(js_to_string(vm, b));
            if (pa.type == JS_STRING || pb.type == JS_STRING) {
                js_string *sa = js_to_string(vm, pa);
                js_string *sb = js_to_string(vm, pb);
                return js_mk_str(js_str_concat(vm, sa, sb));
            }
            return js_mk_num(js_to_number(vm, pa) + js_to_number(vm, pb));
        }
        return js_mk_num(js_to_number(vm, a) + js_to_number(vm, b));
    }
    case T_MINUS:   return js_mk_num(js_to_number(vm,a) - js_to_number(vm,b));
    case T_STAR:    return js_mk_num(js_to_number(vm,a) * js_to_number(vm,b));
    case T_SLASH:   return js_mk_num(js_to_number(vm,a) / js_to_number(vm,b));
    case T_PERCENT: {
        double x = js_to_number(vm,a), y = js_to_number(vm,b);
        if (y == 0.0 || js_isnan(x) || js_isnan(y) || js_isinf(x)) return js_mk_num(js_nan());
        if (js_isinf(y)) return js_mk_num(x);
        double q = x / y;
        double t = q < 0 ? -js_math_floor(-q) : js_math_floor(q);
        return js_mk_num(x - t * y);
    }
    case T_STARSTAR: return js_mk_num(js_math_pow(js_to_number(vm,a), js_to_number(vm,b)));

    case T_EQ:   return js_mk_bool(js_loose_eq(vm, a, b));
    case T_NEQ:  return js_mk_bool(!js_loose_eq(vm, a, b));
    case T_SEQ:  return js_mk_bool(js_strict_eq(a, b));
    case T_SNEQ: return js_mk_bool(!js_strict_eq(a, b));

    case T_LT: case T_LE: case T_GT: case T_GE: {
        /* string vs string -> lexicographic; else numeric */
        if (a.type == JS_STRING && b.type == JS_STRING) {
            js_string *sa = a.u.s, *sb = b.u.s;
            js_usize m = sa->len < sb->len ? sa->len : sb->len;
            int cmp = js_memcmp(sa->data, sb->data, m);
            if (cmp == 0) cmp = (sa->len < sb->len) ? -1 : (sa->len > sb->len ? 1 : 0);
            switch (op) {
            case T_LT: return js_mk_bool(cmp < 0);
            case T_LE: return js_mk_bool(cmp <= 0);
            case T_GT: return js_mk_bool(cmp > 0);
            default:   return js_mk_bool(cmp >= 0);
            }
        }
        double x = js_to_number(vm,a), y = js_to_number(vm,b);
        if (js_isnan(x) || js_isnan(y)) return js_mk_bool(0);
        switch (op) {
        case T_LT: return js_mk_bool(x < y);
        case T_LE: return js_mk_bool(x <= y);
        case T_GT: return js_mk_bool(x > y);
        default:   return js_mk_bool(x >= y);
        }
    }

    case T_BAND: return js_mk_num((double)(to_int32(js_to_number(vm,a)) & to_int32(js_to_number(vm,b))));
    case T_BOR:  return js_mk_num((double)(to_int32(js_to_number(vm,a)) | to_int32(js_to_number(vm,b))));
    case T_BXOR: return js_mk_num((double)(to_int32(js_to_number(vm,a)) ^ to_int32(js_to_number(vm,b))));
    case T_SHL:  return js_mk_num((double)(to_int32(js_to_number(vm,a)) << (to_uint32(js_to_number(vm,b)) & 31)));
    case T_SHR:  return js_mk_num((double)(to_int32(js_to_number(vm,a)) >> (to_uint32(js_to_number(vm,b)) & 31)));
    case T_USHR: return js_mk_num((double)(to_uint32(js_to_number(vm,a)) >> (to_uint32(js_to_number(vm,b)) & 31)));

    case T_INSTANCEOF: {
        /* a instanceof b : walk a's proto chain looking for b.prototype */
        if (b.type != JS_FUNCTION) return js_mk_bool(0);
        if (a.type != JS_OBJECT && a.type != JS_ARRAY && a.type != JS_FUNCTION)
            return js_mk_bool(0);
        js_object *target = func_get_prototype(vm, b);
        if (!target) return js_mk_bool(0);
        js_object *p = a.u.o->proto;
        int guard = 0;
        while (p && guard++ < 64) {
            if (p == target) return js_mk_bool(1);
            p = p->proto;
        }
        return js_mk_bool(0);
    }
    case T_IN: {
        if (b.type == JS_OBJECT || b.type == JS_ARRAY || b.type == JS_FUNCTION) {
            js_string *k = js_to_string(vm, a);
            if (b.type == JS_ARRAY) {
                js_usize idx; int isidx = 0;
                /* check numeric index within length */
                int ok = 0; double d = js_parse_double(k->data, k->len, &ok);
                if (ok) { idx = (js_usize)d; isidx = (d>=0 && d==(double)idx); }
                if (isidx && idx < b.u.o->length) return js_mk_bool(1);
            }
            return js_mk_bool(js_obj_has_own(b.u.o, k));
        }
        return js_mk_bool(0);
    }
    default:
        return js_mk_undef();
    }
}

/* ------------------------------------------------------------------ */
/*  Calling functions (native + script closures)                      */
/* ------------------------------------------------------------------ */
js_completion js_call_function(js_vm *vm, js_value fnv, js_value thisv,
                               js_value *argv, int argc, js_value *out)
{
    *out = js_mk_undef();
    if (fnv.type != JS_FUNCTION) {
        js_throw_str(vm, "value is not a function");
        return CMP_THROW;
    }
    js_func *f = fnv.u.o->fn;

    if (vm->depth >= JS_MAX_CALL_DEPTH) {
        js_throw_str(vm, "Maximum call stack size exceeded");
        return CMP_THROW;
    }
    vm->depth++;

    js_completion result = CMP_NORMAL;

    if (f->native) {
        int rc = f->native(vm, thisv, argv, argc, out);
        if (rc < 0 || vm->has_exception) result = CMP_THROW;
        vm->depth--;
        return result;
    }

    /* script function: new environment chained to closure */
    js_env *fenv = js_env_new(vm, f->closure);
    if (!fenv) { vm->depth--; js_throw_str(vm, "out of memory"); return CMP_THROW; }

    /* bind parameters */
    if (f->params) {
        for (int i = 0; i < f->params->nkids; i++) {
            js_string *pname = f->params->kids[i]->str;
            js_value pv = (i < argc) ? argv[i] : js_mk_undef();
            js_env_define(vm, fenv, pname, pv, 0);
        }
    }

    /* bind `this` and `arguments` (arrow funcs inherit lexically -> skip) */
    if (!f->is_arrow) {
        js_env_define(vm, fenv, js_str_intern(vm, "this", 4), thisv, 0);
        js_object *args = js_array_new(vm);
        if (args) {
            for (int i = 0; i < argc; i++) js_arr_push(vm, args, argv[i]);
            js_env_define(vm, fenv, js_str_intern(vm, "arguments", 9),
                          js_mk_obj(args), 0);
        }
    }

    /* execute body */
    js_value bodyret = js_mk_undef();
    js_completion c;
    if (f->body && f->body->kind == NODE_BLOCK) {
        c = eval_block(vm, f->body, fenv, &bodyret, 0);
    } else {
        /* arrow expression body */
        c = eval_expr(vm, f->body, fenv, &bodyret);
        if (c == CMP_NORMAL) { *out = bodyret; vm->depth--; return CMP_NORMAL; }
    }

    if (c == CMP_RETURN) { *out = bodyret; result = CMP_NORMAL; }
    else if (c == CMP_THROW) { result = CMP_THROW; }
    else { *out = js_mk_undef(); result = CMP_NORMAL; }

    vm->depth--;
    return result;
}

/* ------------------------------------------------------------------ */
/*  Expression evaluation                                              */
/* ------------------------------------------------------------------ */
static js_completion eval_expr(js_vm *vm, js_node *n, js_env *env, js_value *out)
{
    *out = js_mk_undef();
    if (!n) return CMP_NORMAL;

    switch (n->kind) {
    case NODE_NUMBER:    *out = js_mk_num(n->num); return CMP_NORMAL;
    case NODE_STRING:    *out = js_mk_str(n->str); return CMP_NORMAL;
    case NODE_BOOL:      *out = js_mk_bool(n->b); return CMP_NORMAL;
    case NODE_NULL:      *out = js_mk_null(); return CMP_NORMAL;
    case NODE_UNDEFINED: *out = js_mk_undef(); return CMP_NORMAL;
    case NODE_THIS: {
        js_value tv;
        if (js_env_get(env, js_str_intern(vm,"this",4), &tv)) *out = tv;
        return CMP_NORMAL;
    }
    case NODE_IDENT: {
        js_value v;
        if (!js_env_get(env, n->str, &v)) {
            /* undefined identifier reference -> ReferenceError */
            char msg[160];
            js_usize p = 0;
            const char *pre = "ReferenceError: ";
            while (pre[p]) { msg[p]=pre[p]; p++; }
            for (js_usize i=0;i<n->str->len && p<150;i++) msg[p++]=n->str->data[i];
            const char *suf = " is not defined";
            for (js_usize i=0; suf[i] && p<159; i++) msg[p++]=suf[i];
            msg[p]=0;
            js_throw_str(vm, msg);
            return CMP_THROW;
        }
        *out = v;
        return CMP_NORMAL;
    }

    case NODE_TEMPLATE:  *out = js_mk_str(n->str); return CMP_NORMAL;

    case NODE_ARRAY: {
        js_object *a = js_array_new(vm);
        if (!a) { js_throw_str(vm,"out of memory"); return CMP_THROW; }
        for (int i = 0; i < n->nkids; i++) {
            js_value ev;
            js_completion c = eval_expr(vm, n->kids[i], env, &ev);
            if (c != CMP_NORMAL) return c;
            js_arr_push(vm, a, ev);
        }
        *out = js_mk_obj(a);
        return CMP_NORMAL;
    }

    case NODE_OBJECT: {
        js_object *o = js_object_new(vm);
        if (!o) { js_throw_str(vm,"out of memory"); return CMP_THROW; }
        for (int i = 0; i + 1 < n->nkids; i += 2) {
            js_string *key = n->kids[i]->str;
            js_value val;
            js_completion c = eval_expr(vm, n->kids[i+1], env, &val);
            if (c != CMP_NORMAL) return c;
            js_obj_set(vm, o, key, val);
        }
        *out = js_mk_obj(o);
        return CMP_NORMAL;
    }

    case NODE_FUNCTION:  *out = make_closure(vm, n, env, 0); return CMP_NORMAL;
    case NODE_ARROW:     *out = make_closure(vm, n, env, 1); return CMP_NORMAL;

    case NODE_UNARY: {
        if (n->op == T_TYPEOF) {
            /* typeof on undefined identifier yields "undefined" w/o throwing */
            if (n->a->kind == NODE_IDENT) {
                js_value v;
                if (!js_env_get(env, n->a->str, &v)) {
                    *out = js_mk_strz(vm, "undefined");
                    return CMP_NORMAL;
                }
                *out = js_mk_strz(vm, js_typeof(v));
                return CMP_NORMAL;
            }
        }
        if (n->op == T_DELETE) {
            if (n->a->kind == NODE_MEMBER) {
                js_value obj;
                js_completion c = eval_expr(vm, n->a->a, env, &obj);
                if (c != CMP_NORMAL) return c;
                js_string *key;
                if (n->a->flag & MEMBER_COMPUTED) {
                    js_value kv;
                    c = eval_expr(vm, n->a->b_, env, &kv);
                    if (c != CMP_NORMAL) return c;
                    key = js_to_string(vm, kv);
                } else key = n->a->str;
                if (obj.type==JS_OBJECT||obj.type==JS_ARRAY||obj.type==JS_FUNCTION)
                    js_obj_delete(obj.u.o, key);
                *out = js_mk_bool(1);
                return CMP_NORMAL;
            }
            *out = js_mk_bool(1);
            return CMP_NORMAL;
        }

        js_value v;
        js_completion c = eval_expr(vm, n->a, env, &v);
        if (c != CMP_NORMAL) return c;
        switch (n->op) {
        case T_MINUS:  *out = js_mk_num(-js_to_number(vm, v)); break;
        case T_PLUS:   *out = js_mk_num(js_to_number(vm, v)); break;
        case T_NOT:    *out = js_mk_bool(!js_truthy(v)); break;
        case T_BNOT:   *out = js_mk_num((double)(~to_int32(js_to_number(vm, v)))); break;
        case T_TYPEOF: *out = js_mk_strz(vm, js_typeof(v)); break;
        case T_VOID:   *out = js_mk_undef(); break;
        default:       *out = js_mk_undef(); break;
        }
        return CMP_NORMAL;
    }

    case NODE_UPDATE: {
        /* ++/-- on an lvalue (ident or member) */
        js_value old;
        js_completion c = eval_expr(vm, n->a, env, &old);
        if (c != CMP_NORMAL) return c;
        double oldn = js_to_number(vm, old);
        double newn = (n->op == T_INC) ? oldn + 1.0 : oldn - 1.0;
        c = assign_to(vm, n->a, js_mk_num(newn), env);
        if (c != CMP_NORMAL) return c;
        *out = js_mk_num((n->flag & UPDATE_POSTFIX) ? oldn : newn);
        return CMP_NORMAL;
    }

    case NODE_BINARY: {
        js_value a, b;
        js_completion c = eval_expr(vm, n->a, env, &a);
        if (c != CMP_NORMAL) return c;
        c = eval_expr(vm, n->b_, env, &b);
        if (c != CMP_NORMAL) return c;
        *out = apply_binop_value(vm, n->op, a, b);
        if (vm->has_exception) return CMP_THROW;
        return CMP_NORMAL;
    }

    case NODE_LOGICAL: {
        js_value a;
        js_completion c = eval_expr(vm, n->a, env, &a);
        if (c != CMP_NORMAL) return c;
        if (n->op == T_AND) {
            if (!js_truthy(a)) { *out = a; return CMP_NORMAL; }
            return eval_expr(vm, n->b_, env, out);
        }
        if (n->op == T_OR) {
            if (js_truthy(a)) { *out = a; return CMP_NORMAL; }
            return eval_expr(vm, n->b_, env, out);
        }
        /* ?? : only null/undefined falls through */
        if (a.type != JS_NULL && a.type != JS_UNDEFINED) { *out = a; return CMP_NORMAL; }
        return eval_expr(vm, n->b_, env, out);
    }

    case NODE_COND: {
        js_value t;
        js_completion c = eval_expr(vm, n->a, env, &t);
        if (c != CMP_NORMAL) return c;
        return eval_expr(vm, js_truthy(t) ? n->b_ : n->c, env, out);
    }

    case NODE_ASSIGN: {
        js_value rhs;
        js_completion c;
        if (n->op == T_ASSIGN) {
            c = eval_expr(vm, n->b_, env, &rhs);
            if (c != CMP_NORMAL) return c;
        } else {
            /* compound: evaluate current target value, then apply op */
            js_value cur;
            c = eval_expr(vm, n->a, env, &cur);
            if (c != CMP_NORMAL) return c;
            js_value r;
            c = eval_expr(vm, n->b_, env, &r);
            if (c != CMP_NORMAL) return c;
            js_tok_kind bop;
            switch (n->op) {
            case T_PLUSEQ:    bop=T_PLUS; break;
            case T_MINUSEQ:   bop=T_MINUS; break;
            case T_STAREQ:    bop=T_STAR; break;
            case T_SLASHEQ:   bop=T_SLASH; break;
            case T_PERCENTEQ: bop=T_PERCENT; break;
            case T_BANDEQ:    bop=T_BAND; break;
            case T_BOREQ:     bop=T_BOR; break;
            case T_BXOREQ:    bop=T_BXOR; break;
            case T_SHLEQ:     bop=T_SHL; break;
            case T_SHREQ:     bop=T_SHR; break;
            case T_USHREQ:    bop=T_USHR; break;
            case T_ANDEQ:
                if (!js_truthy(cur)) { *out = cur; return CMP_NORMAL; }
                rhs = r; goto do_assign;
            case T_OREQ:
                if (js_truthy(cur)) { *out = cur; return CMP_NORMAL; }
                rhs = r; goto do_assign;
            default: bop=T_PLUS; break;
            }
            rhs = apply_binop_value(vm, bop, cur, r);
            if (vm->has_exception) return CMP_THROW;
        }
    do_assign:
        c = assign_to(vm, n->a, rhs, env);
        if (c != CMP_NORMAL) return c;
        *out = rhs;
        return CMP_NORMAL;
    }

    case NODE_SEQ: {
        js_value v = js_mk_undef();
        for (int i = 0; i < n->nkids; i++) {
            js_completion c = eval_expr(vm, n->kids[i], env, &v);
            if (c != CMP_NORMAL) return c;
        }
        *out = v;
        return CMP_NORMAL;
    }

    case NODE_MEMBER: {
        js_value obj;
        js_completion c = eval_expr(vm, n->a, env, &obj);
        if (c != CMP_NORMAL) return c;
        js_string *key;
        if (n->flag & MEMBER_COMPUTED) {
            js_value kv;
            c = eval_expr(vm, n->b_, env, &kv);
            if (c != CMP_NORMAL) return c;
            key = js_to_string(vm, kv);
        } else key = n->str;
        if (obj.type == JS_NULL || obj.type == JS_UNDEFINED) {
            js_throw_str(vm, "Cannot read property of null or undefined");
            return CMP_THROW;
        }
        js_get_prop(vm, obj, key, out);
        return CMP_NORMAL;
    }

    case NODE_CALL: {
        /* resolve callee + this */
        js_value thisv = js_mk_undef();
        js_value fnv;
        js_node *callee = n->a;
        if (callee->kind == NODE_MEMBER) {
            js_value obj;
            js_completion c = eval_expr(vm, callee->a, env, &obj);
            if (c != CMP_NORMAL) return c;
            js_string *key;
            if (callee->flag & MEMBER_COMPUTED) {
                js_value kv;
                c = eval_expr(vm, callee->b_, env, &kv);
                if (c != CMP_NORMAL) return c;
                key = js_to_string(vm, kv);
            } else key = callee->str;
            if (obj.type == JS_NULL || obj.type == JS_UNDEFINED) {
                js_throw_str(vm, "Cannot call method of null or undefined");
                return CMP_THROW;
            }
            js_get_prop(vm, obj, key, &fnv);
            thisv = obj;
        } else {
            js_completion c = eval_expr(vm, callee, env, &fnv);
            if (c != CMP_NORMAL) return c;
        }

        if (fnv.type != JS_FUNCTION) {
            js_throw_str(vm, "called value is not a function");
            return CMP_THROW;
        }

        /* evaluate args */
        js_value argv[32];
        int argc = n->nkids;
        if (argc > 32) argc = 32;
        for (int i = 0; i < argc; i++) {
            js_completion c = eval_expr(vm, n->kids[i], env, &argv[i]);
            if (c != CMP_NORMAL) return c;
        }
        return js_call_function(vm, fnv, thisv, argv, argc, out);
    }

    case NODE_NEW: {
        js_value fnv;
        js_completion c = eval_expr(vm, n->a, env, &fnv);
        if (c != CMP_NORMAL) return c;
        if (fnv.type != JS_FUNCTION) {
            js_throw_str(vm, "new called on non-constructor");
            return CMP_THROW;
        }
        /* create new object whose proto is the constructor's .prototype,
         * then call the constructor with it as `this` */
        js_object *obj = js_object_new(vm);
        if (obj) {
            js_object *cproto = func_get_prototype(vm, fnv);
            if (cproto) obj->proto = cproto;
        }
        js_value thisv = js_mk_obj(obj);
        js_value argv[32];
        int argc = n->nkids; if (argc>32) argc=32;
        for (int i = 0; i < argc; i++) {
            c = eval_expr(vm, n->kids[i], env, &argv[i]);
            if (c != CMP_NORMAL) return c;
        }
        js_value r;
        c = js_call_function(vm, fnv, thisv, argv, argc, &r);
        if (c != CMP_NORMAL) return c;
        /* if constructor returns an object, use it; else use this */
        if (r.type==JS_OBJECT||r.type==JS_ARRAY||r.type==JS_FUNCTION) *out = r;
        else *out = thisv;
        return CMP_NORMAL;
    }

    default:
        return CMP_NORMAL;
    }
}

/* expose eval_expr through internal name used by builtins (callbacks) */
js_completion js_eval_node(js_vm *vm, js_node *n, js_env *env, js_value *out)
{
    return eval_expr(vm, n, env, out);
}

/* ------------------------------------------------------------------ */
/*  Statement / block execution                                       */
/* ------------------------------------------------------------------ */
static void hoist(js_vm *vm, js_node **stmts, int n, js_env *env)
{
    for (int i = 0; i < n; i++) {
        js_node *s = stmts[i];
        if (s && s->kind == NODE_FUNCDECL) {
            js_value fn = make_closure(vm, s, env, 0);
            js_env_define(vm, env, s->str, fn, 0);
        }
    }
}

static js_completion exec_stmt(js_vm *vm, js_node *n, js_env *env, js_value *out);

static js_completion eval_block(js_vm *vm, js_node *block, js_env *env,
                                js_value *out, int new_scope)
{
    js_env *benv = new_scope ? js_env_new(vm, env) : env;
    if (!benv) { js_throw_str(vm,"out of memory"); return CMP_THROW; }

    /* declaration-group blocks (var a, b) share scope, no hoist */
    if (!(block->flag & 1)) {
        hoist(vm, block->kids, block->nkids, benv);
    }

    js_value last = js_mk_undef();
    for (int i = 0; i < block->nkids; i++) {
        js_completion c = exec_stmt(vm, block->kids[i], benv, &last);
        if (c != CMP_NORMAL) { *out = last; return c; }
    }
    *out = last;
    return CMP_NORMAL;
}

static js_completion exec_stmt(js_vm *vm, js_node *n, js_env *env, js_value *out)
{
    *out = js_mk_undef();
    if (!n) return CMP_NORMAL;

    switch (n->kind) {
    case NODE_EMPTY: return CMP_NORMAL;

    case NODE_EXPRSTMT:
        return eval_expr(vm, n->a, env, out);

    case NODE_VARDECL: {
        js_value v = js_mk_undef();
        if (n->a) {
            js_completion c = eval_expr(vm, n->a, env, &v);
            if (c != CMP_NORMAL) return c;
        }
        js_env_define(vm, env, n->str, v, n->flag == VK_CONST);
        if (vm->has_exception) return CMP_THROW;
        return CMP_NORMAL;
    }

    case NODE_BLOCK:
        /* declaration group: same scope; real block: new scope */
        return eval_block(vm, n, env, out, !(n->flag & 1));

    case NODE_FUNCDECL:
        /* already hoisted; nothing to do at execution point */
        return CMP_NORMAL;

    case NODE_IF: {
        js_value t;
        js_completion c = eval_expr(vm, n->a, env, &t);
        if (c != CMP_NORMAL) return c;
        if (js_truthy(t)) return exec_stmt(vm, n->b_, env, out);
        else if (n->c) return exec_stmt(vm, n->c, env, out);
        return CMP_NORMAL;
    }

    case NODE_WHILE: {
        for (;;) {
            js_value t;
            js_completion c = eval_expr(vm, n->a, env, &t);
            if (c != CMP_NORMAL) return c;
            if (!js_truthy(t)) break;
            js_value bv;
            c = exec_stmt(vm, n->b_, env, &bv);
            if (c == CMP_BREAK) break;
            if (c == CMP_CONTINUE) continue;
            if (c == CMP_RETURN || c == CMP_THROW) { *out = bv; return c; }
        }
        return CMP_NORMAL;
    }

    case NODE_DOWHILE: {
        for (;;) {
            js_value bv;
            js_completion c = exec_stmt(vm, n->b_, env, &bv);
            if (c == CMP_BREAK) break;
            if (c == CMP_RETURN || c == CMP_THROW) { *out = bv; return c; }
            if (c != CMP_CONTINUE) { /* fallthrough to cond */ }
            js_value t;
            c = eval_expr(vm, n->a, env, &t);
            if (c != CMP_NORMAL) return c;
            if (!js_truthy(t)) break;
        }
        return CMP_NORMAL;
    }

    case NODE_FOR: {
        js_env *fenv = js_env_new(vm, env);
        if (!fenv) { js_throw_str(vm,"oom"); return CMP_THROW; }
        if (n->a) {
            js_value iv;
            js_completion c = exec_stmt(vm, n->a, fenv, &iv);
            if (c != CMP_NORMAL) return c;
        }
        for (;;) {
            if (n->b_) {
                js_value t;
                js_completion c = eval_expr(vm, n->b_, fenv, &t);
                if (c != CMP_NORMAL) return c;
                if (!js_truthy(t)) break;
            }
            js_value bv;
            js_completion c = exec_stmt(vm, n->d, fenv, &bv);
            if (c == CMP_BREAK) break;
            if (c == CMP_RETURN || c == CMP_THROW) { *out = bv; return c; }
            /* continue falls through to post */
            if (n->c) {
                js_value pv;
                c = eval_expr(vm, n->c, fenv, &pv);
                if (c != CMP_NORMAL) return c;
            }
        }
        return CMP_NORMAL;
    }

    case NODE_FORIN: {
        js_value iter;
        js_completion c = eval_expr(vm, n->a, env, &iter);
        if (c != CMP_NORMAL) return c;
        int is_of = (n->flag & FORIN_OF);
        js_env *fenv = js_env_new(vm, env);
        if (!fenv) { js_throw_str(vm,"oom"); return CMP_THROW; }
        js_env_define(vm, fenv, n->str, js_mk_undef(), 0);

        if (iter.type == JS_ARRAY) {
            js_object *a = iter.u.o;
            for (js_usize i = 0; i < a->length; i++) {
                js_value item = is_of ? a->elems[i]
                                      : js_mk_str(js_num_to_str(vm, (double)i));
                js_env_set(vm, fenv, n->str, item);
                js_value bv;
                c = exec_stmt(vm, n->b_, fenv, &bv);
                if (c == CMP_BREAK) break;
                if (c == CMP_RETURN || c == CMP_THROW) { *out=bv; return c; }
            }
        } else if (iter.type == JS_STRING && is_of) {
            for (js_usize i = 0; i < iter.u.s->len; i++) {
                js_env_set(vm, fenv, n->str, js_mk_str(js_str_new(vm, iter.u.s->data+i,1)));
                js_value bv;
                c = exec_stmt(vm, n->b_, fenv, &bv);
                if (c == CMP_BREAK) break;
                if (c == CMP_RETURN || c == CMP_THROW) { *out=bv; return c; }
            }
        } else if (iter.type == JS_OBJECT) {
            js_object *o = iter.u.o;
            js_prop *ord[256];
            js_usize cnt = js_obj_ordered(o, ord, 256);
            for (js_usize i = 0; i < cnt; i++) {
                js_prop *pr = ord[i];
                if (!pr->enumerable) continue;
                js_value item = is_of ? pr->val : js_mk_str(pr->key);
                js_env_set(vm, fenv, n->str, item);
                js_value bv;
                c = exec_stmt(vm, n->b_, fenv, &bv);
                if (c == CMP_BREAK) break;
                if (c == CMP_RETURN || c == CMP_THROW) { *out=bv; return c; }
            }
        }
        return CMP_NORMAL;
    }

    case NODE_RETURN: {
        if (n->a) {
            js_completion c = eval_expr(vm, n->a, env, out);
            if (c != CMP_NORMAL) return c;
        }
        return CMP_RETURN;
    }

    case NODE_BREAK:    return CMP_BREAK;
    case NODE_CONTINUE: return CMP_CONTINUE;

    case NODE_THROW: {
        js_value v;
        js_completion c = eval_expr(vm, n->a, env, &v);
        if (c != CMP_NORMAL) return c;
        js_throw(vm, v);
        return CMP_THROW;
    }

    case NODE_TRY: {
        js_value bv;
        js_completion c = eval_block(vm, n->a, env, &bv, 1);
        if (c == CMP_THROW && n->b_) {
            /* run catch with the exception bound */
            js_value exc = vm->exception;
            vm->has_exception = 0;
            js_env *cenv = js_env_new(vm, env);
            if (n->str) js_env_define(vm, cenv, n->str, exc, 0);
            c = eval_block(vm, n->b_, cenv, &bv, 0);
        }
        /* finally always runs */
        if (n->c) {
            js_value fv;
            js_completion fc = eval_block(vm, n->c, env, &fv, 1);
            if (fc != CMP_NORMAL) return fc;  /* finally overrides */
        }
        *out = bv;
        return c;
    }

    default:
        /* treat unknown as expression */
        return eval_expr(vm, n, env, out);
    }
}

/* ------------------------------------------------------------------ */
/*  Program entry                                                      */
/* ------------------------------------------------------------------ */
int js_run_program(js_vm *vm, js_node *prog, js_value *completion)
{
    *completion = js_mk_undef();
    vm->has_exception = 0;
    vm->depth = 0;

    /* hoist top-level function declarations */
    hoist(vm, prog->kids, prog->nkids, vm->global_env);

    js_value last = js_mk_undef();
    for (int i = 0; i < prog->nkids; i++) {
        js_value v;
        js_node *st = prog->kids[i];
        js_completion c = exec_stmt(vm, st, vm->global_env, &v);
        if (c == CMP_THROW) {
            *completion = vm->exception;
            return -1;
        }
        /*
         * REPL completion value: record the value of statements that have a
         * meaningful completion (expression statements plus value-producing
         * compound statements like if/try/blocks). Declarations, loops, and
         * empty statements have empty completion values and don't clobber it.
         */
        switch (st->kind) {
        case NODE_EXPRSTMT:
        case NODE_IF:
        case NODE_TRY:
            last = v;
            break;
        case NODE_BLOCK:
            if (!(st->flag & 1)) last = v;   /* not a var-declaration group */
            break;
        default:
            break;
        }
        if (c == CMP_RETURN) { last = v; break; }
        if (c == CMP_BREAK || c == CMP_CONTINUE) {
            /* illegal at top level; ignore */
        }
    }
    *completion = last;
    return 0;
}

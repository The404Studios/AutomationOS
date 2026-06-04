/*
 * js_builtin.c -- built-in objects/functions + VM lifecycle + selftest.
 * ====================================================================
 *
 * Installs the standard library subset onto the global environment and the
 * shared prototype objects:
 *
 *   console.log / console.error
 *   Math.{PI,E,abs,floor,ceil,round,trunc,sqrt,pow,exp,log,min,max,random,
 *         sign,cbrt,sin,cos,tan,atan,atan2,hypot}
 *   JSON.parse / JSON.stringify
 *   String.prototype.{charAt,charCodeAt,indexOf,lastIndexOf,slice,substring,
 *         substr,split,toUpperCase,toLowerCase,trim,replace(literal),
 *         repeat,includes,startsWith,endsWith,concat,padStart}
 *   Array.prototype.{push,pop,shift,unshift,join,indexOf,includes,slice,
 *         concat,reverse,map,filter,forEach,reduce,some,every,find,fill,sort}
 *   Object.{keys,values,entries,assign,freeze}
 *   Number, String, Boolean, Array (conversion/constructors)
 *   parseInt, parseFloat, isNaN, isFinite, NaN, Infinity, undefined
 *
 * Also defines the public js_new/js_eval/js_set_print/js_selftest and the
 * hand-rolled math primitives (no libm).
 *
 * Memory: a single static js_vm lives in BSS (the multi-MB arena is inside it).
 */

#include "js_internal.h"

/* ================================================================== */
/*  The one static VM instance                                        */
/* ================================================================== */
static js_vm g_vm;

/* ================================================================== */
/*  Math primitives (no libm)                                         */
/* ================================================================== */
double js_math_abs(double x) { return x < 0 ? -x : x; }

double js_math_floor(double x)
{
    if (js_isnan(x) || js_isinf(x)) return x;
    double t = (double)(js_i64)x;
    if (t > x) t -= 1.0;
    return t;
}
double js_math_ceil(double x)
{
    if (js_isnan(x) || js_isinf(x)) return x;
    double t = (double)(js_i64)x;
    if (t < x) t += 1.0;
    return t;
}
static double math_trunc(double x)
{
    if (js_isnan(x) || js_isinf(x)) return x;
    return (double)(js_i64)x;
}
static double math_round(double x)
{
    if (js_isnan(x) || js_isinf(x)) return x;
    return js_math_floor(x + 0.5);
}

double js_math_sqrt(double x)
{
    if (x < 0) return js_nan();
    if (x == 0.0 || js_isnan(x) || js_isinf(x)) return x;
    /* Newton-Raphson */
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        double ng = 0.5 * (g + x / g);
        if (ng == g) break;
        g = ng;
    }
    return g;
}

/* exp via range reduction + Taylor */
static double math_exp(double x)
{
    if (js_isnan(x)) return x;
    if (x > 709.0) return js_inf(0);
    if (x < -745.0) return 0.0;
    /* exp(x) = 2^k * exp(r), k = round(x/ln2), r = x - k*ln2 */
    const double LN2 = 0.6931471805599453;
    double k = math_round(x / LN2);
    double r = x - k * LN2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 30; i++) {
        term *= r / i;
        sum += term;
        if (js_math_abs(term) < 1e-18) break;
    }
    /* multiply by 2^k */
    double p = 1.0;
    int ik = (int)k;
    double base = ik >= 0 ? 2.0 : 0.5;
    int n = ik >= 0 ? ik : -ik;
    for (int i = 0; i < n; i++) p *= base;
    return sum * p;
}

/* natural log via atanh series */
static double math_log(double x)
{
    if (x < 0 || js_isnan(x)) return js_nan();
    if (x == 0.0) return js_inf(1);
    if (js_isinf(x)) return x;
    /* reduce to m in [1,2): x = m * 2^e */
    int e = 0;
    double m = x;
    while (m >= 2.0) { m /= 2.0; e++; }
    while (m < 1.0)  { m *= 2.0; e--; }
    /* log(m) using (m-1)/(m+1) series */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double sum = 0.0, term = t;
    for (int i = 1; i < 60; i += 2) {
        sum += term / i;
        term *= t2;
        if (js_math_abs(term) < 1e-18) break;
    }
    const double LN2 = 0.6931471805599453;
    return 2.0 * sum + e * LN2;
}

double js_math_pow(double b, double e)
{
    if (e == 0.0) return 1.0;
    if (js_isnan(b) || js_isnan(e)) return js_nan();
    /* integer exponent fast path */
    if (e == math_trunc(e) && js_math_abs(e) < 1024.0) {
        int neg = e < 0;
        long n = (long)(neg ? -e : e);
        double r = 1.0, base = b;
        while (n) {
            if (n & 1) r *= base;
            base *= base;
            n >>= 1;
        }
        return neg ? 1.0 / r : r;
    }
    if (b < 0) return js_nan();   /* non-integer power of negative */
    if (b == 0.0) return 0.0;
    return math_exp(e * math_log(b));
}

/* trig: range-reduce mod 2pi, Taylor series */
static const double JS_PI = 3.141592653589793;
static const double JS_2PI = 6.283185307179586;

static double reduce_angle(double x)
{
    /* bring x into [-pi, pi] */
    double n = math_round(x / JS_2PI);
    return x - n * JS_2PI;
}
double js_math_sin(double x)
{
    if (js_isnan(x) || js_isinf(x)) return js_nan();
    x = reduce_angle(x);
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 20; i++) {
        term *= -x2 / ((2*i) * (2*i+1));
        sum += term;
        if (js_math_abs(term) < 1e-17) break;
    }
    return sum;
}
double js_math_cos(double x)
{
    if (js_isnan(x) || js_isinf(x)) return js_nan();
    x = reduce_angle(x);
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int i = 1; i < 20; i++) {
        term *= -x2 / ((2*i-1) * (2*i));
        sum += term;
        if (js_math_abs(term) < 1e-17) break;
    }
    return sum;
}
static double math_tan(double x) { double c = js_math_cos(x); return c==0?js_nan():js_math_sin(x)/c; }
static double math_atan(double x)
{
    /* atan via series with reduction for |x|>1 */
    int sign = x < 0 ? -1 : 1;
    double ax = js_math_abs(x);
    int recip = 0;
    if (ax > 1.0) { ax = 1.0/ax; recip = 1; }
    double term = ax, sum = ax, x2 = ax*ax;
    for (int i = 1; i < 60; i++) {
        term *= -x2;
        sum += term / (2*i+1);
        if (js_math_abs(term/(2*i+1)) < 1e-17) break;
    }
    if (recip) sum = JS_PI/2.0 - sum;
    return sign * sum;
}
static double math_atan2(double y, double x)
{
    if (x > 0) return math_atan(y/x);
    if (x < 0 && y >= 0) return math_atan(y/x) + JS_PI;
    if (x < 0 && y < 0)  return math_atan(y/x) - JS_PI;
    if (x == 0 && y > 0) return JS_PI/2.0;
    if (x == 0 && y < 0) return -JS_PI/2.0;
    return 0.0;
}

/* ================================================================== */
/*  Convenience for native fns                                        */
/* ================================================================== */
static js_value arg(js_value *argv, int argc, int i)
{
    return i < argc ? argv[i] : js_mk_undef();
}

static void out_emit(js_vm *vm, const char *s, js_usize n)
{
    if (vm->emit) vm->emit(s, n);
}

/* register a native onto an object */
static void reg_method(js_vm *vm, js_object *o, const char *name, js_native_fn fn)
{
    js_object *f = js_func_new_native(vm, fn, name);
    if (f) js_obj_set(vm, o, js_str_intern(vm, name, js_strlen(name)), js_mk_obj(f));
}
static void reg_global(js_vm *vm, const char *name, js_native_fn fn)
{
    js_object *f = js_func_new_native(vm, fn, name);
    if (f) js_env_define(vm, vm->global_env,
                         js_str_intern(vm, name, js_strlen(name)),
                         js_mk_obj(f), 0);
}
/* Install fn.prototype = proto as a NON-enumerable property (matches ES, so it
 * stays out of Object.keys(fn)/for-in). Used to link builtin constructors to
 * their shared prototype objects for `instanceof`. */
static void set_proto_prop(js_vm *vm, js_object *fn, js_object *proto)
{
    js_string *kp = js_str_intern(vm, "prototype", 9);
    js_obj_set(vm, fn, kp, js_mk_obj(proto));
    js_prop *ord[8];
    js_usize cnt = js_obj_ordered(fn, ord, 8);
    for (js_usize i = 0; i < cnt; i++)
        if (js_str_eq(ord[i]->key, kp)) { ord[i]->enumerable = 0; break; }
}

/* ================================================================== */
/*  console                                                           */
/* ================================================================== */
static int native_console_log(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    for (int i = 0; i < n; i++) {
        if (i) out_emit(vm, " ", 1);
        js_string *s = js_value_to_display(vm, a[i]);
        if (s) out_emit(vm, s->data, s->len);
    }
    out_emit(vm, "\n", 1);
    *out = js_mk_undef();
    return 0;
}

/* ================================================================== */
/*  Math natives                                                      */
/* ================================================================== */
#define MATH1(fn, expr) \
static int fn(js_vm *vm, js_value t, js_value *a, int n, js_value *out) { \
    (void)t; double x = js_to_number(vm, arg(a,n,0)); *out = js_mk_num(expr); return 0; }

MATH1(m_abs,  js_math_abs(x))
MATH1(m_floor,js_math_floor(x))
MATH1(m_ceil, js_math_ceil(x))
MATH1(m_round,math_round(x))
MATH1(m_trunc,math_trunc(x))
MATH1(m_sqrt, js_math_sqrt(x))
MATH1(m_exp,  math_exp(x))
MATH1(m_log,  math_log(x))
MATH1(m_sin,  js_math_sin(x))
MATH1(m_cos,  js_math_cos(x))
MATH1(m_tan,  math_tan(x))
MATH1(m_atan, math_atan(x))
MATH1(m_sign, (x>0?1.0:(x<0?-1.0:x)))
MATH1(m_cbrt, (x<0? -js_math_pow(-x,1.0/3.0) : js_math_pow(x,1.0/3.0)))
MATH1(m_log2,  (math_log(x) / 0.6931471805599453))
MATH1(m_log10, (math_log(x) / 2.302585092994046))

static int m_pow(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_num(js_math_pow(js_to_number(vm,arg(a,n,0)), js_to_number(vm,arg(a,n,1)))); return 0; }
static int m_atan2(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_num(math_atan2(js_to_number(vm,arg(a,n,0)), js_to_number(vm,arg(a,n,1)))); return 0; }
static int m_min(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; double r = js_inf(0); for (int i=0;i<n;i++){ double x=js_to_number(vm,a[i]); if(js_isnan(x)){*out=js_mk_num(js_nan());return 0;} if(x<r)r=x;} *out=js_mk_num(r); return 0; }
static int m_max(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; double r = js_inf(1); for (int i=0;i<n;i++){ double x=js_to_number(vm,a[i]); if(js_isnan(x)){*out=js_mk_num(js_nan());return 0;} if(x>r)r=x;} *out=js_mk_num(r); return 0; }
static int m_hypot(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; double s=0; for(int i=0;i<n;i++){double x=js_to_number(vm,a[i]); s+=x*x;} *out=js_mk_num(js_math_sqrt(s)); return 0; }
static int m_random(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;(void)a;(void)n;
    /* xorshift64 */
    js_u64 x = vm->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    vm->rng = x;
    double r = (double)(x >> 11) / 9007199254740992.0;  /* 2^53 */
    *out = js_mk_num(r);
    return 0;
}

/* ================================================================== */
/*  Global numeric helpers                                            */
/* ================================================================== */
static int g_parseInt(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *s = js_to_string(vm, arg(a,n,0));
    int radix = 0;
    if (n > 1 && a[1].type != JS_UNDEFINED) radix = (int)js_to_number(vm, a[1]);
    js_usize i = 0;
    while (i < s->len && (s->data[i]==' '||s->data[i]=='\t'||s->data[i]=='\n')) i++;
    int neg = 0;
    if (i < s->len && (s->data[i]=='+'||s->data[i]=='-')) { neg = s->data[i]=='-'; i++; }
    if ((radix == 16 || radix == 0) && i+1 < s->len &&
        s->data[i]=='0' && (s->data[i+1]=='x'||s->data[i+1]=='X')) {
        i += 2; radix = 16;
    }
    if (radix == 0) radix = 10;
    double val = 0; int any = 0;
    for (; i < s->len; i++) {
        char c = s->data[i];
        int d;
        if (c>='0'&&c<='9') d = c-'0';
        else if (c>='a'&&c<='z') d = c-'a'+10;
        else if (c>='A'&&c<='Z') d = c-'A'+10;
        else break;
        if (d >= radix) break;
        val = val * radix + d;
        any = 1;
    }
    *out = any ? js_mk_num(neg ? -val : val) : js_mk_num(js_nan());
    return 0;
}
static int g_parseFloat(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *s = js_to_string(vm, arg(a,n,0));
    js_usize i = 0;
    while (i < s->len && (s->data[i]==' '||s->data[i]=='\t'||s->data[i]=='\n')) i++;
    /* find the longest numeric prefix */
    js_usize start = i, j = i;
    if (j < s->len && (s->data[j]=='+'||s->data[j]=='-')) j++;
    int digits=0;
    while (j < s->len && s->data[j]>='0'&&s->data[j]<='9') { j++; digits=1; }
    if (j < s->len && s->data[j]=='.') { j++; while (j<s->len&&s->data[j]>='0'&&s->data[j]<='9'){j++;digits=1;} }
    if (digits && j < s->len && (s->data[j]=='e'||s->data[j]=='E')) {
        js_usize k=j+1; if (k<s->len&&(s->data[k]=='+'||s->data[k]=='-'))k++;
        int ed=0; while(k<s->len&&s->data[k]>='0'&&s->data[k]<='9'){k++;ed=1;}
        if (ed) j=k;
    }
    if (!digits) { *out = js_mk_num(js_nan()); return 0; }
    int ok=0;
    double d = js_parse_double(s->data+start, j-start, &ok);
    *out = ok ? js_mk_num(d) : js_mk_num(js_nan());
    return 0;
}
static int g_isNaN(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_bool(js_isnan(js_to_number(vm, arg(a,n,0)))); return 0; }
static int g_isFinite(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_bool(js_isfinite(js_to_number(vm, arg(a,n,0)))); return 0; }

/* Number(x) / String(x) / Boolean(x) conversion functions */
static int g_Number(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_num(n? js_to_number(vm,a[0]) : 0.0); return 0; }
static int g_String(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t; *out = js_mk_str(n? js_to_string(vm,a[0]) : js_str_newz(vm,"")); return 0; }
static int g_Boolean(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t;(void)vm; *out = js_mk_bool(n? js_truthy(a[0]) : 0); return 0; }
static int g_Array(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *arr = js_array_new(vm);
    if (n == 1 && a[0].type == JS_NUMBER) {
        js_usize len = (js_usize)a[0].u.n;
        for (js_usize i=0;i<len;i++) js_arr_push(vm, arr, js_mk_undef());
    } else {
        for (int i=0;i<n;i++) js_arr_push(vm, arr, a[i]);
    }
    *out = js_mk_obj(arr);
    return 0;
}

/* Number.* static predicates (no coercion: argument must already be a number) */
static int n_isInteger(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;(void)vm;
    js_value v = arg(a,n,0);
    int r = (v.type==JS_NUMBER) && js_isfinite(v.u.n) && v.u.n==js_math_floor(v.u.n);
    *out = js_mk_bool(r); return 0;
}
static int n_isFinite(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;(void)vm;
    js_value v = arg(a,n,0);
    *out = js_mk_bool(v.type==JS_NUMBER && js_isfinite(v.u.n)); return 0;
}
static int n_isNaN(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;(void)vm;
    js_value v = arg(a,n,0);
    *out = js_mk_bool(v.type==JS_NUMBER && js_isnan(v.u.n)); return 0;
}
static int n_isSafeInteger(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;(void)vm;
    js_value v = arg(a,n,0);
    int r = (v.type==JS_NUMBER) && js_isfinite(v.u.n) &&
            v.u.n==js_math_floor(v.u.n) && js_math_abs(v.u.n) <= 9007199254740991.0;
    *out = js_mk_bool(r); return 0;
}

/* ================================================================== */
/*  String.prototype methods (this is a string value)                */
/* ================================================================== */
static js_string *this_str(js_vm *vm, js_value t) { return js_to_string(vm, t); }

static int s_charAt(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize i = (js_isize)js_to_number(vm, arg(a,n,0));
    if (i < 0 || (js_usize)i >= s->len) { *out = js_mk_strz(vm, ""); return 0; }
    *out = js_mk_str(js_str_new(vm, s->data+i, 1));
    return 0;
}
static int s_charCodeAt(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize i = (js_isize)js_to_number(vm, arg(a,n,0));
    if (i < 0 || (js_usize)i >= s->len) { *out = js_mk_num(js_nan()); return 0; }
    *out = js_mk_num((double)(js_u8)s->data[i]);
    return 0;
}
static int s_indexOf(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t), *q = js_to_string(vm, arg(a,n,0));
    js_isize from = n>1 ? (js_isize)js_to_number(vm,a[1]) : 0;
    if (from < 0) from = 0;
    if (q->len == 0) { *out = js_mk_num((double)(from <= (js_isize)s->len ? from : (js_isize)s->len)); return 0; }
    for (js_usize i = (js_usize)from; i + q->len <= s->len; i++) {
        if (js_memcmp(s->data+i, q->data, q->len) == 0) { *out = js_mk_num((double)i); return 0; }
    }
    *out = js_mk_num(-1);
    return 0;
}
static int s_lastIndexOf(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t), *q = js_to_string(vm, arg(a,n,0));
    if (q->len == 0) { *out = js_mk_num((double)s->len); return 0; }
    if (q->len > s->len) { *out = js_mk_num(-1); return 0; }
    for (js_isize i = (js_isize)(s->len - q->len); i >= 0; i--) {
        if (js_memcmp(s->data+i, q->data, q->len) == 0) { *out = js_mk_num((double)i); return 0; }
    }
    *out = js_mk_num(-1);
    return 0;
}
static int s_includes(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t), *q = js_to_string(vm, arg(a,n,0));
    if (q->len == 0) { *out = js_mk_bool(1); return 0; }
    for (js_usize i = 0; i + q->len <= s->len; i++)
        if (js_memcmp(s->data+i,q->data,q->len)==0) { *out=js_mk_bool(1); return 0; }
    *out = js_mk_bool(0); return 0;
}
static int s_startsWith(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t), *q = js_to_string(vm, arg(a,n,0));
    *out = js_mk_bool(q->len <= s->len && js_memcmp(s->data,q->data,q->len)==0);
    return 0;
}
static int s_endsWith(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t), *q = js_to_string(vm, arg(a,n,0));
    *out = js_mk_bool(q->len <= s->len &&
        js_memcmp(s->data + s->len - q->len, q->data, q->len)==0);
    return 0;
}
static int s_slice(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize len = (js_isize)s->len;
    js_isize b = n>0 ? (js_isize)js_to_number(vm,a[0]) : 0;
    js_isize e = (n>1 && a[1].type!=JS_UNDEFINED) ? (js_isize)js_to_number(vm,a[1]) : len;
    if (b < 0) { b += len; if (b<0) b=0; } if (b>len) b=len;
    if (e < 0) { e += len; if (e<0) e=0; } if (e>len) e=len;
    if (e < b) e = b;
    *out = js_mk_str(js_str_new(vm, s->data+b, (js_usize)(e-b)));
    return 0;
}
static int s_substring(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize len = (js_isize)s->len;
    js_isize b = n>0 ? (js_isize)js_to_number(vm,a[0]) : 0;
    js_isize e = (n>1 && a[1].type!=JS_UNDEFINED) ? (js_isize)js_to_number(vm,a[1]) : len;
    if (b<0)b=0;
    if (b>len)b=len;
    if (e<0)e=0;
    if (e>len)e=len;
    if (b>e) { js_isize tmp=b; b=e; e=tmp; }
    *out = js_mk_str(js_str_new(vm, s->data+b, (js_usize)(e-b)));
    return 0;
}
static int s_substr(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize len = (js_isize)s->len;
    js_isize b = n>0 ? (js_isize)js_to_number(vm,a[0]) : 0;
    if (b<0){b+=len; if(b<0)b=0;}
    js_isize cnt = (n>1 && a[1].type!=JS_UNDEFINED) ? (js_isize)js_to_number(vm,a[1]) : len-b;
    if (cnt<0)cnt=0;
    if (b+cnt>len)cnt=len-b;
    *out = js_mk_str(js_str_new(vm, s->data+b, (js_usize)cnt));
    return 0;
}
static int s_toUpper(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;
    js_string *s = this_str(vm, t);
    js_string *r = js_str_new(vm, s->data, s->len);
    for (js_usize i=0;i<r->len;i++) if (r->data[i]>='a'&&r->data[i]<='z') r->data[i]-=32;
    r->hash = js_str_hash(r->data, r->len);
    *out = js_mk_str(r); return 0;
}
static int s_toLower(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;
    js_string *s = this_str(vm, t);
    js_string *r = js_str_new(vm, s->data, s->len);
    for (js_usize i=0;i<r->len;i++) if (r->data[i]>='A'&&r->data[i]<='Z') r->data[i]+=32;
    r->hash = js_str_hash(r->data, r->len);
    *out = js_mk_str(r); return 0;
}
static int s_trim(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;
    js_string *s = this_str(vm, t);
    js_usize i=0,j=s->len;
    while (i<j && (s->data[i]==' '||s->data[i]=='\t'||s->data[i]=='\n'||s->data[i]=='\r')) i++;
    while (j>i && (s->data[j-1]==' '||s->data[j-1]=='\t'||s->data[j-1]=='\n'||s->data[j-1]=='\r')) j--;
    *out = js_mk_str(js_str_new(vm, s->data+i, j-i)); return 0;
}
static int s_concat(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *r = this_str(vm, t);
    for (int i=0;i<n;i++) r = js_str_concat(vm, r, js_to_string(vm,a[i]));
    *out = js_mk_str(r); return 0;
}
static int s_repeat(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_isize cnt = (js_isize)js_to_number(vm, arg(a,n,0));
    if (cnt < 0) { js_throw_str(vm,"Invalid count value"); return -1; }
    js_string *r = js_str_newz(vm, "");
    for (js_isize i=0;i<cnt;i++) r = js_str_concat(vm, r, s);
    *out = js_mk_str(r); return 0;
}
static int s_split(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_object *res = js_array_new(vm);
    /* optional 2nd arg: limit on number of pieces */
    js_isize limit = -1;
    if (n > 1 && a[1].type != JS_UNDEFINED) {
        double ld = js_to_number(vm, a[1]);
        limit = js_isnan(ld) ? 0 : (js_isize)ld;
        if (limit < 0) limit = 0;
    }
    if (limit == 0) { *out = js_mk_obj(res); return 0; }
    if (n == 0 || a[0].type == JS_UNDEFINED) {
        js_arr_push(vm, res, js_mk_str(s));
        *out = js_mk_obj(res); return 0;
    }
    js_string *sep = js_to_string(vm, a[0]);
    if (sep->len == 0) {
        /* split into characters */
        for (js_usize i=0;i<s->len;i++) {
            if (limit >= 0 && (js_isize)res->length >= limit) break;
            js_arr_push(vm, res, js_mk_str(js_str_new(vm, s->data+i, 1)));
        }
        *out = js_mk_obj(res); return 0;
    }
    js_usize start = 0;
    for (js_usize i = 0; i + sep->len <= s->len; ) {
        if (js_memcmp(s->data+i, sep->data, sep->len) == 0) {
            if (limit >= 0 && (js_isize)res->length >= limit) { *out = js_mk_obj(res); return 0; }
            js_arr_push(vm, res, js_mk_str(js_str_new(vm, s->data+start, i-start)));
            i += sep->len;
            start = i;
        } else i++;
    }
    if (limit < 0 || (js_isize)res->length < limit)
        js_arr_push(vm, res, js_mk_str(js_str_new(vm, s->data+start, s->len-start)));
    *out = js_mk_obj(res); return 0;
}
static int s_replace(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    /* literal first-occurrence replacement only */
    js_string *s = this_str(vm, t);
    js_string *pat = js_to_string(vm, arg(a,n,0));
    js_string *rep = js_to_string(vm, arg(a,n,1));
    if (pat->len == 0 || pat->len > s->len) { *out = js_mk_str(s); return 0; }
    for (js_usize i = 0; i + pat->len <= s->len; i++) {
        if (js_memcmp(s->data+i, pat->data, pat->len) == 0) {
            js_string *r = js_str_new(vm, s->data, i);
            r = js_str_concat(vm, r, rep);
            js_string *tail = js_str_new(vm, s->data+i+pat->len, s->len-i-pat->len);
            r = js_str_concat(vm, r, tail);
            *out = js_mk_str(r); return 0;
        }
    }
    *out = js_mk_str(s); return 0;
}
static int s_padStart(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_usize target = (js_usize)js_to_number(vm, arg(a,n,0));
    js_string *pad = (n>1 && a[1].type!=JS_UNDEFINED) ? js_to_string(vm,a[1]) : js_str_newz(vm," ");
    if (s->len >= target || pad->len == 0) { *out = js_mk_str(s); return 0; }
    js_string *r = js_str_newz(vm,"");
    while (r->len + s->len < target) r = js_str_concat(vm, r, pad);
    /* trim padding to exact */
    if (r->len + s->len > target) r = js_str_new(vm, r->data, target - s->len);
    r = js_str_concat(vm, r, s);
    *out = js_mk_str(r); return 0;
}
static int s_padEnd(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_usize target = (js_usize)js_to_number(vm, arg(a,n,0));
    js_string *pad = (n>1 && a[1].type!=JS_UNDEFINED) ? js_to_string(vm,a[1]) : js_str_newz(vm," ");
    if (s->len >= target || pad->len == 0) { *out = js_mk_str(s); return 0; }
    js_string *r = js_str_new(vm, s->data, s->len);
    while (r->len < target) r = js_str_concat(vm, r, pad);
    if (r->len > target) r = js_str_new(vm, r->data, target);
    *out = js_mk_str(r); return 0;
}
static int s_trimStart(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;
    js_string *s = this_str(vm, t);
    js_usize i=0;
    while (i<s->len && (s->data[i]==' '||s->data[i]=='\t'||s->data[i]=='\n'||s->data[i]=='\r')) i++;
    *out = js_mk_str(js_str_new(vm, s->data+i, s->len-i)); return 0;
}
static int s_trimEnd(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;
    js_string *s = this_str(vm, t);
    js_usize j=s->len;
    while (j>0 && (s->data[j-1]==' '||s->data[j-1]=='\t'||s->data[j-1]=='\n'||s->data[j-1]=='\r')) j--;
    *out = js_mk_str(js_str_new(vm, s->data, j)); return 0;
}
/* replaceAll: literal pattern, every (non-overlapping) occurrence */
static int s_replaceAll(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *s = this_str(vm, t);
    js_string *pat = js_to_string(vm, arg(a,n,0));
    js_string *rep = js_to_string(vm, arg(a,n,1));
    if (pat->len == 0) { *out = js_mk_str(s); return 0; }   /* avoid infinite loop */
    js_string *r = js_str_newz(vm, "");
    js_usize i = 0, start = 0;
    while (i + pat->len <= s->len) {
        if (js_memcmp(s->data+i, pat->data, pat->len) == 0) {
            r = js_str_concat(vm, r, js_str_new(vm, s->data+start, i-start));
            r = js_str_concat(vm, r, rep);
            i += pat->len;
            start = i;
        } else i++;
    }
    r = js_str_concat(vm, r, js_str_new(vm, s->data+start, s->len-start));
    *out = js_mk_str(r); return 0;
}
/* String.fromCharCode(...codes) -- static (this ignored) */
static int s_fromCharCode(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    char *buf = (char *)js_arena_alloc(vm, (js_usize)(n>0?n:1) + 1);
    js_usize len = 0;
    for (int i=0;i<n;i++) buf[len++] = (char)((js_u32)js_to_number(vm, a[i]) & 0xFF);
    *out = js_mk_str(js_str_new(vm, buf, len)); return 0;
}

/* ================================================================== */
/*  Array.prototype methods (this is an array)                        */
/* ================================================================== */
static js_object *this_arr(js_value t) { return (t.type==JS_ARRAY) ? t.u.o : NULL; }

static int a_push(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    if (!arr) { js_throw_str(vm,"push of non-array"); return -1; }
    for (int i=0;i<n;i++) js_arr_push(vm, arr, a[i]);
    *out = js_mk_num((double)arr->length); return 0;
}
static int a_pop(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;(void)vm;
    js_object *arr = this_arr(t);
    if (!arr || arr->length==0) { *out = js_mk_undef(); return 0; }
    *out = arr->elems[--arr->length]; return 0;
}
static int a_shift(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;(void)vm;
    js_object *arr = this_arr(t);
    if (!arr || arr->length==0) { *out = js_mk_undef(); return 0; }
    *out = arr->elems[0];
    for (js_usize i=1;i<arr->length;i++) arr->elems[i-1]=arr->elems[i];
    arr->length--;
    return 0;
}
static int a_unshift(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    if (!arr) { js_throw_str(vm,"unshift of non-array"); return -1; }
    for (int k=0;k<n;k++) js_arr_push(vm, arr, js_mk_undef());  /* grow */
    for (js_isize i=(js_isize)arr->length-1;i>=n;i--) arr->elems[i]=arr->elems[i-n];
    for (int i=0;i<n;i++) arr->elems[i]=a[i];
    *out = js_mk_num((double)arr->length); return 0;
}
static int a_join(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    if (!arr) { *out = js_mk_strz(vm,""); return 0; }
    js_string *sep = (n>0 && a[0].type!=JS_UNDEFINED) ? js_to_string(vm,a[0]) : js_str_newz(vm,",");
    js_string *r = js_str_newz(vm,"");
    for (js_usize i=0;i<arr->length;i++) {
        if (i) r = js_str_concat(vm, r, sep);
        js_value e = arr->elems[i];
        if (e.type==JS_UNDEFINED||e.type==JS_NULL) continue;
        r = js_str_concat(vm, r, js_to_string(vm, e));
    }
    *out = js_mk_str(r); return 0;
}
static int a_indexOf(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value q = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++)
        if (js_strict_eq(arr->elems[i], q)) { *out=js_mk_num((double)i); return 0; }
    *out = js_mk_num(-1); (void)vm; return 0;
}
static int a_includes(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value q = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++)
        if (js_strict_eq(arr->elems[i], q)) { *out=js_mk_bool(1); return 0; }
    *out = js_mk_bool(0); (void)vm; return 0;
}
static int a_slice(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_object *res = js_array_new(vm);
    if (arr) {
        js_isize len = (js_isize)arr->length;
        js_isize b = n>0 ? (js_isize)js_to_number(vm,a[0]) : 0;
        js_isize e = (n>1 && a[1].type!=JS_UNDEFINED) ? (js_isize)js_to_number(vm,a[1]) : len;
        if (b<0){b+=len;if(b<0)b=0;} if(b>len)b=len;
        if (e<0){e+=len;if(e<0)e=0;} if(e>len)e=len;
        for (js_isize i=b;i<e;i++) js_arr_push(vm, res, arr->elems[i]);
    }
    *out = js_mk_obj(res); return 0;
}
static int a_concat(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_object *res = js_array_new(vm);
    if (arr) for (js_usize i=0;i<arr->length;i++) js_arr_push(vm,res,arr->elems[i]);
    for (int i=0;i<n;i++) {
        if (a[i].type==JS_ARRAY) {
            js_object *o = a[i].u.o;
            for (js_usize k=0;k<o->length;k++) js_arr_push(vm,res,o->elems[k]);
        } else js_arr_push(vm, res, a[i]);
    }
    *out = js_mk_obj(res); return 0;
}
static int a_reverse(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)a;(void)n;(void)vm;
    js_object *arr = this_arr(t);
    if (arr) for (js_usize i=0,j=arr->length?arr->length-1:0; i<j; i++,j--) {
        js_value tmp=arr->elems[i]; arr->elems[i]=arr->elems[j]; arr->elems[j]=tmp;
    }
    *out = t; return 0;
}
static int a_fill(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value v = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) arr->elems[i]=v;
    *out = t; (void)vm; return 0;
}

/* higher-order: map/filter/forEach/reduce/some/every/find */
static int call_cb(js_vm *vm, js_value cb, js_value thisv, js_value el,
                   double idx, js_value arrv, js_value *res)
{
    js_value args[3] = { el, js_mk_num(idx), arrv };
    return js_call_function(vm, cb, thisv, args, 3, res) == CMP_THROW ? -1 : 0;
}

static int a_map(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    js_object *res = js_array_new(vm);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        js_arr_push(vm, res, r);
    }
    *out = js_mk_obj(res); return 0;
}
static int a_filter(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    js_object *res = js_array_new(vm);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        if (js_truthy(r)) js_arr_push(vm, res, arr->elems[i]);
    }
    *out = js_mk_obj(res); return 0;
}
static int a_forEach(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
    }
    *out = js_mk_undef(); return 0;
}
static int a_reduce(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    js_value acc;
    js_usize i = 0;
    if (n > 1) acc = a[1];
    else {
        if (!arr || arr->length==0) { js_throw_str(vm,"Reduce of empty array with no initial value"); return -1; }
        acc = arr->elems[0]; i = 1;
    }
    if (arr) for (; i<arr->length; i++) {
        js_value args[4] = { acc, arr->elems[i], js_mk_num((double)i), t };
        js_value r;
        if (js_call_function(vm, cb, js_mk_undef(), args, 4, &r) == CMP_THROW) return -1;
        acc = r;
    }
    *out = acc; return 0;
}
static int a_some(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        if (js_truthy(r)) { *out = js_mk_bool(1); return 0; }
    }
    *out = js_mk_bool(0); return 0;
}
static int a_every(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        if (!js_truthy(r)) { *out = js_mk_bool(0); return 0; }
    }
    *out = js_mk_bool(1); return 0;
}
static int a_find(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        if (js_truthy(r)) { *out = arr->elems[i]; return 0; }
    }
    *out = js_mk_undef(); return 0;
}
/* sort: default ascending by string compare, or via comparator */
static int a_sort(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    if (!arr) { *out = t; return 0; }
    js_value cmp = arg(a,n,0);
    int has_cmp = (cmp.type == JS_FUNCTION);
    /* simple insertion sort (stable, bounded sizes) */
    for (js_usize i = 1; i < arr->length; i++) {
        js_value key = arr->elems[i];
        js_isize j = (js_isize)i - 1;
        for (; j >= 0; j--) {
            int gt;
            if (has_cmp) {
                js_value args[2] = { arr->elems[j], key };
                js_value r;
                if (js_call_function(vm, cmp, js_mk_undef(), args, 2, &r) == CMP_THROW) return -1;
                gt = js_to_number(vm, r) > 0;
            } else {
                js_string *sa = js_to_string(vm, arr->elems[j]);
                js_string *sb = js_to_string(vm, key);
                js_usize m = sa->len<sb->len?sa->len:sb->len;
                int c = js_memcmp(sa->data, sb->data, m);
                if (c==0) c = (sa->len<sb->len)?-1:(sa->len>sb->len?1:0);
                gt = c > 0;
            }
            if (!gt) break;
            arr->elems[j+1] = arr->elems[j];
        }
        arr->elems[j+1] = key;
    }
    *out = t; return 0;
}

static int a_lastIndexOf(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)vm;
    js_object *arr = this_arr(t);
    js_value q = arg(a,n,0);
    if (arr) for (js_isize i=(js_isize)arr->length-1;i>=0;i--)
        if (js_strict_eq(arr->elems[i], q)) { *out=js_mk_num((double)i); return 0; }
    *out = js_mk_num(-1); return 0;
}
static int a_findIndex(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_value cb = arg(a,n,0);
    if (arr) for (js_usize i=0;i<arr->length;i++) {
        js_value r;
        if (call_cb(vm, cb, js_mk_undef(), arr->elems[i], (double)i, t, &r) < 0) return -1;
        if (js_truthy(r)) { *out = js_mk_num((double)i); return 0; }
    }
    *out = js_mk_num(-1); return 0;
}
/* splice(start, deleteCount, ...items) -- mutates, returns removed elements */
static int a_splice(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    js_object *removed = js_array_new(vm);
    if (!arr) { *out = js_mk_obj(removed); return 0; }
    js_isize len = (js_isize)arr->length;
    js_isize start = n>0 ? (js_isize)js_to_number(vm,a[0]) : 0;
    if (start < 0) { start += len; if (start < 0) start = 0; }
    if (start > len) start = len;
    js_isize delc;
    if (n < 2) delc = len - start;                /* delete to end */
    else { delc = (js_isize)js_to_number(vm,a[1]); if (delc < 0) delc = 0;
           if (delc > len - start) delc = len - start; }
    int nins = n > 2 ? n - 2 : 0;
    /* collect removed */
    for (js_isize i=0;i<delc;i++) js_arr_push(vm, removed, arr->elems[start+i]);
    /* build the new element layout into a fresh buffer, then copy back */
    js_isize newlen = len - delc + nins;
    js_object *tmp = js_array_new(vm);
    for (js_isize i=0;i<start;i++) js_arr_push(vm, tmp, arr->elems[i]);
    for (int i=0;i<nins;i++) js_arr_push(vm, tmp, a[2+i]);
    for (js_isize i=start+delc;i<len;i++) js_arr_push(vm, tmp, arr->elems[i]);
    /* write back into arr */
    for (js_isize i=0;i<newlen;i++) js_arr_set(vm, arr, (js_usize)i, tmp->elems[i]);
    arr->length = (js_usize)newlen;
    *out = js_mk_obj(removed); return 0;
}
/* flat(depth=1) -- flatten nested arrays */
static void flat_into(js_vm *vm, js_object *dst, js_object *src, int depth)
{
    for (js_usize i=0;i<src->length;i++) {
        js_value e = src->elems[i];
        if (e.type==JS_ARRAY && depth>0) flat_into(vm, dst, e.u.o, depth-1);
        else js_arr_push(vm, dst, e);
    }
}
static int a_flat(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_object *arr = this_arr(t);
    int depth = (n>0 && a[0].type!=JS_UNDEFINED) ? (int)js_to_number(vm,a[0]) : 1;
    if (depth < 0) depth = 0;
    if (depth > 64) depth = 64;     /* bound recursion */
    js_object *res = js_array_new(vm);
    if (arr) flat_into(vm, res, arr, depth);
    *out = js_mk_obj(res); return 0;
}
static int a_isArray(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)vm;(void)t; *out = js_mk_bool(arg(a,n,0).type == JS_ARRAY); return 0; }

/* ================================================================== */
/*  Object.* static methods                                          */
/* ================================================================== */
static int o_keys(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *res = js_array_new(vm);
    js_value o = arg(a,n,0);
    if (o.type == JS_OBJECT || o.type == JS_FUNCTION) {
        js_object *obj = o.u.o;
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(obj, ord, 256);
        for (js_usize i=0;i<cnt;i++)
            if (ord[i]->enumerable) js_arr_push(vm, res, js_mk_str(ord[i]->key));
    } else if (o.type == JS_ARRAY) {
        js_object *obj = o.u.o;
        for (js_usize i=0;i<obj->length;i++)
            js_arr_push(vm, res, js_mk_str(js_num_to_str(vm,(double)i)));
    }
    *out = js_mk_obj(res); return 0;
}
static int o_values(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *res = js_array_new(vm);
    js_value o = arg(a,n,0);
    if (o.type == JS_OBJECT || o.type == JS_FUNCTION) {
        js_object *obj = o.u.o;
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(obj, ord, 256);
        for (js_usize i=0;i<cnt;i++)
            if (ord[i]->enumerable) js_arr_push(vm, res, ord[i]->val);
    } else if (o.type == JS_ARRAY) {
        js_object *obj = o.u.o;
        for (js_usize i=0;i<obj->length;i++) js_arr_push(vm, res, obj->elems[i]);
    }
    *out = js_mk_obj(res); return 0;
}
static int o_entries(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *res = js_array_new(vm);
    js_value o = arg(a,n,0);
    if (o.type == JS_OBJECT || o.type == JS_FUNCTION) {
        js_object *obj = o.u.o;
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(obj, ord, 256);
        for (js_usize i=0;i<cnt;i++) {
            js_prop *p = ord[i];
            if (p->enumerable) {
                js_object *pair = js_array_new(vm);
                js_arr_push(vm, pair, js_mk_str(p->key));
                js_arr_push(vm, pair, p->val);
                js_arr_push(vm, res, js_mk_obj(pair));
            }
        }
    }
    *out = js_mk_obj(res); return 0;
}
static int o_assign(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_value target = arg(a,n,0);
    if (target.type != JS_OBJECT) { *out = target; return 0; }
    for (int i=1;i<n;i++) {
        if (a[i].type != JS_OBJECT) continue;
        js_object *src = a[i].u.o;
        for (js_usize k=0;k<src->cap;k++) {
            js_prop *p = &src->props[k];
            if (p->key && p->enumerable) js_obj_set(vm, target.u.o, p->key, p->val);
        }
    }
    *out = target; return 0;
}
static int o_freeze(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)t;(void)vm; *out = arg(a,n,0); return 0; }  /* no-op (no enforcement) */
/* Object.create(proto [, props]) -- new object with the given prototype */
static int o_create(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *o = js_object_new(vm);
    if (!o) { js_throw_str(vm, "out of memory"); return -1; }
    js_value proto = arg(a,n,0);
    if (proto.type == JS_OBJECT || proto.type == JS_ARRAY || proto.type == JS_FUNCTION)
        o->proto = proto.u.o;
    else if (proto.type == JS_NULL)
        o->proto = NULL;
    /* second arg: property descriptors { key: { value: v } } */
    if (n > 1 && a[1].type == JS_OBJECT) {
        js_object *descs = a[1].u.o;
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(descs, ord, 256);
        for (js_usize i=0;i<cnt;i++) {
            if (!ord[i]->enumerable) continue;
            js_value d = ord[i]->val, val = js_mk_undef();
            if (d.type==JS_OBJECT) js_obj_get(vm, d.u.o, js_str_intern(vm,"value",5), &val);
            js_obj_set(vm, o, ord[i]->key, val);
        }
    }
    *out = js_mk_obj(o); return 0;
}
static int o_getPrototypeOf(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_value v = arg(a,n,0);
    if ((v.type==JS_OBJECT||v.type==JS_ARRAY||v.type==JS_FUNCTION) && v.u.o->proto)
        *out = js_mk_obj(v.u.o->proto);
    else
        *out = js_mk_null();
    (void)vm; return 0;
}
/* Object.defineProperty(obj, key, descriptor) -- supports value + enumerable */
static int o_defineProperty(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_value ov = arg(a,n,0);
    if (ov.type != JS_OBJECT && ov.type != JS_ARRAY && ov.type != JS_FUNCTION) {
        js_throw_str(vm, "Object.defineProperty called on non-object"); return -1;
    }
    js_string *key = js_to_string(vm, arg(a,n,1));
    js_value desc = arg(a,n,2);
    js_value val = js_mk_undef();
    int enumerable = 0;
    if (desc.type == JS_OBJECT) {
        js_obj_get(vm, desc.u.o, js_str_intern(vm,"value",5), &val);
        js_value en;
        if (js_obj_get(vm, desc.u.o, js_str_intern(vm,"enumerable",10), &en))
            enumerable = js_truthy(en);
    }
    js_obj_set(vm, ov.u.o, key, val);
    /* honor enumerable:false so the prop hides from keys()/for-in/JSON */
    if (!enumerable) {
        js_prop *p = (js_prop *)0;
        /* locate the slot we just wrote and clear its enumerable flag */
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(ov.u.o, ord, 256);
        for (js_usize i=0;i<cnt;i++) if (js_str_eq(ord[i]->key, key)) { p = ord[i]; break; }
        if (p) p->enumerable = 0;
    }
    *out = ov; return 0;
}
static int o_getOwnPropertyNames(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_object *res = js_array_new(vm);
    js_value o = arg(a,n,0);
    if (o.type == JS_OBJECT || o.type == JS_FUNCTION) {
        js_object *obj = o.u.o;
        js_prop *ord[256]; js_usize cnt = js_obj_ordered(obj, ord, 256);
        for (js_usize i=0;i<cnt;i++)   /* includes non-enumerable */
            js_arr_push(vm, res, js_mk_str(ord[i]->key));
    } else if (o.type == JS_ARRAY) {
        js_object *obj = o.u.o;
        for (js_usize i=0;i<obj->length;i++)
            js_arr_push(vm, res, js_mk_str(js_num_to_str(vm,(double)i)));
        js_arr_push(vm, res, js_mk_strz(vm, "length"));
    }
    *out = js_mk_obj(res); return 0;
}

/* ================================================================== */
/*  JSON                                                              */
/* ================================================================== */
static void json_str_escape(js_vm *vm, js_string **acc, js_string *s)
{
    *acc = js_str_concat(vm, *acc, js_str_newz(vm, "\""));
    for (js_usize i=0;i<s->len;i++) {
        char c = s->data[i];
        char esc[8]; js_usize el = 0;
        switch (c) {
        case '"':  esc[0]='\\'; esc[1]='"'; el=2; break;
        case '\\': esc[0]='\\'; esc[1]='\\'; el=2; break;
        case '\n': esc[0]='\\'; esc[1]='n'; el=2; break;
        case '\t': esc[0]='\\'; esc[1]='t'; el=2; break;
        case '\r': esc[0]='\\'; esc[1]='r'; el=2; break;
        case '\b': esc[0]='\\'; esc[1]='b'; el=2; break;
        case '\f': esc[0]='\\'; esc[1]='f'; el=2; break;
        default: esc[0]=c; el=1; break;
        }
        *acc = js_str_concat(vm, *acc, js_str_new(vm, esc, el));
    }
    *acc = js_str_concat(vm, *acc, js_str_newz(vm, "\""));
}

static js_string *json_stringify_rec(js_vm *vm, js_value v, int depth)
{
    if (depth > 32) return js_str_newz(vm, "null");
    switch (v.type) {
    case JS_NULL:      return js_str_newz(vm, "null");
    case JS_UNDEFINED: return NULL;   /* omitted */
    case JS_BOOL:      return js_str_newz(vm, v.u.b?"true":"false");
    case JS_NUMBER:    return js_isfinite(v.u.n) ? js_num_to_str(vm,v.u.n)
                                                 : js_str_newz(vm,"null");
    case JS_STRING: {
        js_string *acc = js_str_newz(vm,"");
        json_str_escape(vm, &acc, v.u.s);
        return acc;
    }
    case JS_FUNCTION:  return NULL;
    case JS_ARRAY: {
        js_object *a = v.u.o;
        js_string *acc = js_str_newz(vm, "[");
        for (js_usize i=0;i<a->length;i++) {
            if (i) acc = js_str_concat(vm, acc, js_str_newz(vm,","));
            js_string *e = json_stringify_rec(vm, a->elems[i], depth+1);
            if (!e) e = js_str_newz(vm,"null");
            acc = js_str_concat(vm, acc, e);
        }
        acc = js_str_concat(vm, acc, js_str_newz(vm,"]"));
        return acc;
    }
    case JS_OBJECT: {
        js_object *o = v.u.o;
        js_string *acc = js_str_newz(vm, "{");
        js_prop *ord[256];
        js_usize cnt = js_obj_ordered(o, ord, 256);
        int first = 1;
        for (js_usize i=0;i<cnt;i++) {
            js_prop *p = ord[i];
            if (!p->enumerable) continue;
            js_string *val = json_stringify_rec(vm, p->val, depth+1);
            if (!val) continue;  /* skip undefined/function */
            if (!first) acc = js_str_concat(vm, acc, js_str_newz(vm,","));
            first = 0;
            json_str_escape(vm, &acc, p->key);
            acc = js_str_concat(vm, acc, js_str_newz(vm,":"));
            acc = js_str_concat(vm, acc, val);
        }
        acc = js_str_concat(vm, acc, js_str_newz(vm,"}"));
        return acc;
    }
    default: return js_str_newz(vm,"null");
    }
}
/* indented (pretty) variant; `gap` is the per-level indent unit, `cur` the
 * accumulated indent for the current depth. Mirrors json_stringify_rec. */
static js_string *json_pretty_rec(js_vm *vm, js_value v, int depth,
                                  js_string *gap, js_string *cur)
{
    if (depth > 32) return js_str_newz(vm, "null");
    switch (v.type) {
    case JS_NULL:      return js_str_newz(vm, "null");
    case JS_UNDEFINED: return NULL;
    case JS_BOOL:      return js_str_newz(vm, v.u.b?"true":"false");
    case JS_NUMBER:    return js_isfinite(v.u.n) ? js_num_to_str(vm,v.u.n)
                                                 : js_str_newz(vm,"null");
    case JS_STRING: {
        js_string *acc = js_str_newz(vm,"");
        json_str_escape(vm, &acc, v.u.s);
        return acc;
    }
    case JS_FUNCTION:  return NULL;
    case JS_ARRAY: {
        js_object *a = v.u.o;
        if (a->length == 0) return js_str_newz(vm, "[]");
        js_string *inner = js_str_concat(vm, cur, gap);
        js_string *acc = js_str_newz(vm, "[\n");
        for (js_usize i=0;i<a->length;i++) {
            if (i) acc = js_str_concat(vm, acc, js_str_newz(vm,",\n"));
            acc = js_str_concat(vm, acc, inner);
            js_string *e = json_pretty_rec(vm, a->elems[i], depth+1, gap, inner);
            if (!e) e = js_str_newz(vm,"null");
            acc = js_str_concat(vm, acc, e);
        }
        acc = js_str_concat(vm, acc, js_str_newz(vm,"\n"));
        acc = js_str_concat(vm, acc, cur);
        acc = js_str_concat(vm, acc, js_str_newz(vm,"]"));
        return acc;
    }
    case JS_OBJECT: {
        js_object *o = v.u.o;
        js_string *inner = js_str_concat(vm, cur, gap);
        js_prop *ord[256];
        js_usize cnt = js_obj_ordered(o, ord, 256);
        js_string *acc = js_str_newz(vm, "{");
        int first = 1;
        for (js_usize i=0;i<cnt;i++) {
            js_prop *p = ord[i];
            if (!p->enumerable) continue;
            js_string *val = json_pretty_rec(vm, p->val, depth+1, gap, inner);
            if (!val) continue;
            acc = js_str_concat(vm, acc, js_str_newz(vm, first?"\n":",\n"));
            first = 0;
            acc = js_str_concat(vm, acc, inner);
            json_str_escape(vm, &acc, p->key);
            acc = js_str_concat(vm, acc, js_str_newz(vm,": "));
            acc = js_str_concat(vm, acc, val);
        }
        if (first) return js_str_newz(vm, "{}");
        acc = js_str_concat(vm, acc, js_str_newz(vm,"\n"));
        acc = js_str_concat(vm, acc, cur);
        acc = js_str_concat(vm, acc, js_str_newz(vm,"}"));
        return acc;
    }
    default: return js_str_newz(vm,"null");
    }
}
static int json_stringify(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    /* 3rd arg: indentation (a number of spaces, or a string). When present and
     * non-empty, produce pretty-printed output. */
    js_string *gap = NULL;
    if (n > 2) {
        js_value sp = a[2];
        if (sp.type == JS_NUMBER) {
            int cnt = (int)sp.u.n; if (cnt < 0) cnt = 0; if (cnt > 10) cnt = 10;
            char sb[16]; for (int i=0;i<cnt;i++) sb[i]=' ';
            gap = js_str_new(vm, sb, (js_usize)cnt);
        } else if (sp.type == JS_STRING) {
            gap = sp.u.s->len > 10 ? js_str_new(vm, sp.u.s->data, 10) : sp.u.s;
        }
    }
    js_string *s;
    if (gap && gap->len)
        s = json_pretty_rec(vm, arg(a,n,0), 0, gap, js_str_newz(vm,""));
    else
        s = json_stringify_rec(vm, arg(a,n,0), 0);
    *out = s ? js_mk_str(s) : js_mk_undef();
    return 0;
}

/* --- JSON.parse --- */
typedef struct { js_vm *vm; const char *p; const char *end; int err; } jparse;
static void jp_ws(jparse *j) { while (j->p<j->end && (*j->p==' '||*j->p=='\t'||*j->p=='\n'||*j->p=='\r')) j->p++; }
/* Cap recursion so deeply-nested JSON (e.g. '['.repeat(50000)) can't overflow
 * the user stack. The stringify side already guards (depth>32); the parser
 * did not. 128 is far beyond any real document. */
#define JSON_MAX_DEPTH 128
static js_value jp_value(jparse *j, int depth);

static js_value jp_string(jparse *j)
{
    j->p++; /* opening quote */
    js_usize cap = (js_usize)(j->end - j->p) + 1;
    char *buf = (char *)js_arena_alloc(j->vm, cap);
    js_usize n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
            case 'n': buf[n++]='\n'; break;
            case 't': buf[n++]='\t'; break;
            case 'r': buf[n++]='\r'; break;
            case 'b': buf[n++]='\b'; break;
            case 'f': buf[n++]='\f'; break;
            case '/': buf[n++]='/'; break;
            case '"': buf[n++]='"'; break;
            case '\\': buf[n++]='\\'; break;
            case 'u': {
                js_u32 cp = 0;
                for (int k=0;k<4 && j->p<j->end;k++) {
                    char h = *j->p++;
                    int d = (h>='0'&&h<='9')?h-'0':(h>='a'&&h<='f')?h-'a'+10:(h>='A'&&h<='F')?h-'A'+10:0;
                    cp = cp*16 + d;
                }
                if (cp < 0x80) buf[n++]=(char)cp;
                else if (cp < 0x800) { buf[n++]=(char)(0xC0|(cp>>6)); buf[n++]=(char)(0x80|(cp&0x3F)); }
                else { buf[n++]=(char)(0xE0|(cp>>12)); buf[n++]=(char)(0x80|((cp>>6)&0x3F)); buf[n++]=(char)(0x80|(cp&0x3F)); }
                break;
            }
            default: buf[n++]=e; break;
            }
        } else buf[n++]=c;
    }
    if (j->p < j->end) j->p++; /* closing quote */
    return js_mk_str(js_str_new(j->vm, buf, n));
}

static js_value jp_value(jparse *j, int depth)
{
    if (depth > JSON_MAX_DEPTH) { j->err = 1; return js_mk_undef(); }
    jp_ws(j);
    if (j->p >= j->end) { j->err = 1; return js_mk_undef(); }
    char c = *j->p;
    if (c == '"') return jp_string(j);
    if (c == '{') {
        j->p++;
        js_object *o = js_object_new(j->vm);
        jp_ws(j);
        if (j->p<j->end && *j->p=='}') { j->p++; return js_mk_obj(o); }
        for (;;) {
            jp_ws(j);
            if (j->p>=j->end || *j->p!='"') { j->err=1; break; }
            js_value key = jp_string(j);
            jp_ws(j);
            if (j->p>=j->end || *j->p!=':') { j->err=1; break; }
            j->p++;
            js_value val = jp_value(j, depth + 1);
            js_obj_set(j->vm, o, key.u.s, val);
            jp_ws(j);
            if (j->p<j->end && *j->p==',') { j->p++; continue; }
            if (j->p<j->end && *j->p=='}') { j->p++; break; }
            j->err=1; break;
        }
        return js_mk_obj(o);
    }
    if (c == '[') {
        j->p++;
        js_object *arr = js_array_new(j->vm);
        jp_ws(j);
        if (j->p<j->end && *j->p==']') { j->p++; return js_mk_obj(arr); }
        for (;;) {
            js_value v = jp_value(j, depth + 1);
            js_arr_push(j->vm, arr, v);
            jp_ws(j);
            if (j->p<j->end && *j->p==',') { j->p++; continue; }
            if (j->p<j->end && *j->p==']') { j->p++; break; }
            j->err=1; break;
        }
        return js_mk_obj(arr);
    }
    if (c=='t' && (j->end-j->p)>=4 && js_memcmp(j->p,"true",4)==0) { j->p+=4; return js_mk_bool(1); }
    if (c=='f' && (j->end-j->p)>=5 && js_memcmp(j->p,"false",5)==0) { j->p+=5; return js_mk_bool(0); }
    if (c=='n' && (j->end-j->p)>=4 && js_memcmp(j->p,"null",4)==0) { j->p+=4; return js_mk_null(); }
    /* number */
    const char *start = j->p;
    if (*j->p=='-'||*j->p=='+') j->p++;
    while (j->p<j->end && ((*j->p>='0'&&*j->p<='9')||*j->p=='.'||*j->p=='e'||*j->p=='E'||*j->p=='+'||*j->p=='-')) j->p++;
    int ok=0;
    double d = js_parse_double(start, (js_usize)(j->p-start), &ok);
    if (!ok) { j->err=1; return js_mk_undef(); }
    return js_mk_num(d);
}
static int json_parse(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *s = js_to_string(vm, arg(a,n,0));
    jparse j = { vm, s->data, s->data + s->len, 0 };
    js_value v = jp_value(&j, 0);
    if (j.err) { js_throw_str(vm, "Unexpected token in JSON"); return -1; }
    *out = v; return 0;
}

/* ================================================================== */
/*  Number.prototype.toFixed                                          */
/* ================================================================== */
static int num_toFixed(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    double x = js_to_number(vm, t);
    int digits = n>0 ? (int)js_to_number(vm,a[0]) : 0;
    if (digits < 0) digits = 0;
    if (digits > 18) digits = 18;
    /* round to `digits` decimals */
    double scale = 1.0;
    for (int i=0;i<digits;i++) scale *= 10.0;
    int neg = x < 0; if (neg) x = -x;
    double scaled = math_round(x * scale);
    /* build integer-part and fractional digits */
    char buf[64]; js_usize pos = 0;
    if (neg) buf[pos++]='-';
    double ip = js_math_floor(scaled / scale);
    js_u64 ipart = (js_u64)ip;
    char tmp[24]; js_usize tn=0;
    if (ipart==0) tmp[tn++]='0';
    while (ipart) { tmp[tn++]=(char)('0'+ipart%10); ipart/=10; }
    for (js_usize i=0;i<tn;i++) buf[pos++]=tmp[tn-1-i];
    if (digits > 0) {
        buf[pos++]='.';
        double frac = scaled - ip*scale;
        js_u64 fpart = (js_u64)frac;
        char fb[24]; js_usize fn=0;
        for (int i=0;i<digits;i++) { fb[fn++]=(char)('0'+fpart%10); fpart/=10; }
        for (js_usize i=0;i<fn;i++) buf[pos++]=fb[fn-1-i];
    }
    buf[pos]=0;
    *out = js_mk_str(js_str_new(vm, buf, pos));
    return 0;
}
static int num_toString(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)a;(void)n; *out = js_mk_str(js_num_to_str(vm, js_to_number(vm,t))); return 0; }

/* generic toString for any value */
static int any_toString(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{ (void)a;(void)n; *out = js_mk_str(js_to_string(vm, t)); return 0; }
static int obj_hasOwnProperty(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    js_string *k = js_to_string(vm, arg(a,n,0));
    int has = 0;
    if (t.type==JS_OBJECT||t.type==JS_FUNCTION) has = js_obj_has_own(t.u.o, k);
    else if (t.type==JS_ARRAY) {
        int ok=0; double d=js_parse_double(k->data,k->len,&ok);
        if (ok && d>=0 && d==(double)(js_usize)d && (js_usize)d < t.u.o->length) has=1;
    }
    *out = js_mk_bool(has); return 0;
}

/* ================================================================== */
/*  Install everything                                                */
/* ================================================================== */
void js_install_builtins(js_vm *vm)
{
    /* prototype objects first (proto chain target for methods) */
    vm->proto_object   = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    vm->proto_array    = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    vm->proto_string   = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    vm->proto_number   = (js_object *)js_arena_alloc(vm, sizeof(js_object));
    vm->proto_function = (js_object *)js_arena_alloc(vm, sizeof(js_object));

    /* String.prototype */
    js_object *sp = vm->proto_string;
    reg_method(vm, sp, "charAt", s_charAt);
    reg_method(vm, sp, "charCodeAt", s_charCodeAt);
    reg_method(vm, sp, "indexOf", s_indexOf);
    reg_method(vm, sp, "lastIndexOf", s_lastIndexOf);
    reg_method(vm, sp, "includes", s_includes);
    reg_method(vm, sp, "startsWith", s_startsWith);
    reg_method(vm, sp, "endsWith", s_endsWith);
    reg_method(vm, sp, "slice", s_slice);
    reg_method(vm, sp, "substring", s_substring);
    reg_method(vm, sp, "substr", s_substr);
    reg_method(vm, sp, "toUpperCase", s_toUpper);
    reg_method(vm, sp, "toLowerCase", s_toLower);
    reg_method(vm, sp, "trim", s_trim);
    reg_method(vm, sp, "concat", s_concat);
    reg_method(vm, sp, "repeat", s_repeat);
    reg_method(vm, sp, "split", s_split);
    reg_method(vm, sp, "replace", s_replace);
    reg_method(vm, sp, "replaceAll", s_replaceAll);
    reg_method(vm, sp, "padStart", s_padStart);
    reg_method(vm, sp, "padEnd", s_padEnd);
    reg_method(vm, sp, "trimStart", s_trimStart);
    reg_method(vm, sp, "trimEnd", s_trimEnd);
    reg_method(vm, sp, "toString", any_toString);

    /* Array.prototype */
    js_object *ap = vm->proto_array;
    reg_method(vm, ap, "push", a_push);
    reg_method(vm, ap, "pop", a_pop);
    reg_method(vm, ap, "shift", a_shift);
    reg_method(vm, ap, "unshift", a_unshift);
    reg_method(vm, ap, "join", a_join);
    reg_method(vm, ap, "indexOf", a_indexOf);
    reg_method(vm, ap, "includes", a_includes);
    reg_method(vm, ap, "slice", a_slice);
    reg_method(vm, ap, "concat", a_concat);
    reg_method(vm, ap, "reverse", a_reverse);
    reg_method(vm, ap, "fill", a_fill);
    reg_method(vm, ap, "map", a_map);
    reg_method(vm, ap, "filter", a_filter);
    reg_method(vm, ap, "forEach", a_forEach);
    reg_method(vm, ap, "reduce", a_reduce);
    reg_method(vm, ap, "some", a_some);
    reg_method(vm, ap, "every", a_every);
    reg_method(vm, ap, "find", a_find);
    reg_method(vm, ap, "findIndex", a_findIndex);
    reg_method(vm, ap, "lastIndexOf", a_lastIndexOf);
    reg_method(vm, ap, "splice", a_splice);
    reg_method(vm, ap, "flat", a_flat);
    reg_method(vm, ap, "sort", a_sort);
    reg_method(vm, ap, "join", a_join);
    reg_method(vm, ap, "toString", any_toString);

    /* Number.prototype */
    js_object *np = vm->proto_number;
    reg_method(vm, np, "toFixed", num_toFixed);
    reg_method(vm, np, "toString", num_toString);

    /* Object.prototype (methods available on plain objects) */
    js_object *op = vm->proto_object;
    reg_method(vm, op, "hasOwnProperty", obj_hasOwnProperty);
    reg_method(vm, op, "toString", any_toString);

    /* console */
    js_object *console = js_object_new(vm);
    reg_method(vm, console, "log", native_console_log);
    reg_method(vm, console, "error", native_console_log);
    reg_method(vm, console, "warn", native_console_log);
    reg_method(vm, console, "info", native_console_log);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"console",7), js_mk_obj(console), 0);

    /* Math */
    js_object *math = js_object_new(vm);
    js_obj_set(vm, math, js_str_intern(vm,"PI",2), js_mk_num(JS_PI));
    js_obj_set(vm, math, js_str_intern(vm,"E",1), js_mk_num(2.718281828459045));
    js_obj_set(vm, math, js_str_intern(vm,"LN2",3), js_mk_num(0.6931471805599453));
    js_obj_set(vm, math, js_str_intern(vm,"LN10",4), js_mk_num(2.302585092994046));
    js_obj_set(vm, math, js_str_intern(vm,"SQRT2",5), js_mk_num(1.4142135623730951));
    js_obj_set(vm, math, js_str_intern(vm,"SQRT1_2",7), js_mk_num(0.7071067811865476));
    js_obj_set(vm, math, js_str_intern(vm,"LOG2E",5), js_mk_num(1.4426950408889634));
    js_obj_set(vm, math, js_str_intern(vm,"LOG10E",6), js_mk_num(0.4342944819032518));
    reg_method(vm, math, "abs", m_abs);
    reg_method(vm, math, "floor", m_floor);
    reg_method(vm, math, "ceil", m_ceil);
    reg_method(vm, math, "round", m_round);
    reg_method(vm, math, "trunc", m_trunc);
    reg_method(vm, math, "sqrt", m_sqrt);
    reg_method(vm, math, "cbrt", m_cbrt);
    reg_method(vm, math, "pow", m_pow);
    reg_method(vm, math, "exp", m_exp);
    reg_method(vm, math, "log", m_log);
    reg_method(vm, math, "log2", m_log2);
    reg_method(vm, math, "log10", m_log10);
    reg_method(vm, math, "sin", m_sin);
    reg_method(vm, math, "cos", m_cos);
    reg_method(vm, math, "tan", m_tan);
    reg_method(vm, math, "atan", m_atan);
    reg_method(vm, math, "atan2", m_atan2);
    reg_method(vm, math, "sign", m_sign);
    reg_method(vm, math, "min", m_min);
    reg_method(vm, math, "max", m_max);
    reg_method(vm, math, "hypot", m_hypot);
    reg_method(vm, math, "random", m_random);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"Math",4), js_mk_obj(math), 0);

    /* JSON */
    js_object *json = js_object_new(vm);
    reg_method(vm, json, "stringify", json_stringify);
    reg_method(vm, json, "parse", json_parse);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"JSON",4), js_mk_obj(json), 0);

    /* Object (with static methods) */
    js_object *Object = js_func_new_native(vm, NULL, "Object");
    reg_method(vm, Object, "keys", o_keys);
    reg_method(vm, Object, "values", o_values);
    reg_method(vm, Object, "entries", o_entries);
    reg_method(vm, Object, "assign", o_assign);
    reg_method(vm, Object, "freeze", o_freeze);
    reg_method(vm, Object, "create", o_create);
    reg_method(vm, Object, "getPrototypeOf", o_getPrototypeOf);
    reg_method(vm, Object, "defineProperty", o_defineProperty);
    reg_method(vm, Object, "getOwnPropertyNames", o_getOwnPropertyNames);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"Object",6), js_mk_obj(Object), 0);

    /* global functions */
    reg_global(vm, "parseInt", g_parseInt);
    reg_global(vm, "parseFloat", g_parseFloat);
    reg_global(vm, "isNaN", g_isNaN);
    reg_global(vm, "isFinite", g_isFinite);
    reg_global(vm, "Boolean", g_Boolean);

    /* Number (conversion fn + static predicates/constants) */
    js_object *Number = js_func_new_native(vm, g_Number, "Number");
    reg_method(vm, Number, "isInteger", n_isInteger);
    reg_method(vm, Number, "isFinite", n_isFinite);
    reg_method(vm, Number, "isNaN", n_isNaN);
    reg_method(vm, Number, "isSafeInteger", n_isSafeInteger);
    reg_method(vm, Number, "parseFloat", g_parseFloat);
    reg_method(vm, Number, "parseInt", g_parseInt);
    js_obj_set(vm, Number, js_str_intern(vm,"MAX_SAFE_INTEGER",16), js_mk_num(9007199254740991.0));
    js_obj_set(vm, Number, js_str_intern(vm,"MIN_SAFE_INTEGER",16), js_mk_num(-9007199254740991.0));
    js_obj_set(vm, Number, js_str_intern(vm,"MAX_VALUE",9), js_mk_num(1.7976931348623157e308));
    js_obj_set(vm, Number, js_str_intern(vm,"MIN_VALUE",9), js_mk_num(5e-324));
    js_obj_set(vm, Number, js_str_intern(vm,"EPSILON",7), js_mk_num(2.220446049250313e-16));
    js_obj_set(vm, Number, js_str_intern(vm,"POSITIVE_INFINITY",17), js_mk_num(js_inf(0)));
    js_obj_set(vm, Number, js_str_intern(vm,"NEGATIVE_INFINITY",17), js_mk_num(js_inf(1)));
    js_obj_set(vm, Number, js_str_intern(vm,"NaN",3), js_mk_num(js_nan()));
    js_env_define(vm, vm->global_env, js_str_intern(vm,"Number",6), js_mk_obj(Number), 0);

    /* String (conversion fn + fromCharCode) */
    js_object *String = js_func_new_native(vm, g_String, "String");
    reg_method(vm, String, "fromCharCode", s_fromCharCode);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"String",6), js_mk_obj(String), 0);

    /* Array (conversion fn + isArray) */
    js_object *Array = js_func_new_native(vm, g_Array, "Array");
    reg_method(vm, Array, "isArray", a_isArray);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"Array",5), js_mk_obj(Array), 0);

    /*
     * Wire builtin constructors' `.prototype` to the shared prototype objects so
     * that `instanceof` against the builtins works for the common cases:
     *   [] instanceof Array, {} instanceof Object, etc.
     * Also chain the builtin prototypes up to Object.prototype so that
     * `[] instanceof Object` and friends hold too.
     */
    vm->proto_array->proto  = vm->proto_object;
    vm->proto_string->proto = vm->proto_object;
    vm->proto_number->proto = vm->proto_object;
    vm->proto_function->proto = vm->proto_object;
    set_proto_prop(vm, Object, vm->proto_object);
    set_proto_prop(vm, Array,  vm->proto_array);
    set_proto_prop(vm, String, vm->proto_string);
    set_proto_prop(vm, Number, vm->proto_number);

    /* global constants */
    js_env_define(vm, vm->global_env, js_str_intern(vm,"NaN",3), js_mk_num(js_nan()), 1);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"Infinity",8), js_mk_num(js_inf(0)), 1);
    js_env_define(vm, vm->global_env, js_str_intern(vm,"undefined",9), js_mk_undef(), 1);
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */
static void vm_init(js_vm *vm)
{
    /* zero the control fields (arena_store left intact-but-unused) */
    vm->initialized = 0;
    vm->arena = vm->arena_store;
    vm->arena_cap = JS_ARENA_BYTES;
    vm->arena_used = 0;
    vm->arena_mark = 0;
    vm->oom = 0;
    vm->nintern = 0;
    for (int i = 0; i < JS_MAX_INTERN; i++) vm->intern[i] = NULL;
    vm->proto_string = vm->proto_array = vm->proto_object = NULL;
    vm->proto_number = vm->proto_function = NULL;
    vm->has_exception = 0;
    vm->depth = 0;
    vm->rng = 0x2545F4914F6CDD1DULL;  /* fixed seed (deterministic) */
    vm->errmsg[0] = 0;
    /* keep existing emit sink across re-init within selftest if any */

    /* clear native-class registry (filled later by js_native_register_class) */
    vm->n_native_classes = 0;
    for (int i = 0; i < JS_MAX_NATIVE_CLASSES; i++) vm->native_classes[i] = (void *)0;

    vm->global_env = js_env_new(vm, NULL);
    js_install_builtins(vm);
    js_arena_mark(vm);   /* mark for any embedders that call js_arena_reset */
    vm->initialized = 1;
}

js_vm *js_new(void)
{
    void (*saved_emit)(const char *, js_usize) = g_vm.initialized ? g_vm.emit : NULL;
    vm_init(&g_vm);
    g_vm.emit = saved_emit;
    return &g_vm;
}

void js_set_print(js_vm *vm, void (*emit)(const char *s, unsigned long n))
{
    vm->emit = (void (*)(const char *, js_usize))emit;
}

int js_eval(js_vm *vm, const char *src, unsigned long len,
            char *out_result, unsigned long out_cap)
{
    if (!vm || !out_result || out_cap == 0) return -1;
    out_result[0] = 0;

    /*
     * Each top-level eval starts from a clean slate: reset the arena fully and
     * reinstall builtins + a fresh global environment. This keeps the engine's
     * invariants simple and correct (no dangling pre-mark/post-mark pointers)
     * and reclaims all memory between runs, which is exactly the documented
     * "no-free arena reset per top-level run" policy. The intern table is
     * cleared because its entries point into the arena we just reset.
     */
    vm->arena_used = 0;
    vm->oom = 0;
    vm->nintern = 0;
    for (int i = 0; i < JS_MAX_INTERN; i++) vm->intern[i] = NULL;
    vm->has_exception = 0;
    vm->depth = 0;
    vm->proto_string = vm->proto_array = vm->proto_object = NULL;
    vm->proto_number = vm->proto_function = NULL;
    /* native-class registry is also reset; the embedder must re-register
     * (DOM bindings etc.) AFTER each js_eval that should see them. */
    vm->n_native_classes = 0;
    for (int i = 0; i < JS_MAX_NATIVE_CLASSES; i++) vm->native_classes[i] = (void *)0;
    vm->global_env = js_env_new(vm, NULL);
    js_install_builtins(vm);

    js_node *prog = js_parse_program(vm, src, len);
    if (!prog || vm->oom) {
        const char *m = (vm->oom) ? "Out of memory" : js_parse_error();
        js_usize i = 0;
        while (m[i] && i < out_cap - 1) { out_result[i] = m[i]; i++; }
        out_result[i] = 0;
        return -1;
    }

    js_value completion;
    int rc = js_run_program(vm, prog, &completion);

    if (rc < 0) {
        /* uncaught exception: format it */
        js_string *es;
        if (completion.type == JS_OBJECT && (completion.u.o->flags & JS_OBJ_ERROR)) {
            js_value msg;
            js_obj_get(vm, completion.u.o, js_str_intern(vm,"message",7), &msg);
            es = js_to_string(vm, msg);
            js_string *pre = js_str_newz(vm, "Uncaught ");
            es = js_str_concat(vm, pre, es);
        } else {
            js_string *pre = js_str_newz(vm, "Uncaught ");
            es = js_str_concat(vm, pre, js_to_string(vm, completion));
        }
        js_usize i = 0;
        if (es) while (i < es->len && i < out_cap - 1) { out_result[i]=es->data[i]; i++; }
        out_result[i] = 0;
        return -1;
    }

    /* success: stringify completion value */
    js_string *s = js_to_string(vm, completion);
    js_usize i = 0;
    if (s) while (i < s->len && i < out_cap - 1) { out_result[i] = s->data[i]; i++; }
    out_result[i] = 0;
    return 0;
}

/*
 * js_eval_keep_env -- like js_eval but no reset. Lets embedder-registered
 * native classes and global values (e.g. the DOM `document` global) survive
 * across calls. See js.h.
 */
int js_eval_keep_env(js_vm *vm, const char *src, unsigned long len,
                     char *out_result, unsigned long out_cap)
{
    if (!vm || !out_result || out_cap == 0) return -1;
    out_result[0] = 0;

    js_node *prog = js_parse_program(vm, src, len);
    if (!prog || vm->oom) {
        const char *m = (vm->oom) ? "Out of memory" : js_parse_error();
        js_usize i = 0;
        while (m[i] && i < out_cap - 1) { out_result[i] = m[i]; i++; }
        out_result[i] = 0;
        return -1;
    }

    js_value completion;
    int rc = js_run_program(vm, prog, &completion);

    if (rc < 0) {
        js_string *es;
        if (completion.type == JS_OBJECT && (completion.u.o->flags & JS_OBJ_ERROR)) {
            js_value msg;
            js_obj_get(vm, completion.u.o, js_str_intern(vm,"message",7), &msg);
            es = js_to_string(vm, msg);
            js_string *pre = js_str_newz(vm, "Uncaught ");
            es = js_str_concat(vm, pre, es);
        } else {
            js_string *pre = js_str_newz(vm, "Uncaught ");
            es = js_str_concat(vm, pre, js_to_string(vm, completion));
        }
        js_usize i = 0;
        if (es) while (i < es->len && i < out_cap - 1) { out_result[i]=es->data[i]; i++; }
        out_result[i] = 0;
        return -1;
    }

    js_string *s = js_to_string(vm, completion);
    js_usize i = 0;
    if (s) while (i < s->len && i < out_cap - 1) { out_result[i] = s->data[i]; i++; }
    out_result[i] = 0;
    return 0;
}

/* ================================================================== */
/*  Self-test                                                         */
/* ================================================================== */
/* format a u32 into buf (no NUL); returns number of digits written */
static js_usize fmt_uint(js_u32 v, char *buf)
{
    char rev[12];
    js_usize rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
    for (js_usize i = 0; i < rn; i++) buf[i] = rev[rn - 1 - i];
    return rn;
}

static void dflt_emit(const char *s, js_usize n)
{
    /* default sink: write(1, s, n) via inline syscall (SYS_WRITE=3) */
    long r;
    register long r10 asm("r10") = 0, r8 asm("r8") = 0;
    asm volatile("syscall" : "=a"(r)
                 : "a"(3L), "D"(1L), "S"((long)s), "d"((long)n), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    (void)r;
}

static int str_eq_c(const char *a, const char *b)
{
    js_usize i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

int js_selftest(void)
{
    js_vm *vm = js_new();
    if (!vm->emit) js_set_print(vm, dflt_emit);

    struct { const char *src; const char *want; } cases[] = {
        { "1+2*3", "7" },
        { "2**10", "1024" },
        { "(1+2)*3", "9" },
        { "10/4", "2.5" },
        { "7%3", "1" },
        { "-3*-3", "9" },
        { "1<2 && 2<3", "true" },
        { "5 & 3", "1" },
        { "5 | 2", "7" },
        { "1 << 4", "16" },
        { "-1 >>> 28", "15" },
        { "typeof 1", "number" },
        { "typeof 'x'", "string" },
        { "typeof undefined", "undefined" },
        { "1 == '1'", "true" },
        { "1 === '1'", "false" },
        { "null == undefined", "true" },
        { "var s=0; for(var i=1;i<=5;i++) s+=i; s", "15" },
        { "var p=1; for(var i=1;i<=5;i++) p*=i; p", "120" },
        { "var n=0,i=0; while(i<10){n+=i;i++} n", "45" },
        { "var x=0; do { x++ } while(x<3); x", "3" },
        { "function add(a,b){return a+b} add(40,2)", "42" },
        { "function fib(n){return n<2?n:fib(n-1)+fib(n-2)} fib(10)", "55" },
        { "function mk(){var c=0; return function(){return ++c}} var f=mk(); f();f();f()", "3" },
        { "var a=function(x){return x*x}; a(7)", "49" },
        { "var sq = x => x*x; sq(6)", "36" },
        { "var add = (a,b) => a+b; add(3,4)", "7" },
        { "'ab'+'cd'", "abcd" },
        { "'a'+1", "a1" },
        { "1+'a'", "1a" },
        { "'hello'.toUpperCase()", "HELLO" },
        { "'HELLO'.toLowerCase()", "hello" },
        { "'hello'.length", "5" },
        { "'hello'.charAt(1)", "e" },
        { "'hello world'.indexOf('world')", "6" },
        { "'a,b,c'.split(',').length", "3" },
        { "'hello'.slice(1,3)", "el" },
        { "'  hi  '.trim()", "hi" },
        { "'ab'.repeat(3)", "ababab" },
        { "[3,1,2].length", "3" },
        { "[1,2,3].join('-')", "1-2-3" },
        { "[1,2,3].map(function(x){return x*2}).join(',')", "2,4,6" },
        { "[1,2,3,4].filter(function(x){return x%2==0}).join(',')", "2,4" },
        { "[1,2,3,4].reduce(function(a,b){return a+b},0)", "10" },
        { "var t=0; [1,2,3].forEach(function(x){t+=x}); t", "6" },
        { "[3,1,2].sort().join(',')", "1,2,3" },
        { "[1,2,3].indexOf(2)", "1" },
        { "[1,2,3].includes(2)", "true" },
        { "var a=[1,2]; a.push(3); a.length", "3" },
        { "var a=[1,2,3]; a.pop()", "3" },
        { "[1,2,3].reverse().join(',')", "3,2,1" },
        { "[1,2,3].slice(1).join(',')", "2,3" },
        { "[1,2].concat([3,4]).join(',')", "1,2,3,4" },
        { "[1,2,3].some(function(x){return x>2})", "true" },
        { "[1,2,3].every(function(x){return x>0})", "true" },
        { "[1,2,3,4].find(function(x){return x>2})", "3" },
        { "var o={a:1,b:2}; o.a+o.b", "3" },
        { "var o={x:10}; o.y=20; o.x+o.y", "30" },
        { "var o={a:1,b:2,c:3}; Object.keys(o).length", "3" },
        { "var o={a:1,b:2}; Object.values(o).join(',')", "1,2" },
        { "var o={a:1}; o.hasOwnProperty('a')", "true" },
        { "JSON.stringify({x:1,y:[2,3]})", "{\"x\":1,\"y\":[2,3]}" },
        { "JSON.stringify([1,2,3])", "[1,2,3]" },
        { "JSON.stringify('hi')", "\"hi\"" },
        { "JSON.parse('[1,2,3]').length", "3" },
        { "JSON.parse('{\"a\":5}').a", "5" },
        { "JSON.parse('{\"n\":{\"m\":7}}').n.m", "7" },
        { "Math.max(1,9,4)", "9" },
        { "Math.min(3,1,2)", "1" },
        { "Math.abs(-5)", "5" },
        { "Math.floor(3.7)", "3" },
        { "Math.ceil(3.2)", "4" },
        { "Math.round(3.5)", "4" },
        { "Math.sqrt(144)", "12" },
        { "Math.pow(2,8)", "256" },
        { "Math.PI > 3.14 && Math.PI < 3.15", "true" },
        { "parseInt('42')", "42" },
        { "parseInt('0xff')", "255" },
        { "parseInt('101',2)", "5" },
        { "parseFloat('3.14xyz')", "3.14" },
        { "isNaN(NaN)", "true" },
        { "isNaN(5)", "false" },
        { "Number('123')", "123" },
        { "String(456)", "456" },
        { "Boolean(0)", "false" },
        { "Boolean('x')", "true" },
        { "true ? 'yes' : 'no'", "yes" },
        { "var x = 5; x > 3 ? 'big' : 'small'", "big" },
        { "var a; a ?? 'default'", "default" },
        { "var a=0; a ?? 'default'", "0" },
        { "try { throw 'boom' } catch(e) { e }", "boom" },
        { "try { null.x } catch(e) { 'caught' }", "caught" },
        { "var r=0; try { r=1 } finally { r=2 } r", "2" },
        { "function f(){ if(true) return 1; return 2 } f()", "1" },
        { "[1,2,3,4,5].filter(x=>x>2).map(x=>x*10).join(',')", "30,40,50" },
        { "var sum=0; for(var x of [10,20,30]) sum+=x; sum", "60" },
        { "var keys=''; for(var k in {a:1,b:2}) keys+=k; keys", "ab" },
        { "[1,[2,[3]]].length", "2" },
        { "({a:{b:{c:42}}}).a.b.c", "42" },
        { "5 .toString()", "5" },
        { "(3.14159).toFixed(2)", "3.14" },

        /* ---- newly added breadth (P1: run more real page scripts) ---- */
        /* Array.prototype.splice (mutating) */
        { "var a=[1,2,3,4,5]; a.splice(1,2); a.join(',')", "1,4,5" },
        { "var a=[1,2,3,4,5]; a.splice(1,2).join(',')", "2,3" },
        { "var a=[1,2,3]; a.splice(1,0,9,8); a.join(',')", "1,9,8,2,3" },
        { "var a=[1,2,3,4]; a.splice(-2); a.join(',')", "1,2" },
        { "var a=[1,2,3]; a.splice(1,1,'x'); a.join(',')", "1,x,3" },
        /* findIndex / lastIndexOf / flat / isArray */
        { "[5,10,15].findIndex(function(x){return x>8})", "1" },
        { "[10,20,10,30].lastIndexOf(10)", "2" },
        { "[1,[2,[3,[4]]]].flat().join(',')", "1,2,3,4" },
        { "[1,[2,[3]]].flat(2).join(',')", "1,2,3" },
        { "Array.isArray([1,2])", "true" },
        { "Array.isArray('no')", "false" },
        { "Array.isArray({length:3})", "false" },
        /* String: padEnd / trimStart / trimEnd / replaceAll / split-limit */
        { "'5'.padEnd(3,'0')", "500" },
        { "'5'.padStart(3,'0')", "005" },
        { "'  hi'.trimStart()", "hi" },
        { "'hi  '.trimEnd()+'!'", "hi!" },
        { "'a-b-a-b'.replaceAll('a','X')", "X-b-X-b" },
        { "'a,b,c,d'.split(',',2).join('|')", "a|b" },
        { "'abc'.split('').join('-')", "a-b-c" },
        { "String.fromCharCode(72,105)", "Hi" },
        { "'ABC'.charCodeAt(0)", "65" },
        /* Object.create / getPrototypeOf / defineProperty / getOwnPropertyNames */
        { "var p={greet:1}; var o=Object.create(p); o.greet", "1" },
        { "var o=Object.create(null); o.x=5; o.x", "5" },
        { "var p={a:1}; var o=Object.create(p); Object.getPrototypeOf(o).a", "1" },
        { "var o={}; Object.defineProperty(o,'k',{value:7}); o.k", "7" },
        { "var o={}; Object.defineProperty(o,'h',{value:1,enumerable:false}); Object.keys(o).length", "0" },
        { "var o={a:1,b:2}; Object.getOwnPropertyNames(o).join(',')", "a,b" },
        /* Number static predicates + constants */
        { "Number.isInteger(5)", "true" },
        { "Number.isInteger(5.5)", "false" },
        { "Number.isInteger('5')", "false" },
        { "Number.isNaN(NaN)", "true" },
        { "Number.isNaN('x')", "false" },
        { "Number.isFinite(1/0)", "false" },
        { "Number.isFinite(42)", "true" },
        { "Number.isSafeInteger(9007199254740991)", "true" },
        { "Number.MAX_SAFE_INTEGER", "9007199254740991" },
        { "Number.parseInt('0x10')", "16" },
        { "Number.parseFloat('2.5x')", "2.5" },
        /* Math.log2 / log10 */
        { "Math.log2(8)", "3" },
        { "Math.log10(1000)", "3" },
        /* instanceof (user constructors + builtins) */
        { "function P(n){this.n=n} var p=new P(7); p.n", "7" },
        { "function P(){} var p=new P(); p instanceof P", "true" },
        { "function P(){} function Q(){} (new P()) instanceof Q", "false" },
        { "function P(){} P.prototype.hi=function(){return 42}; (new P()).hi()", "42" },
        { "[] instanceof Array", "true" },
        { "({}) instanceof Object", "true" },
        { "[] instanceof Object", "true" },
        { "({}) instanceof Array", "false" },
        { "5 instanceof Number", "false" },
        /* prototype-chain method inheritance via new */
        { "function C(){this.x=10} C.prototype.dbl=function(){return this.x*2}; var c=new C(); c.dbl()", "20" },
        /* JSON.stringify with indent */
        { "JSON.stringify({a:1},null,2)", "{\n  \"a\": 1\n}" },
        { "JSON.stringify([1,2],null,2)", "[\n  1,\n  2\n]" },
        { "JSON.stringify({a:1})", "{\"a\":1}" },
        /* typeof on function / object / array */
        { "typeof function(){}", "function" },
        { "typeof []", "object" },
        { "typeof null", "object" },
        { "typeof {}", "object" },
    };

    int total = sizeof(cases)/sizeof(cases[0]);
    int failures = 0;
    char outbuf[512];

    for (int i = 0; i < total; i++) {
        js_vm *v = js_new();
        if (!v->emit) js_set_print(v, dflt_emit);
        int rc = js_eval(v, cases[i].src, js_strlen(cases[i].src), outbuf, sizeof outbuf);
        int pass = (rc == 0) && str_eq_c(outbuf, cases[i].want);
        if (!pass) {
            failures++;
            dflt_emit("FAIL: ", 6);
            dflt_emit(cases[i].src, js_strlen(cases[i].src));
            dflt_emit("  => got [", 10);
            dflt_emit(outbuf, js_strlen(outbuf));
            dflt_emit("] want [", 8);
            dflt_emit(cases[i].want, js_strlen(cases[i].want));
            dflt_emit("]\n", 2);
        }
    }

    /* print a numeric summary: "JS tests: <passed>/<total> pass\n" */
    char sb[64];
    js_usize p = 0;
    const char *lab = "JS tests: ";
    for (js_usize k = 0; lab[k]; k++) sb[p++] = lab[k];
    p += fmt_uint((js_u32)(total - failures), sb + p);
    sb[p++] = '/';
    p += fmt_uint((js_u32)total, sb + p);
    const char *suf = " pass\n";
    for (js_usize k = 0; suf[k]; k++) sb[p++] = suf[k];
    dflt_emit(sb, p);

    return failures;
}

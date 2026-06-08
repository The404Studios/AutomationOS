/*
 * js_console.c -- full console API for the AutomationOS JS engine.
 * =================================================================
 *
 * See js_console.h for the full surface description.
 *
 * Freestanding: no libc, no stdio, no malloc.  All helpers come from
 * js_internal.h (js_strlen, js_memcpy, js_memset, js_memcmp, js_dtoa,
 * js_to_string, js_value_to_display, json_stringify_rec via the builtin
 * path, etc.).
 *
 * The implementation deliberately mirrors the style of js_builtin.c
 * so it can be compiled with identical flags.
 */

#include "js_internal.h"    /* full internal types + helper declarations */
#include "js_console.h"

/* ================================================================== */
/*  Module-static state                                               */
/* ================================================================== */

/* Current indentation level (0 = no indent). */
static int  g_indent;

/* --- Timer map for console.time / timeEnd --- */
typedef struct {
    char     label[JS_CON_KEY_MAX];
    js_u64   start_ms;
    int      used;
} con_timer;
static con_timer g_timers[JS_CON_MAX_TIMERS];

/* --- Counter map for console.count --- */
typedef struct {
    char   label[JS_CON_KEY_MAX];
    int    count;
    int    used;
} con_counter;
static con_counter g_counters[JS_CON_MAX_COUNTERS];

/* ================================================================== */
/*  Syscall: SYS_GET_TICKS_MS = 40                                    */
/* ================================================================== */
static js_u64 con_get_ticks_ms(void)
{
    js_u64 r;
    /* ABI: rax=40, no args; return value in rax. */
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"((long)40)
                 : "rcx", "r11", "memory");
    return r;
}

/* ================================================================== */
/*  Low-level emit helpers                                            */
/* ================================================================== */

/* Emit via the VM's registered sink (vm->emit).  No-op if sink is NULL. */
static void con_emit(js_vm *vm, const char *s, js_usize n)
{
    if (vm->emit && n > 0) vm->emit(s, n);
}

/* Emit a NUL-terminated literal. */
static void con_emitz(js_vm *vm, const char *s)
{
    con_emit(vm, s, js_strlen(s));
}

/* Emit the current indentation (2 spaces per level). */
static void con_emit_indent(js_vm *vm)
{
    static const char spaces[] = "                                "; /* 32 sp */
    int n = g_indent * 2;
    if (n > 32) n = 32;
    if (n > 0) con_emit(vm, spaces, (js_usize)n);
}

/* Format a uint64 into buf (no NUL); returns number of bytes written. */
static js_usize con_fmt_u64(js_u64 v, char *buf)
{
    char rev[20];
    js_usize rn = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v) { rev[rn++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (js_usize i = 0; i < rn; i++) buf[i] = rev[rn - 1 - i];
    return rn;
}

/* Format a double into buf using js_dtoa; returns length. */
static js_usize con_fmt_double(double n, char *buf, js_usize cap)
{
    return js_dtoa(n, buf, cap);
}

/* ================================================================== */
/*  Key (label) helpers                                               */
/* ================================================================== */
static void con_key_copy(char *dst, const char *src)
{
    js_usize i = 0;
    while (i < JS_CON_KEY_MAX - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int con_key_eq(const char *a, const char *b)
{
    js_usize i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* ================================================================== */
/*  Object / value dump (for console.dir and %o)                      */
/* ================================================================== */

/*
 * Forward-declared recursive dump.  Writes into the VM arena via the
 * string-concat path.  Returns a js_string* (never NULL; falls back to
 * "undefined" or "null" on OOM-like conditions).
 */
static js_string *con_dump_value(js_vm *vm, js_value v, int depth);

static js_string *con_dump_object(js_vm *vm, js_object *o, int depth)
{
    if (depth > 8) return js_str_newz(vm, "{...}");

    js_string *acc = js_str_newz(vm, "{ ");
    js_prop  *ord[256];
    js_usize  cnt = js_obj_ordered(o, ord, 256);
    int first = 1;
    for (js_usize i = 0; i < cnt; i++) {
        js_prop *p = ord[i];
        if (!p->enumerable) continue;
        if (!first) acc = js_str_concat(vm, acc, js_str_newz(vm, ", "));
        first = 0;
        /* key */
        acc = js_str_concat(vm, acc, p->key ? js_mk_str(p->key).u.s : js_str_newz(vm, "?"));
        acc = js_str_concat(vm, acc, js_str_newz(vm, ": "));
        /* value */
        acc = js_str_concat(vm, acc, con_dump_value(vm, p->val, depth + 1));
    }
    if (first) return js_str_newz(vm, "{}");
    acc = js_str_concat(vm, acc, js_str_newz(vm, " }"));
    return acc;
}

static js_string *con_dump_array(js_vm *vm, js_object *a, int depth)
{
    if (depth > 8) return js_str_newz(vm, "[...]");
    js_string *acc = js_str_newz(vm, "[ ");
    for (js_usize i = 0; i < a->length; i++) {
        if (i) acc = js_str_concat(vm, acc, js_str_newz(vm, ", "));
        acc = js_str_concat(vm, acc, con_dump_value(vm, a->elems[i], depth + 1));
    }
    if (a->length == 0) return js_str_newz(vm, "[]");
    acc = js_str_concat(vm, acc, js_str_newz(vm, " ]"));
    return acc;
}

static js_string *con_dump_value(js_vm *vm, js_value v, int depth)
{
    switch (v.type) {
    case JS_UNDEFINED: return js_str_newz(vm, "undefined");
    case JS_NULL:      return js_str_newz(vm, "null");
    case JS_BOOL:      return js_str_newz(vm, v.u.b ? "true" : "false");
    case JS_NUMBER: {
        char buf[JS_NUMBUF_CAP];
        js_usize n = con_fmt_double(v.u.n, buf, sizeof buf);
        return js_str_new(vm, buf, n);
    }
    case JS_STRING: {
        /* Wrap in double-quotes for dump representation. */
        js_string *inner = v.u.s;
        js_string *acc = js_str_newz(vm, "\"");
        acc = js_str_concat(vm, acc, inner);
        acc = js_str_concat(vm, acc, js_str_newz(vm, "\""));
        return acc;
    }
    case JS_FUNCTION:
        return js_str_newz(vm, "[Function]");
    case JS_ARRAY:
        return con_dump_array(vm, v.u.o, depth);
    case JS_OBJECT:
        return con_dump_object(vm, v.u.o, depth);
    default:
        return js_str_newz(vm, "?");
    }
}

/* ================================================================== */
/*  Format-specifier processing for log/info/warn/error/debug         */
/* ================================================================== */

/*
 * Process a format string (first argument) with %s/%d/%f/%o substitutions.
 * `args` are the substitution arguments (argv[1..argc-1]).
 * Returns a js_string* of the result.
 *
 * NOT handled (emitted as literal):  %i %x %c %O
 * %% is emitted as a single '%'.
 */
static js_string *con_format(js_vm *vm, js_string *fmt,
                              js_value *args, int nargs)
{
    js_string *result = js_str_newz(vm, "");
    const char *p   = fmt->data;
    const char *end = fmt->data + fmt->len;
    int arg_idx = 0;

    while (p < end) {
        if (*p != '%' || p + 1 >= end) {
            /* emit one byte */
            result = js_str_concat(vm, result, js_str_new(vm, p, 1));
            p++;
            continue;
        }
        char spec = *(p + 1);
        if (spec == '%') {
            result = js_str_concat(vm, result, js_str_newz(vm, "%"));
            p += 2;
            continue;
        }
        if (spec != 's' && spec != 'd' && spec != 'f' && spec != 'o') {
            /* unknown specifier: emit literally */
            result = js_str_concat(vm, result, js_str_new(vm, p, 2));
            p += 2;
            continue;
        }
        /* consume the specifier, get corresponding argument */
        js_value av = (arg_idx < nargs) ? args[arg_idx++] : js_mk_undef();
        p += 2;

        switch (spec) {
        case 's': {
            result = js_str_concat(vm, result, js_to_string(vm, av));
            break;
        }
        case 'd': {
            char buf[24];
            js_i64 iv = (js_i64)js_to_number(vm, av);
            int neg = iv < 0;
            js_u64 uv = neg ? (js_u64)0 - (js_u64)iv : (js_u64)iv;
            js_usize n = con_fmt_u64(uv, buf + 1);
            if (neg) { buf[0] = '-'; result = js_str_concat(vm, result, js_str_new(vm, buf, n + 1)); }
            else     { result = js_str_concat(vm, result, js_str_new(vm, buf + 1, n)); }
            break;
        }
        case 'f': {
            char buf[JS_NUMBUF_CAP];
            js_usize n = con_fmt_double(js_to_number(vm, av), buf, sizeof buf);
            result = js_str_concat(vm, result, js_str_new(vm, buf, n));
            break;
        }
        case 'o': {
            result = js_str_concat(vm, result, con_dump_value(vm, av, 0));
            break;
        }
        }
    }
    return result;
}

/* ================================================================== */
/*  Core log emitter (handles prefix, indent, format, trailing args)  */
/* ================================================================== */
static void con_log_core(js_vm *vm, const char *prefix,
                          js_value *argv, int argc)
{
    con_emit_indent(vm);
    if (prefix) con_emitz(vm, prefix);

    if (argc == 0) {
        con_emitz(vm, "\n");
        return;
    }

    /*
     * If the first argument is a string AND contains a '%' followed by a
     * known specifier, run the format path.  Remaining (unconsumed)
     * arguments after format substitution are appended space-separated.
     */
    int fmt_consumed = 0;
    js_string *first_s = NULL;

    if (argv[0].type == JS_STRING) {
        first_s = argv[0].u.s;
        /* Quick check: does it have a '%'? */
        for (js_usize k = 0; k + 1 < first_s->len; k++) {
            if (first_s->data[k] == '%') {
                char sp = first_s->data[k + 1];
                if (sp == 's' || sp == 'd' || sp == 'f' || sp == 'o' || sp == '%') {
                    /* Use format path */
                    js_string *out = con_format(vm, first_s,
                                                argv + 1, argc - 1);
                    /* format() consumes all args via substitution indices;
                     * any excess args are appended space-separated below.
                     * For simplicity we treat ALL remaining args as consumed
                     * by the format string (matching browser behaviour). */
                    con_emit(vm, out->data, out->len);
                    fmt_consumed = 1;
                    break;
                }
            }
        }
    }

    if (!fmt_consumed) {
        /* Plain space-separated display, like the original console.log. */
        for (int i = 0; i < argc; i++) {
            if (i) con_emit(vm, " ", 1);
            js_string *s = js_value_to_display(vm, argv[i]);
            if (s) con_emit(vm, s->data, s->len);
        }
    }

    con_emit(vm, "\n", 1);
}

/* ================================================================== */
/*  Native method implementations                                      */
/* ================================================================== */

/* --- console.log --- */
static int con_log(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_log_core(vm, NULL, a, n);
    *out = js_mk_undef();
    return 0;
}

/* --- console.info --- */
static int con_info(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_log_core(vm, "[info] ", a, n);
    *out = js_mk_undef();
    return 0;
}

/* --- console.debug --- */
static int con_debug(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_log_core(vm, "[debug] ", a, n);
    *out = js_mk_undef();
    return 0;
}

/* --- console.warn --- */
static int con_warn(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_log_core(vm, "[warn] ", a, n);
    *out = js_mk_undef();
    return 0;
}

/* --- console.error --- */
static int con_error(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_log_core(vm, "[error] ", a, n);
    *out = js_mk_undef();
    return 0;
}

/* --- console.group(label) --- */
static int con_group(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    con_emit_indent(vm);
    if (n > 0) {
        js_string *s = js_value_to_display(vm, a[0]);
        if (s) con_emit(vm, s->data, s->len);
    }
    con_emit(vm, "\n", 1);
    if (g_indent < JS_CON_MAX_INDENT) g_indent++;
    *out = js_mk_undef();
    return 0;
}

/* --- console.groupEnd() --- */
static int con_groupEnd(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)vm;(void)t;(void)a;(void)n;
    if (g_indent > 0) g_indent--;
    *out = js_mk_undef();
    return 0;
}

/* --- console.time(label) --- */
static int con_time(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *lbl = (n > 0) ? js_to_string(vm, a[0]) : js_str_newz(vm, "default");
    /* find or allocate a slot */
    const char *ldata = lbl->data;
    for (int i = 0; i < JS_CON_MAX_TIMERS; i++) {
        if (g_timers[i].used && con_key_eq(g_timers[i].label, ldata)) {
            /* already exists: overwrite silently */
            g_timers[i].start_ms = con_get_ticks_ms();
            *out = js_mk_undef();
            return 0;
        }
    }
    for (int i = 0; i < JS_CON_MAX_TIMERS; i++) {
        if (!g_timers[i].used) {
            con_key_copy(g_timers[i].label, ldata);
            g_timers[i].start_ms = con_get_ticks_ms();
            g_timers[i].used = 1;
            *out = js_mk_undef();
            return 0;
        }
    }
    /* table full: silently drop */
    *out = js_mk_undef();
    return 0;
}

/* --- console.timeEnd(label) --- */
static int con_timeEnd(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *lbl = (n > 0) ? js_to_string(vm, a[0]) : js_str_newz(vm, "default");
    const char *ldata = lbl->data;
    js_u64 now = con_get_ticks_ms();
    for (int i = 0; i < JS_CON_MAX_TIMERS; i++) {
        if (g_timers[i].used && con_key_eq(g_timers[i].label, ldata)) {
            js_u64 elapsed = now - g_timers[i].start_ms;
            g_timers[i].used = 0;

            /* emit: "<label>: <N>ms" */
            con_emit_indent(vm);
            con_emit(vm, ldata, js_strlen(ldata));
            con_emitz(vm, ": ");
            char nbuf[24];
            js_usize nlen = con_fmt_u64(elapsed, nbuf);
            con_emit(vm, nbuf, nlen);
            con_emitz(vm, "ms\n");
            *out = js_mk_undef();
            return 0;
        }
    }
    /* label not found */
    con_emit_indent(vm);
    con_emitz(vm, "[warn] console.timeEnd: timer not found: ");
    con_emit(vm, ldata, js_strlen(ldata));
    con_emitz(vm, "\n");
    *out = js_mk_undef();
    return 0;
}

/* --- console.count(label) --- */
static int con_count(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_string *lbl = (n > 0) ? js_to_string(vm, a[0]) : js_str_newz(vm, "default");
    const char *ldata = lbl->data;
    /* find existing counter */
    for (int i = 0; i < JS_CON_MAX_COUNTERS; i++) {
        if (g_counters[i].used && con_key_eq(g_counters[i].label, ldata)) {
            g_counters[i].count++;
            /* emit "<label>: N" */
            con_emit_indent(vm);
            con_emit(vm, ldata, js_strlen(ldata));
            con_emitz(vm, ": ");
            char nbuf[24];
            js_usize nlen = con_fmt_u64((js_u64)g_counters[i].count, nbuf);
            con_emit(vm, nbuf, nlen);
            con_emitz(vm, "\n");
            *out = js_mk_undef();
            return 0;
        }
    }
    /* allocate new slot */
    for (int i = 0; i < JS_CON_MAX_COUNTERS; i++) {
        if (!g_counters[i].used) {
            con_key_copy(g_counters[i].label, ldata);
            g_counters[i].count = 1;
            g_counters[i].used  = 1;
            con_emit_indent(vm);
            con_emit(vm, ldata, js_strlen(ldata));
            con_emitz(vm, ": 1\n");
            *out = js_mk_undef();
            return 0;
        }
    }
    /* table full */
    *out = js_mk_undef();
    return 0;
}

/* --- console.dir(obj) --- */
static int con_dir(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    js_value v = (n > 0) ? a[0] : js_mk_undef();
    con_emit_indent(vm);
    js_string *s = con_dump_value(vm, v, 0);
    con_emit(vm, s->data, s->len);
    con_emitz(vm, "\n");
    *out = js_mk_undef();
    return 0;
}

/* --- console.assert(cond, msg) --- */
static int con_assert(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    int cond = (n > 0) ? js_truthy(a[0]) : 0;
    if (!cond) {
        con_emit_indent(vm);
        con_emitz(vm, "Assertion failed");
        if (n > 1) {
            con_emitz(vm, ": ");
            js_string *msg = js_value_to_display(vm, a[1]);
            if (msg) con_emit(vm, msg->data, msg->len);
        }
        con_emitz(vm, "\n");
    }
    *out = js_mk_undef();
    return 0;
}

/* ================================================================== */
/*  console.table                                                     */
/* ================================================================== */

/*
 * Basic ASCII table.  Expects an array of objects.  Header is derived
 * from the keys of the first element.  Maximum 16 columns, 256 rows.
 */
#define CON_TABLE_MAX_COLS  16
#define CON_TABLE_MAX_ROWS 256
#define CON_TABLE_MAX_COLW  32  /* max column width before truncation  */

static void con_table_hline(js_vm *vm, int ncols, int *widths)
{
    con_emit_indent(vm);
    con_emitz(vm, "+");
    for (int c = 0; c < ncols; c++) {
        for (int k = 0; k < widths[c] + 2; k++) con_emitz(vm, "-");
        con_emitz(vm, "+");
    }
    con_emitz(vm, "\n");
}

static void con_table_cell(js_vm *vm, const char *s, js_usize slen, int width)
{
    con_emitz(vm, " ");
    /* emit min(slen, width) bytes */
    js_usize emit_n = slen < (js_usize)width ? slen : (js_usize)width;
    con_emit(vm, s, emit_n);
    /* pad with spaces */
    int pad = width - (int)emit_n;
    while (pad-- > 0) con_emitz(vm, " ");
    con_emitz(vm, " |");
}

static int con_table(js_vm *vm, js_value t, js_value *a, int n, js_value *out)
{
    (void)t;
    if (n == 0 || a[0].type != JS_ARRAY) {
        /* Fall back to dir */
        return con_dir(vm, t, a, n, out);
    }
    js_object *arr = a[0].u.o;
    if (arr->length == 0) {
        con_emit_indent(vm);
        con_emitz(vm, "(empty table)\n");
        *out = js_mk_undef();
        return 0;
    }

    /* Collect column names from first element. */
    if (arr->elems[0].type != JS_OBJECT) {
        return con_dir(vm, t, a, n, out);
    }

    js_prop *ord[CON_TABLE_MAX_COLS];
    js_usize ncols = js_obj_ordered(arr->elems[0].u.o, ord, CON_TABLE_MAX_COLS);

    /* For each column: label + max width across all rows. */
    const char *col_names[CON_TABLE_MAX_COLS];
    int         col_widths[CON_TABLE_MAX_COLS];
    js_usize ci;
    for (ci = 0; ci < ncols; ci++) {
        col_names[ci]  = ord[ci]->key ? ord[ci]->key->data : "";
        int klen = (int)js_strlen(col_names[ci]);
        col_widths[ci] = klen < 1 ? 1 : (klen > CON_TABLE_MAX_COLW ? CON_TABLE_MAX_COLW : klen);
    }

    /* Build cell strings and widen columns. */
    /* We'll store cell strings as arena-allocated js_string*s. */
    js_usize nrows = arr->length;
    if (nrows > CON_TABLE_MAX_ROWS) nrows = CON_TABLE_MAX_ROWS;

    /* cells[row][col] -- allocated on arena (temporary), OK for render. */
    js_string **cells[CON_TABLE_MAX_ROWS];
    for (js_usize r = 0; r < nrows; r++) {
        cells[r] = (js_string **)js_arena_alloc(vm,
                    ncols * sizeof(js_string *));
        if (!cells[r]) goto fallback;
        js_value row = arr->elems[r];
        for (ci = 0; ci < ncols; ci++) {
            js_string *cv;
            if (row.type == JS_OBJECT) {
                js_value cell;
                if (js_obj_get(vm, row.u.o, ord[ci]->key, &cell) == 0) {
                    cv = js_value_to_display(vm, cell);
                } else {
                    cv = js_str_newz(vm, "");
                }
            } else {
                cv = js_value_to_display(vm, row);
            }
            if (!cv) cv = js_str_newz(vm, "");
            cells[r][ci] = cv;
            int clen = (int)cv->len;
            if (clen > CON_TABLE_MAX_COLW) clen = CON_TABLE_MAX_COLW;
            if (clen > col_widths[ci]) col_widths[ci] = clen;
        }
    }

    /* Render */
    con_table_hline(vm, (int)ncols, col_widths);

    /* Header row */
    con_emit_indent(vm);
    con_emitz(vm, "|");
    for (ci = 0; ci < ncols; ci++) {
        con_table_cell(vm, col_names[ci], js_strlen(col_names[ci]),
                       col_widths[ci]);
    }
    con_emitz(vm, "\n");
    con_table_hline(vm, (int)ncols, col_widths);

    /* Data rows */
    for (js_usize r = 0; r < nrows; r++) {
        con_emit_indent(vm);
        con_emitz(vm, "|");
        for (ci = 0; ci < ncols; ci++) {
            js_string *cv = cells[r][ci];
            con_table_cell(vm, cv->data, cv->len, col_widths[ci]);
        }
        con_emitz(vm, "\n");
    }
    con_table_hline(vm, (int)ncols, col_widths);

    *out = js_mk_undef();
    return 0;

fallback:
    return con_dir(vm, t, a, n, out);
}

/* ================================================================== */
/*  Install                                                            */
/* ================================================================== */

/*
 * Helper: install a native function as a property of `obj`.
 * Mirrors js_builtin.c's reg_method / reg_global.
 */
static void con_reg_method(js_vm *vm, js_object *obj,
                            const char *name, js_native_fn fn)
{
    js_object *f = js_func_new_native(vm, fn, name);
    if (f) js_obj_set(vm, obj,
                      js_str_intern(vm, name, js_strlen(name)),
                      js_mk_obj(f));
}

void js_console_install(js_vm *vm)
{
    /*
     * Create a new console object and install ALL methods.
     * Then bind it as the global `console`, replacing whatever
     * js_install_builtins() put there.
     */
    js_object *con = js_object_new(vm);
    if (!con) return;

    con_reg_method(vm, con, "log",      con_log);
    con_reg_method(vm, con, "info",     con_info);
    con_reg_method(vm, con, "debug",    con_debug);
    con_reg_method(vm, con, "warn",     con_warn);
    con_reg_method(vm, con, "error",    con_error);
    con_reg_method(vm, con, "group",    con_group);
    con_reg_method(vm, con, "groupEnd", con_groupEnd);
    con_reg_method(vm, con, "time",     con_time);
    con_reg_method(vm, con, "timeEnd",  con_timeEnd);
    con_reg_method(vm, con, "count",    con_count);
    con_reg_method(vm, con, "dir",      con_dir);
    con_reg_method(vm, con, "assert",   con_assert);
    con_reg_method(vm, con, "table",    con_table);

    /* Overwrite (or create) the global `console` binding. */
    js_string *key = js_str_intern(vm, "console", 7);
    js_env_set(vm, vm->global_env, key, js_mk_obj(con));
}

/* ================================================================== */
/*  Self-test                                                          */
/* ================================================================== */

/* Capture sink: collects emitted bytes into a static buffer. */
#define SELFTEST_BUFCAP (4096)
static char     st_buf[SELFTEST_BUFCAP];
static js_usize st_buf_n;
static int      st_overflow;

static void st_emit(const char *s, js_usize n)
{
    for (js_usize i = 0; i < n; i++) {
        if (st_buf_n >= SELFTEST_BUFCAP - 1) { st_overflow = 1; return; }
        st_buf[st_buf_n++] = s[i];
    }
}

static void st_reset(void)
{
    st_buf_n   = 0;
    st_overflow = 0;
    st_buf[0]  = 0;
    /* Also reset module-static state that persists between tests. */
    g_indent = 0;
    for (int i = 0; i < JS_CON_MAX_TIMERS;   i++) g_timers[i].used   = 0;
    for (int i = 0; i < JS_CON_MAX_COUNTERS; i++) g_counters[i].used = 0;
}

/* NUL-terminate the capture buffer and return pointer. */
static const char *st_str(void)
{
    st_buf[st_buf_n] = 0;
    return st_buf;
}

/*
 * Emit via write(fd 1) for diagnostics during selftest.
 *
 * CRITICAL: on AutomationOS SYS_WRITE = 3. Do NOT use Linux's SYS_write = 1
 * here -- syscall 1 on AutomationOS is SYS_FORK, so the old "probe Linux 1
 * first" code fork-bombed the system when a selftest assertion failed. This
 * is freestanding-on-AOS code, so we use the AOS number unconditionally.
 */
static void st_dflt_emit(const char *s, js_usize n)
{
    long r;
    register long r10 asm("r10") = 0, r8 asm("r8") = 0;
    /* AutomationOS: SYS_WRITE = 3 */
    asm volatile("syscall" : "=a"(r)
                 : "a"(3L), "D"(1L), "S"((long)s), "d"((long)n), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    (void)r;
}

static void st_report(int idx, const char *label,
                       const char *got, const char *want)
{
    /* report failure to fd 1 */
    char ibuf[8];
    js_usize ilen = con_fmt_u64((js_u64)(idx < 0 ? 0 : idx), ibuf);
    st_dflt_emit("FAIL #", 6);
    st_dflt_emit(ibuf, ilen);
    st_dflt_emit(" [", 2);
    st_dflt_emit(label, js_strlen(label));
    st_dflt_emit("]\n  got : [", 10);
    st_dflt_emit(got,  js_strlen(got));
    st_dflt_emit("]\n  want: [", 10);
    st_dflt_emit(want, js_strlen(want));
    st_dflt_emit("]\n", 2);
}

/* Simple string equality. */
static int st_eq(const char *a, const char *b)
{
    js_usize i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}

/* Prefix check. */
static int st_starts(const char *s, const char *prefix)
{
    js_usize i = 0;
    while (prefix[i] && s[i] == prefix[i]) i++;
    return prefix[i] == 0;
}

/* Suffix check. */
static int st_ends(const char *s, const char *suffix)
{
    js_usize slen  = js_strlen(s);
    js_usize sflen = js_strlen(suffix);
    if (sflen > slen) return 0;
    return js_memcmp(s + slen - sflen, suffix, sflen) == 0;
}

/*
 * st_vm -- get a fresh VM wired to the capture sink with console installed.
 * NOTE: Does NOT call js_eval -- that would reset+reinstall builtins and
 * wipe our extended console.  All selftest cases drive the C native
 * functions directly via invoke_con() below.
 */
static js_vm *st_fresh_vm(void)
{
    js_vm *vm = js_new();
    js_set_print(vm, st_emit);
    js_console_install(vm);
    return vm;
}

/*
 * Invoke a console native function by property name on the given VM's
 * console object.  Returns 0 on success.
 * We look up `console` in the global env, then call the named method.
 */
static int invoke_con(js_vm *vm, const char *method,
                       js_value *argv, int argc)
{
    js_value undef = js_mk_undef();
    js_value out   = js_mk_undef();

    /* Lookup the console method by name from global scope. */
    js_value con_v;
    if (!js_env_get(vm->global_env,
                    js_str_intern(vm, "console", 7), &con_v))
        return -1;
    if (con_v.type != JS_OBJECT) return -1;

    js_value method_v;
    if (!js_obj_get(vm, con_v.u.o,
                    js_str_intern(vm, method, js_strlen(method)),
                    &method_v))
        return -1;
    if (method_v.type != JS_FUNCTION) return -1;

    js_call_function(vm, method_v, con_v, argv, argc, &out);
    (void)undef;
    return 0;
}

/* Convenience: make a JS string value from a C literal (via arena). */
#define MKS(vm, lit) js_mk_str(js_str_newz((vm), (lit)))
#define MKN(n)       js_mk_num((double)(n))
#define MKB(b)       js_mk_bool(b)

int js_console_selftest(void)
{
    int failures = 0;
    int tc = 0;  /* test case index */

#define CHECK_EQ(label, want) do { \
    const char *_got = st_str(); \
    if (!st_eq(_got, (want))) { \
        failures++; \
        st_report(tc, (label), _got, (want)); \
    } \
    tc++; \
} while(0)

#define CHECK_STARTS(label, prefix) do { \
    const char *_got = st_str(); \
    if (!st_starts(_got, (prefix))) { \
        failures++; \
        st_report(tc, (label), _got, (prefix)); \
    } \
    tc++; \
} while(0)

#define CHECK_ENDS(label, suffix) do { \
    const char *_got = st_str(); \
    if (!st_ends(_got, (suffix))) { \
        failures++; \
        st_report(tc, (label), _got, (suffix)); \
    } \
    tc++; \
} while(0)

    js_vm  *vm;
    js_value args[8];

    /* ---- 1. console.log: basic ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "hello");
    invoke_con(vm, "log", args, 1);
    CHECK_EQ("log basic", "hello\n");

    /* ---- 2. console.log: multiple args ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKN(1); args[1] = MKN(2); args[2] = MKN(3);
    invoke_con(vm, "log", args, 3);
    CHECK_EQ("log multi", "1 2 3\n");

    /* ---- 3. console.info ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "hi");
    invoke_con(vm, "info", args, 1);
    CHECK_EQ("info prefix", "[info] hi\n");

    /* ---- 4. console.debug ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "dbg");
    invoke_con(vm, "debug", args, 1);
    CHECK_EQ("debug prefix", "[debug] dbg\n");

    /* ---- 5. console.warn ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "oops");
    invoke_con(vm, "warn", args, 1);
    CHECK_EQ("warn prefix", "[warn] oops\n");

    /* ---- 6. console.error ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "err");
    invoke_con(vm, "error", args, 1);
    CHECK_EQ("error prefix", "[error] err\n");

    /* ---- 7. %s format specifier ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "hello %s");
    args[1] = MKS(vm, "world");
    invoke_con(vm, "log", args, 2);
    CHECK_EQ("fmt %s", "hello world\n");

    /* ---- 8. %d format specifier ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "val=%d");
    args[1] = MKN(42);
    invoke_con(vm, "log", args, 2);
    CHECK_EQ("fmt %d", "val=42\n");

    /* ---- 9. %f format specifier (integer input) ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "pi~%f");
    args[1] = MKN(3);
    invoke_con(vm, "log", args, 2);
    CHECK_EQ("fmt %f int", "pi~3\n");

    /* ---- 10. %% literal ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "100%%");
    invoke_con(vm, "log", args, 1);
    CHECK_EQ("fmt %%", "100%\n");

    /* ---- 11. %o object dump ---- */
    st_reset(); vm = st_fresh_vm();
    {
        js_object *o = js_object_new(vm);
        js_obj_set(vm, o, js_str_intern(vm, "a", 1), MKN(1));
        args[0] = MKS(vm, "%o");
        args[1] = js_mk_obj(o);
        invoke_con(vm, "log", args, 2);
    }
    CHECK_EQ("fmt %o obj", "{ a: 1 }\n");

    /* ---- 12. console.group / groupEnd indentation ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "G");
    invoke_con(vm, "group",    args, 1);
    args[0] = MKS(vm, "x");
    invoke_con(vm, "log",      args, 1);
    invoke_con(vm, "groupEnd", NULL, 0);
    CHECK_EQ("group indent", "G\n  x\n");

    /* ---- 13. nested group ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "A");
    invoke_con(vm, "group", args, 1);
    args[0] = MKS(vm, "B");
    invoke_con(vm, "group", args, 1);
    args[0] = MKS(vm, "deep");
    invoke_con(vm, "log",      args, 1);
    invoke_con(vm, "groupEnd", NULL,  0);
    invoke_con(vm, "groupEnd", NULL,  0);
    CHECK_EQ("nested group", "A\n  B\n    deep\n");

    /* ---- 14. console.count first call ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "x");
    invoke_con(vm, "count", args, 1);
    CHECK_EQ("count 1", "x: 1\n");

    /* ---- 15. console.count increments ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "y");
    invoke_con(vm, "count", args, 1);
    invoke_con(vm, "count", args, 1);
    invoke_con(vm, "count", args, 1);
    CHECK_EQ("count 3", "y: 1\ny: 2\ny: 3\n");

    /* ---- 16. console.count default label ---- */
    st_reset(); vm = st_fresh_vm();
    invoke_con(vm, "count", NULL, 0);
    CHECK_EQ("count default", "default: 1\n");

    /* ---- 17. console.assert passing (no output) ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKB(1);
    args[1] = MKS(vm, "should not appear");
    invoke_con(vm, "assert", args, 2);
    CHECK_EQ("assert pass", "");

    /* ---- 18. console.assert failing ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKB(0);
    args[1] = MKS(vm, "bad news");
    invoke_con(vm, "assert", args, 2);
    CHECK_EQ("assert fail", "Assertion failed: bad news\n");

    /* ---- 19. console.assert no msg ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKN(0);
    invoke_con(vm, "assert", args, 1);
    CHECK_EQ("assert no msg", "Assertion failed\n");

    /* ---- 20. console.dir object ---- */
    st_reset(); vm = st_fresh_vm();
    {
        js_object *o = js_object_new(vm);
        js_obj_set(vm, o, js_str_intern(vm, "x", 1), MKN(1));
        js_obj_set(vm, o, js_str_intern(vm, "y", 1), MKN(2));
        args[0] = js_mk_obj(o);
        invoke_con(vm, "dir", args, 1);
    }
    CHECK_EQ("dir obj", "{ x: 1, y: 2 }\n");

    /* ---- 21. console.dir array ---- */
    st_reset(); vm = st_fresh_vm();
    {
        js_object *a = js_array_new(vm);
        js_arr_push(vm, a, MKN(1));
        js_arr_push(vm, a, MKN(2));
        js_arr_push(vm, a, MKN(3));
        args[0] = js_mk_obj(a);
        invoke_con(vm, "dir", args, 1);
    }
    CHECK_EQ("dir arr", "[ 1, 2, 3 ]\n");

    /* ---- 22. console.dir string ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "hi");
    invoke_con(vm, "dir", args, 1);
    CHECK_EQ("dir str", "\"hi\"\n");

    /* ---- 23. console.table simple ---- */
    st_reset(); vm = st_fresh_vm();
    {
        /* Build [{a:1,b:2},{a:3,b:4}] */
        js_object *arr = js_array_new(vm);
        js_object *r1  = js_object_new(vm);
        js_object *r2  = js_object_new(vm);
        js_obj_set(vm, r1, js_str_intern(vm,"a",1), MKN(1));
        js_obj_set(vm, r1, js_str_intern(vm,"b",1), MKN(2));
        js_obj_set(vm, r2, js_str_intern(vm,"a",1), MKN(3));
        js_obj_set(vm, r2, js_str_intern(vm,"b",1), MKN(4));
        js_arr_push(vm, arr, js_mk_obj(r1));
        js_arr_push(vm, arr, js_mk_obj(r2));
        args[0] = js_mk_obj(arr);
        invoke_con(vm, "table", args, 1);
    }
    {
        const char *got = st_str();
        int ok = (got[0] == '+') && st_ends(got, "+\n");
        if (!ok) {
            failures++;
            st_report(tc, "table shape",
                      got, "<starts with '+', ends with '+\\n'>");
        }
        tc++;
    }

    /* ---- 24. console.time / timeEnd basic shape ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "t1");
    invoke_con(vm, "time",    args, 1);
    invoke_con(vm, "timeEnd", args, 1);
    {
        const char *got = st_str();
        int ok = st_starts(got, "t1: ") && st_ends(got, "ms\n");
        if (!ok) {
            failures++;
            st_report(tc, "timeEnd shape", got, "t1: <N>ms\\n");
        }
        tc++;
    }

    /* ---- 25. console.timeEnd unknown label ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKS(vm, "no_such");
    invoke_con(vm, "timeEnd", args, 1);
    CHECK_STARTS("timeEnd unknown", "[warn] console.timeEnd: timer not found:");

    /* ---- 26. log no args (just newline) ---- */
    st_reset(); vm = st_fresh_vm();
    invoke_con(vm, "log", NULL, 0);
    CHECK_EQ("log no args", "\n");

    /* ---- 27. log number ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKN(42);
    invoke_con(vm, "log", args, 1);
    CHECK_EQ("log number", "42\n");

    /* ---- 28. log boolean ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = MKB(1); args[1] = MKB(0);
    invoke_con(vm, "log", args, 2);
    CHECK_EQ("log bool", "true false\n");

    /* ---- 29. log null/undefined ---- */
    st_reset(); vm = st_fresh_vm();
    args[0] = js_mk_null(); args[1] = js_mk_undef();
    invoke_con(vm, "log", args, 2);
    CHECK_EQ("log null/undef", "null undefined\n");

    /* ---- 30. log array ---- */
    /* console.log uses js_value_to_display which formats strings with
     * single-quotes in array display, matching the engine's native form. */
    st_reset(); vm = st_fresh_vm();
    {
        js_object *arr = js_array_new(vm);
        js_arr_push(vm, arr, MKN(1));
        js_arr_push(vm, arr, MKS(vm, "x"));
        args[0] = js_mk_obj(arr);
        invoke_con(vm, "log", args, 1);
    }
    CHECK_EQ("log array", "[ 1, 'x' ]\n");

    /* Print summary. */
    {
        char sb[64];
        js_usize p = 0;
        const char *hdr = "js_console selftest: ";
        for (js_usize k = 0; hdr[k]; k++) sb[p++] = hdr[k];
        p += con_fmt_u64((js_u64)(tc - failures), sb + p);
        sb[p++] = '/';
        p += con_fmt_u64((js_u64)tc, sb + p);
        const char *suf = " pass\n";
        for (js_usize k = 0; suf[k]; k++) sb[p++] = suf[k];
        st_dflt_emit(sb, p);
    }

    return failures;

#undef RUN
#undef CHECK_EQ
#undef CHECK_STARTS
#undef CHECK_ENDS
}

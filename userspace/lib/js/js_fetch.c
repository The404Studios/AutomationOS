/*
 * js_fetch.c -- JS fetch() and XMLHttpRequest bindings for AutomationOS.
 * =======================================================================
 *
 * See js_fetch.h for the full design contract.
 *
 * SYNCHRONOUS MODEL:
 *   Both APIs block during the HTTP call (http_get / https_get).  fetch()
 *   returns an already-resolved Response-like JS object; .then(cb) calls
 *   cb immediately.  XMLHttpRequest.send() populates .status /
 *   .responseText before returning, then fires .onload synchronously.
 *
 * Static layout:
 *   g_resp_buf  -- 1 MiB BSS buffer shared by ALL calls (not re-entrant).
 *   g_xhr_pool  -- small pool of XHR state structs (static, no malloc).
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -mstackrealign -O2 \
 *       -c js_fetch.c -o js_fetch.o
 *   objdump -d js_fetch.o | grep fs:0x28   # must be empty
 */

#include "js_fetch.h"
#include "js_internal.h"   /* js_vm internals, value helpers              */
#include "js_native.h"      /* js_native_register_class / wrap / helpers   */
#include "../net/http.h"    /* http_get / https_get                        */
#include "../net/dns.h"     /* dns_resolve (used indirectly by http layer) */

/* ================================================================== */
/*  Freestanding helpers (no libc)                                    */
/* ================================================================== */

static js_usize fetch_strlen(const char *s)
{
    js_usize n = 0;
    while (s[n]) n++;
    return n;
}

static void fetch_memcpy(void *d, const void *s, js_usize n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    for (js_usize i = 0; i < n; i++) dd[i] = ss[i];
}

static void fetch_memset(void *d, int c, js_usize n)
{
    unsigned char *dd = (unsigned char *)d;
    for (js_usize i = 0; i < n; i++) dd[i] = (unsigned char)c;
}

static int fetch_memcmp(const void *a, const void *b, js_usize n)
{
    const unsigned char *aa = (const unsigned char *)a;
    const unsigned char *bb = (const unsigned char *)b;
    for (js_usize i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    }
    return 0;
}

/* Copy at most `cap-1` chars from src to dst, always NUL-terminate. */
static void fetch_strncpy(char *dst, const char *src, js_usize cap)
{
    if (!cap) return;
    js_usize i = 0;
    for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ================================================================== */
/*  Static response buffer (1 MiB BSS -- shared, non-reentrant)      */
/* ================================================================== */
static char g_resp_buf[JS_FETCH_BODY_CAP];

/* ================================================================== */
/*  URL parser                                                        */
/* ================================================================== */
/*
 * Accepted forms:
 *   http://host/path
 *   http://host:port/path
 *   https://host/path
 *   https://host:port/path
 *
 * `out_host` must be at least 256 bytes.
 * `out_path` must be at least 1024 bytes.
 * Returns 1 on success, 0 on parse error.
 */
typedef struct {
    char           host[256];
    char           path[1024];
    unsigned short port;
    int            is_tls;      /* 1 = https */
} fetch_url_t;

static int fetch_parse_url(const char *url, fetch_url_t *out)
{
    fetch_memset(out, 0, sizeof(*out));

    const char *p = url;
    js_usize ulen = fetch_strlen(url);
    const char *end = url + ulen;

    /* scheme */
    if (ulen >= 8 && fetch_memcmp(p, "https://", 8) == 0) {
        out->is_tls = 1;
        out->port   = 443;
        p += 8;
    } else if (ulen >= 7 && fetch_memcmp(p, "http://", 7) == 0) {
        out->is_tls = 0;
        out->port   = 80;
        p += 7;
    } else {
        return 0; /* unsupported scheme */
    }

    if (p >= end) return 0; /* no host */

    /* host (and optional :port) -- terminated by '/' or end of string */
    const char *host_start = p;
    while (p < end && *p != '/' && *p != ':') p++;
    js_usize host_len = (js_usize)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return 0;
    fetch_memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    /* optional :port */
    if (p < end && *p == ':') {
        p++; /* skip ':' */
        unsigned int port_acc = 0;
        int digits = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            port_acc = port_acc * 10 + (unsigned int)(*p - '0');
            p++;
            digits++;
        }
        if (!digits || port_acc == 0 || port_acc > 65535) return 0;
        out->port = (unsigned short)port_acc;
    }

    /* path: everything from here to end (default "/") */
    if (p >= end || *p != '/') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        js_usize path_len = (js_usize)(end - p);
        if (path_len >= sizeof(out->path)) path_len = sizeof(out->path) - 1;
        fetch_memcpy(out->path, p, path_len);
        out->path[path_len] = '\0';
    }

    return 1; /* success */
}

/* ================================================================== */
/*  Response object state (native wrapper)                            */
/* ================================================================== */

/* Maximum number of simultaneous Response objects alive in one JS eval.
 * (Practically 1-2 per eval turn; bump if needed.) */
#define FETCH_RESP_POOL  8

typedef struct {
    int   status;           /* HTTP status code, e.g. 200               */
    int   ok;               /* 1 if 200-299                              */
    char *body;             /* pointer into g_resp_buf (or NULL)         */
    long  body_len;         /* bytes in body (may be 0)                  */
    int   in_use;
} fetch_resp_t;

static fetch_resp_t g_resp_pool[FETCH_RESP_POOL];

static fetch_resp_t *resp_alloc(void)
{
    for (int i = 0; i < FETCH_RESP_POOL; i++) {
        if (!g_resp_pool[i].in_use) {
            fetch_memset(&g_resp_pool[i], 0, sizeof(fetch_resp_t));
            g_resp_pool[i].in_use = 1;
            return &g_resp_pool[i];
        }
    }
    return (fetch_resp_t *)0; /* pool exhausted */
}

/* ================================================================== */
/*  Response native class                                             */
/* ================================================================== */

static int g_resp_class_id = 0;

/* .text() -- returns the body as a JS string */
static js_value resp_method_text(js_vm *vm, void *self_ptr,
                                 int argc, js_value *argv)
{
    (void)argc; (void)argv;
    fetch_resp_t *r = (fetch_resp_t *)self_ptr;
    if (!r || !r->body || r->body_len <= 0)
        return js_native_make_string(vm, "");
    /* body_len may be large; build from pointer + length */
    js_string *s = js_str_new(vm, r->body, (js_usize)r->body_len);
    if (!s) return js_native_make_string(vm, "");
    js_value v;
    v.type = JS_STRING;
    v.u.s  = s;
    return v;
}

/*
 * .json() -- parse the body as JSON and return the value.
 * We delegate to the engine's JSON.parse by constructing a temporary
 * script string -- but that would require re-entrancy.  Instead we do
 * a direct call via js_eval on a tiny wrapper; however to stay simple
 * and avoid re-entrant arena trouble, we call the builtin JSON parser
 * directly by building a js_value string and calling json_parse
 * indirectly through a js_eval snippet.
 *
 * Simpler approach: store the body in a JS string, then run JSON.parse
 * via the existing js_eval path into a fresh result buffer.
 * We return a string on failure (matching browser .json() throw behaviour
 * is not critical for this OS binding).
 */
static js_value resp_method_json(js_vm *vm, void *self_ptr,
                                 int argc, js_value *argv)
{
    (void)argc; (void)argv;
    fetch_resp_t *r = (fetch_resp_t *)self_ptr;
    if (!r || !r->body || r->body_len <= 0)
        return js_native_make_null();

    /*
     * Build the call: JSON.parse(<body>)
     * We compose a tiny script: "JSON.parse(<body_escaped>)".
     * To avoid shell-quote issues we expose the body as a global
     * variable __fetch_json_src and run "JSON.parse(__fetch_json_src)".
     *
     * LIFECYCLE NOTE: js_eval resets the arena and re-installs builtins,
     * which would wipe the native-class registry.  So we CANNOT call
     * js_eval from inside a native method safely.
     *
     * Alternative: use the internal jp_value JSON parser directly.
     * The parser is static in js_builtin.c so we cannot call it.
     *
     * Practical solution for a freestanding OS: return the raw body
     * as a JS string from .json() and let the caller call JSON.parse()
     * in JS.  Document this limitation.
     */
    return resp_method_text(vm, self_ptr, 0, (js_value *)0);
}

/*
 * .then(callback) -- calls callback(response) synchronously and returns
 * the result.  This makes fetch() behave like a resolved Promise for
 * simple "fetch(url).then(r => r.text())" patterns.
 */
static js_value resp_method_then(js_vm *vm, void *self_ptr,
                                 int argc, js_value *argv)
{
    if (argc < 1) return js_native_make_undefined();

    /* Build a JS value wrapping `self_ptr` again using the same class. */
    js_value self_val = js_native_wrap(vm, g_resp_class_id, self_ptr);
    if (self_val.type == JS_UNDEFINED) return js_native_make_undefined();

    js_value result = js_native_make_undefined();
    js_call_function(vm, argv[0], js_native_make_undefined(),
                     &self_val, 1, &result);
    return result;
}

/* Property getter for Response: status, ok, statusText */
static js_value resp_get(js_vm *vm, void *self_ptr, const char *prop)
{
    fetch_resp_t *r = (fetch_resp_t *)self_ptr;
    if (!r) return js_native_make_undefined();

    js_usize plen = fetch_strlen(prop);
    if (plen == 6 && fetch_memcmp(prop, "status", 6) == 0)
        return js_native_make_number(vm, (double)r->status);
    if (plen == 2 && fetch_memcmp(prop, "ok", 2) == 0)
        return js_native_make_bool(vm, r->ok);
    if (plen == 10 && fetch_memcmp(prop, "statusText", 10) == 0) {
        if (r->status == 200) return js_native_make_string(vm, "OK");
        if (r->status == 404) return js_native_make_string(vm, "Not Found");
        if (r->status == 500) return js_native_make_string(vm, "Internal Server Error");
        return js_native_make_string(vm, "");
    }
    return js_native_make_undefined();
}

static int resp_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    (void)vm; (void)self_ptr; (void)prop; (void)val;
    return 1; /* not handled -- fall through to own-prop store */
}

/*
 * Class method tables declared as js_native_method_entry[] (public typedef,
 * same layout as the anonymous struct in js_native_class.methods per
 * js_native.h).  We cannot use these in a static initializer for .methods
 * because GCC 15 treats anonymous struct tags as unique per definition site.
 * The pointer is patched in at install time via patch_methods_ptr().
 */
static const js_native_method_entry g_resp_methods[] = {
    { "text", resp_method_text },
    { "json", resp_method_json },
    { "then", resp_method_then },
    { (const char *)0, (js_native_method)0 }
};

/* Base class with .methods == NULL; patched at install time. */
static const js_native_class g_resp_class_base = {
    "Response",
    resp_get,
    resp_set,
    (void *)0
};

/* ================================================================== */
/*  Perform an HTTP(S) GET and return a Response native object        */
/* ================================================================== */

/*
 * Static fallback Response used when the pool is exhausted.  Callers
 * always get a valid object they can call .text() / .then() on without
 * crashing; they just see ok=false / status=0 / empty body.
 */
static fetch_resp_t g_fallback_resp;

static js_value make_error_response(js_vm *vm)
{
    /*
     * Try the normal pool first; if full, recycle the static fallback.
     * The fallback is NOT pool-managed (in_use stays 0 permanently so
     * it doesn't block normal pool entries).  We write it here and wrap
     * it directly, which is safe because the native object lifetime is
     * bounded to this JS eval turn.
     */
    fetch_resp_t *r = resp_alloc();
    if (!r) {
        /* Pool exhausted -- use the static fallback (non-reentrant). */
        fetch_memset(&g_fallback_resp, 0, sizeof(g_fallback_resp));
        g_fallback_resp.in_use = 0; /* not pool-managed */
        r = &g_fallback_resp;
    }
    r->status   = 0;
    r->ok       = 0;
    r->body     = (char *)0;
    r->body_len = 0;
    return js_native_wrap(vm, g_resp_class_id, r);
}

static js_value do_fetch(js_vm *vm, const char *url_str)
{
    fetch_url_t url;
    if (!fetch_parse_url(url_str, &url)) {
        /* Bad/unsupported URL -- return a Response with ok=false, status=0. */
        return make_error_response(vm);
    }

    /* Shared response buffer -- bounded, not re-entrant.
     * NUL-terminate first byte so that an http_get that writes nothing
     * leaves a valid empty C-string in the buffer. */
    g_resp_buf[0] = '\0';

    int http_status = 0;
    long n_bytes;
    if (url.is_tls) {
        n_bytes = https_get(url.host, url.port, url.path,
                            g_resp_buf, (unsigned long)JS_FETCH_BODY_CAP,
                            &http_status);
    } else {
        n_bytes = http_get(url.host, url.port, url.path,
                           g_resp_buf, (unsigned long)JS_FETCH_BODY_CAP,
                           &http_status);
    }

    /* Negative return means network failure or timeout -- degrade gracefully. */
    if (n_bytes < 0) {
        return make_error_response(vm);
    }

    fetch_resp_t *r = resp_alloc();
    if (!r) return make_error_response(vm);

    /* Clamp to buffer capacity (paranoid: http layer should already do this). */
    if (n_bytes > (long)(JS_FETCH_BODY_CAP - 1))
        n_bytes = (long)(JS_FETCH_BODY_CAP - 1);

    r->status   = http_status;
    r->ok       = (http_status >= 200 && http_status <= 299) ? 1 : 0;
    r->body     = g_resp_buf;
    r->body_len = n_bytes;

    return js_native_wrap(vm, g_resp_class_id, r);
}

/* ================================================================== */
/*  fetch() global function binding                                   */
/* ================================================================== */
static js_value fn_fetch(js_vm *vm, int argc, js_value *argv)
{
    /* Missing / null / undefined URL: return error Response, not undefined.
     * This lets page scripts use fetch().then(r => ...) without a crash even
     * when called with no arguments. */
    if (argc < 1 || js_native_is_null_or_undefined(argv[0]))
        return make_error_response(vm);

    const char *url_str = js_native_to_cstr(vm, argv[0]);
    if (!url_str) return make_error_response(vm);

    return do_fetch(vm, url_str);
}

/* ================================================================== */
/*  XMLHttpRequest native class                                       */
/* ================================================================== */

#define XHR_POOL_SIZE  4
#define XHR_URL_CAP    2048
#define XHR_METHOD_CAP 16

typedef struct {
    char   method[XHR_METHOD_CAP];     /* e.g. "GET"            */
    char   url[XHR_URL_CAP];           /* full URL string       */
    int    status;                     /* HTTP status after send */
    long   response_len;               /* body byte count        */
    char  *response_text;              /* points into g_resp_buf */
    int    ready_state;                /* 0=UNSENT 1=OPENED 4=DONE */
    int    in_use;
    /* onload callback -- stored as a JS value (function or undefined) */
    js_value onload_fn;
    js_vm   *vm;                       /* back-pointer for onload call */
} xhr_t;

static xhr_t g_xhr_pool[XHR_POOL_SIZE];
static int   g_xhr_class_id = 0;

static xhr_t *xhr_alloc(void)
{
    for (int i = 0; i < XHR_POOL_SIZE; i++) {
        if (!g_xhr_pool[i].in_use) {
            fetch_memset(&g_xhr_pool[i], 0, sizeof(xhr_t));
            g_xhr_pool[i].in_use      = 1;
            g_xhr_pool[i].ready_state = 0; /* UNSENT */
            g_xhr_pool[i].onload_fn   = js_native_make_undefined();
            return &g_xhr_pool[i];
        }
    }
    return (xhr_t *)0;
}

/* xhr.open(method, url) */
static js_value xhr_method_open(js_vm *vm, void *self_ptr,
                                int argc, js_value *argv)
{
    xhr_t *x = (xhr_t *)self_ptr;
    if (!x) return js_native_make_undefined();

    const char *method = (argc >= 1) ? js_native_to_cstr(vm, argv[0]) : "GET";
    const char *url    = (argc >= 2) ? js_native_to_cstr(vm, argv[1]) : "";

    if (method) fetch_strncpy(x->method, method, XHR_METHOD_CAP);
    else        fetch_strncpy(x->method, "GET",  XHR_METHOD_CAP);

    if (url)   fetch_strncpy(x->url, url, XHR_URL_CAP);
    else       x->url[0] = '\0';

    x->ready_state = 1; /* OPENED */
    x->vm = vm;
    return js_native_make_undefined();
}

/* xhr.send() -- blocking, populates status/responseText, fires onload */
static js_value xhr_method_send(js_vm *vm, void *self_ptr,
                                int argc, js_value *argv)
{
    (void)argc; (void)argv;
    xhr_t *x = (xhr_t *)self_ptr;
    if (!x || x->url[0] == '\0') return js_native_make_undefined();

    fetch_url_t url;
    if (!fetch_parse_url(x->url, &url)) {
        x->status        = 0;
        x->response_len  = 0;
        x->response_text = (char *)0;
        x->ready_state   = 4; /* DONE */
        return js_native_make_undefined();
    }

    fetch_memset(g_resp_buf, 0, 1);

    int http_status = 0;
    long n_bytes;
    if (url.is_tls) {
        n_bytes = https_get(url.host, url.port, url.path,
                            g_resp_buf, (unsigned long)JS_FETCH_BODY_CAP,
                            &http_status);
    } else {
        n_bytes = http_get(url.host, url.port, url.path,
                           g_resp_buf, (unsigned long)JS_FETCH_BODY_CAP,
                           &http_status);
    }

    if (n_bytes < 0) {
        x->status        = 0;
        x->response_len  = 0;
        x->response_text = (char *)0;
    } else {
        x->status        = http_status;
        x->response_len  = n_bytes;
        x->response_text = g_resp_buf;
    }
    x->ready_state = 4; /* DONE */

    /* Fire onload synchronously if set */
    if (x->onload_fn.type == JS_FUNCTION) {
        js_value self_val = js_native_wrap(vm, g_xhr_class_id, x);
        js_value result   = js_native_make_undefined();
        js_call_function(vm, x->onload_fn, self_val,
                         (js_value *)0, 0, &result);
    }

    return js_native_make_undefined();
}

/* xhr.abort() -- no-op for sync XHR */
static js_value xhr_method_abort(js_vm *vm, void *self_ptr,
                                 int argc, js_value *argv)
{
    (void)vm; (void)self_ptr; (void)argc; (void)argv;
    return js_native_make_undefined();
}

/* Property getter */
static js_value xhr_get(js_vm *vm, void *self_ptr, const char *prop)
{
    xhr_t *x = (xhr_t *)self_ptr;
    if (!x) return js_native_make_undefined();

    js_usize plen = fetch_strlen(prop);

    if (plen == 6 && fetch_memcmp(prop, "status", 6) == 0)
        return js_native_make_number(vm, (double)x->status);

    if (plen == 10 && fetch_memcmp(prop, "readyState", 10) == 0)
        return js_native_make_number(vm, (double)x->ready_state);

    if (plen == 12 && fetch_memcmp(prop, "responseText", 12) == 0) {
        if (!x->response_text || x->response_len <= 0)
            return js_native_make_string(vm, "");
        js_string *s = js_str_new(vm, x->response_text,
                                  (js_usize)x->response_len);
        if (!s) return js_native_make_string(vm, "");
        js_value v;
        v.type = JS_STRING;
        v.u.s  = s;
        return v;
    }

    if (plen == 8 && fetch_memcmp(prop, "response", 8) == 0) {
        /* .response == .responseText for text/plain XHR */
        return xhr_get(vm, self_ptr, "responseText");
    }

    if (plen == 6 && fetch_memcmp(prop, "onload", 6) == 0)
        return x->onload_fn;

    if (plen == 7 && fetch_memcmp(prop, "onerror", 7) == 0)
        return js_native_make_undefined(); /* not wired */

    return js_native_make_undefined();
}

/* Property setter */
static int xhr_set(js_vm *vm, void *self_ptr,
                   const char *prop, js_value val)
{
    (void)vm;
    xhr_t *x = (xhr_t *)self_ptr;
    if (!x) return 1;

    js_usize plen = fetch_strlen(prop);
    if (plen == 6 && fetch_memcmp(prop, "onload", 6) == 0) {
        x->onload_fn = val;
        return 0; /* handled */
    }
    if (plen == 7 && fetch_memcmp(prop, "onerror", 7) == 0) {
        /* accept but ignore */
        return 0;
    }
    return 1; /* not handled */
}

static const js_native_method_entry g_xhr_methods[] = {
    { "open",  xhr_method_open  },
    { "send",  xhr_method_send  },
    { "abort", xhr_method_abort },
    { (const char *)0, (js_native_method)0 }
};

static const js_native_class g_xhr_class_base = {
    "XMLHttpRequest",
    xhr_get,
    xhr_set,
    (void *)0
};

/* ================================================================== */
/*  XMLHttpRequest constructor global function                        */
/* ================================================================== */
static js_value fn_xhr_constructor(js_vm *vm, int argc, js_value *argv)
{
    (void)argc; (void)argv;
    xhr_t *x = xhr_alloc();
    if (!x) return js_native_make_null();
    x->vm = vm;
    return js_native_wrap(vm, g_xhr_class_id, x);
}

/* ================================================================== */
/*  Installation                                                      */
/* ================================================================== */

/*
 * The js_native_class.methods field is typed as a pointer to an inline
 * anonymous struct { const char *name; js_native_method fn; }.  Any attempt
 * to assign our js_native_method_entry* (a named typedef) to it produces a
 * GCC -Wincompatible-pointer-types error even with identical layout.
 *
 * Resolution: build a mutable (non-const) local copy of each class
 * descriptor, then write the pointer value directly into the .methods
 * member by aliasing through a void** -- the same size as any other pointer
 * on LP64 x86_64.  js_native.c itself casts cls->methods back to its own
 * internal type on every read, so the layout contract is preserved.
 */

/* Helper: writes `ptr` into the void* at address `field_ptr`. */
static void patch_methods_ptr(void *field_ptr, const void *ptr)
{
    /* Both sides are pointer-sized. Copy via unsigned char to avoid
     * strict-aliasing issues. */
    const unsigned char *src = (const unsigned char *)&ptr;
    unsigned char       *dst = (unsigned char *)field_ptr;
    for (js_usize i = 0; i < sizeof(void *); i++) dst[i] = src[i];
}

void js_fetch_install(js_vm *vm)
{
    /* Build mutable local descriptors (methods starts NULL). */
    js_native_class resp_cls;
    fetch_memcpy(&resp_cls, &g_resp_class_base, sizeof(js_native_class));

    js_native_class xhr_cls;
    fetch_memcpy(&xhr_cls, &g_xhr_class_base, sizeof(js_native_class));

    /* Patch .methods by writing the pointer value through void**. */
    patch_methods_ptr(&resp_cls.methods, (const void *)g_resp_methods);
    patch_methods_ptr(&xhr_cls.methods,  (const void *)g_xhr_methods);

    g_resp_class_id = js_native_register_class(vm, &resp_cls);
    g_xhr_class_id  = js_native_register_class(vm, &xhr_cls);

    /* fetch() global function */
    js_native_register_function(vm, "fetch", fn_fetch);

    /* XMLHttpRequest global constructor function */
    js_native_register_function(vm, "XMLHttpRequest", fn_xhr_constructor);
}

/* ================================================================== */
/*  Self-test (offline -- no live network required)                   */
/* ================================================================== */

/* Tiny result accumulator -- prints nothing, just counts failures */
static int g_test_fails = 0;

#define SELFTEST_ASSERT(cond) \
    do { if (!(cond)) { g_test_fails++; } } while (0)

int js_fetch_selftest(void)
{
    g_test_fails = 0;

    /* --- 1. URL parser: valid http ----------------------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("http://example.com/index.html", &u);
        SELFTEST_ASSERT(ok == 1);
        SELFTEST_ASSERT(u.is_tls == 0);
        SELFTEST_ASSERT(u.port   == 80);
        SELFTEST_ASSERT(fetch_memcmp(u.host, "example.com", 11) == 0);
        SELFTEST_ASSERT(fetch_memcmp(u.path, "/index.html", 11) == 0);
    }

    /* --- 2. URL parser: https with custom port ----------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("https://api.example.com:8443/v1/data", &u);
        SELFTEST_ASSERT(ok == 1);
        SELFTEST_ASSERT(u.is_tls == 1);
        SELFTEST_ASSERT(u.port   == 8443);
        SELFTEST_ASSERT(fetch_memcmp(u.host, "api.example.com", 15) == 0);
        SELFTEST_ASSERT(fetch_memcmp(u.path, "/v1/data", 8) == 0);
    }

    /* --- 3. URL parser: default port https --------------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("https://secure.example.com/", &u);
        SELFTEST_ASSERT(ok == 1);
        SELFTEST_ASSERT(u.is_tls == 1);
        SELFTEST_ASSERT(u.port   == 443);
    }

    /* --- 4. URL parser: bare host (no path) inserts "/" -------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("http://host.local", &u);
        SELFTEST_ASSERT(ok == 1);
        SELFTEST_ASSERT(u.path[0] == '/');
    }

    /* --- 5. URL parser: missing scheme fails ------------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("ftp://example.com/", &u);
        SELFTEST_ASSERT(ok == 0);
    }

    /* --- 6. URL parser: empty string fails -------------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("", &u);
        SELFTEST_ASSERT(ok == 0);
    }

    /* --- 7. URL parser: dotted-quad host works ---------------------- */
    {
        fetch_url_t u;
        int ok = fetch_parse_url("http://10.0.2.2:8080/api", &u);
        SELFTEST_ASSERT(ok == 1);
        SELFTEST_ASSERT(u.port == 8080);
        SELFTEST_ASSERT(fetch_memcmp(u.host, "10.0.2.2", 8) == 0);
        SELFTEST_ASSERT(fetch_memcmp(u.path, "/api", 4) == 0);
    }

    /* --- 8. Response pool allocation -------------------------------- */
    {
        fetch_resp_t *r = resp_alloc();
        SELFTEST_ASSERT(r != (fetch_resp_t *)0);
        SELFTEST_ASSERT(r->in_use == 1);
        r->in_use = 0; /* release */
    }

    /* --- 9. XHR pool allocation ------------------------------------- */
    {
        xhr_t *x = xhr_alloc();
        SELFTEST_ASSERT(x != (xhr_t *)0);
        SELFTEST_ASSERT(x->in_use == 1);
        SELFTEST_ASSERT(x->ready_state == 0);
        x->in_use = 0; /* release */
    }

    /* --- 10. fetch_install plumbing: class IDs assigned correctly ----- */
    {
        js_vm *vm = js_new();
        if (vm) {
            js_fetch_install(vm);
            /* class IDs must be positive and distinct */
            SELFTEST_ASSERT(g_resp_class_id > 0);
            SELFTEST_ASSERT(g_xhr_class_id  > 0);
            SELFTEST_ASSERT(g_xhr_class_id  != g_resp_class_id);

            /*
             * NOTE: js_eval() resets the VM arena and the native-class
             * registry; it ALSO re-runs js_install_builtins() before
             * the script.  That means any js_native_register_class /
             * js_native_register_function calls made before js_eval are
             * wiped out by the time the script runs.  Embedders must
             * either (a) call js_fetch_install() from within a "pre-eval"
             * callback that the engine provides (not yet implemented), or
             * (b) use js_eval() exclusively BEFORE installing native
             * classes, then make sure the script is self-contained.
             *
             * For this offline selftest we verify the API presence by
             * looking up the 'fetch' global directly in the VM environment
             * (bypassing js_eval) using js_env_get(), which is valid as
             * long as we do NOT call js_eval afterwards.
             */
            js_string *fetch_name = js_str_intern(vm, "fetch", 5);
            js_value fetch_v = js_mk_undef();
            int found = js_env_get(vm->global_env, fetch_name, &fetch_v);
            SELFTEST_ASSERT(found == 1);
            SELFTEST_ASSERT(fetch_v.type == JS_FUNCTION);

            js_string *xhr_name = js_str_intern(vm, "XMLHttpRequest", 14);
            js_value xhr_v = js_mk_undef();
            found = js_env_get(vm->global_env, xhr_name, &xhr_v);
            SELFTEST_ASSERT(found == 1);
            SELFTEST_ASSERT(xhr_v.type == JS_FUNCTION);
        }
    }

    /* --- 11. Response stub object via wrap -------------------------- */
    {
        js_vm *vm = js_new();
        if (vm) {
            js_fetch_install(vm);
            fetch_resp_t r;
            fetch_memset(&r, 0, sizeof(r));
            r.in_use    = 1;
            r.status    = 200;
            r.ok        = 1;

            static const char body_data[] = "hello";
            r.body      = (char *)body_data;
            r.body_len  = 5;

            js_value wrapped = js_native_wrap(vm, g_resp_class_id, &r);
            SELFTEST_ASSERT(wrapped.type == JS_OBJECT || wrapped.type == JS_FUNCTION);
            /* Retrieve .status via the getter */
            js_value status_v = resp_get(vm, &r, "status");
            SELFTEST_ASSERT(status_v.type == JS_NUMBER);
            SELFTEST_ASSERT((int)status_v.u.n == 200);
            /* Retrieve .ok */
            js_value ok_v = resp_get(vm, &r, "ok");
            SELFTEST_ASSERT(ok_v.type == JS_BOOL);
            SELFTEST_ASSERT(ok_v.u.b == 1);
            /* Call .text() */
            js_value text_v = resp_method_text(vm, &r, 0, (js_value *)0);
            SELFTEST_ASSERT(text_v.type == JS_STRING);
            SELFTEST_ASSERT(text_v.u.s->len == 5);
            SELFTEST_ASSERT(fetch_memcmp(text_v.u.s->data, "hello", 5) == 0);
        }
    }

    /* --- 12. XHR state machine (no network) ------------------------- */
    {
        xhr_t x;
        fetch_memset(&x, 0, sizeof(x));
        x.in_use      = 1;
        x.ready_state = 0;
        x.onload_fn   = js_native_make_undefined();

        js_vm *vm = js_new();
        if (vm) {
            js_fetch_install(vm);
            /* open() */
            js_value open_argv[2];
            open_argv[0] = js_native_make_string(vm, "GET");
            open_argv[1] = js_native_make_string(vm, "http://10.0.2.2/");
            xhr_method_open(vm, &x, 2, open_argv);
            SELFTEST_ASSERT(x.ready_state == 1);
            SELFTEST_ASSERT(fetch_memcmp(x.method, "GET", 3) == 0);
            SELFTEST_ASSERT(fetch_memcmp(x.url, "http://10.0.2.2/", 16) == 0);
        }
    }

    return (g_test_fails == 0) ? 0 : -1;
}

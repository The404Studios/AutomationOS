/*
 * js_url.c -- URL, URLSearchParams, encodeURI* / decodeURI* for AutomationOS JS.
 * ================================================================================
 *
 * Freestanding: no libc, no stdio.  Memory comes from:
 *   - The JS engine arena (js_arena_alloc) for values returned from JS functions.
 *   - The userspace heap (malloc/free from libc/malloc.h) for URLSearchParams
 *     and URL structs that must survive arena resets between js_eval() calls.
 *
 * RFC 3986 coverage
 * -----------------
 * HANDLED:
 *   §3    Syntax components: scheme, authority (host + port), path, query,
 *         fragment -- all split by the parser.
 *   §5.2  Reference resolution algorithm (merge + remove_dot_segments).
 *   §5.3  Component recomposition (for .href).
 *   §2.1  Percent-encoding/decoding.
 *   §3.3  Path normalization: remove_dot_segments handles '.' and '..'.
 *
 * NOT HANDLED (known gaps):
 *   §3.2.1  Userinfo (user:password@host) -- userinfo is silently stripped.
 *   §3.2.3  IP-literal IPv6 hosts [::1] -- treated as opaque strings.
 *   IDNA / Punycode hostname normalization.
 *   Relative-reference schemes with //authority-only refs.
 *   Unicode surrogate pair encoding in encodeURIComponent.
 *   Percent-encoded sequences in base-URL authority are passed through.
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -I<js-dir> -I<libc-dir> \
 *       -c userspace/lib/js/js_url.c -o js_url.o
 *
 * Selftest binary (standalone, no full engine needed):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -I<js-dir> -I<libc-dir> -DJS_URL_STANDALONE_TEST \
 *       js_url.c malloc.o string.o crt0.o -o js_url_test
 */

#ifndef JS_URL_STANDALONE_TEST
#  include "js_url.h"
#  include "js_internal.h"    /* js_arena_alloc, js_str_*, js_mk_*, etc. */
/* URLSearchParams and URL structs use heap (survive arena resets). */
#  include "../../libc/malloc.h"
#else
/*
 * Standalone selftest mode: define just enough to compile the C-side
 * parser, encode/decode, and URLSearchParams without the JS engine.
 */
#  ifndef NULL
#    define NULL ((void *)0)
#  endif
typedef unsigned long  js_usize;
typedef long           js_isize;
/* Minimal stubs so we can include just the parts we need */
static void *st_malloc(js_usize n);
static void  st_free(void *p);
#  define malloc  st_malloc
#  define free    st_free
/* Forward the selftest prototype */
int js_url_selftest(void);
#endif /* JS_URL_STANDALONE_TEST */

/* ======================================================================
 * Internal C-string helpers (no libc dependency)
 * ====================================================================== */

static js_usize url_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (js_usize)(p - s);
}

static void url_memcpy(char *d, const char *s, js_usize n)
{
    for (js_usize i = 0; i < n; i++) d[i] = s[i];
}

/* url_memcmp is available for future use; suppress unused-function warning. */
static int url_memcmp(const char *a, const char *b, js_usize n)
    __attribute__((unused));
static int url_memcmp(const char *a, const char *b, js_usize n)
{
    for (js_usize i = 0; i < n; i++) {
        unsigned char ai = (unsigned char)a[i];
        unsigned char bi = (unsigned char)b[i];
        if (ai < bi) return -1;
        if (ai > bi) return  1;
    }
    return 0;
}

static void url_memset_c(char *d, char c, js_usize n)
{
    for (js_usize i = 0; i < n; i++) d[i] = c;
}

static int url_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static char *url_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static void url_strcat(char *dst, const char *src)
{
    while (*dst) dst++;
    while ((*dst++ = *src++));
}

/* Bounded strcat: appends at most `n` bytes from `src` to `dst`, where
 * `dsz` is the total capacity of `dst` (including NUL). */
static void url_strcat_n(char *dst, js_usize dsz, const char *src)
{
    js_usize i = 0;
    while (i < dsz && dst[i]) i++;
    while (i < dsz - 1 && *src) { dst[i++] = *src++; }
    if (i < dsz) dst[i] = '\0';
}

static js_usize url_uitoa(unsigned int v, char *buf)
{
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    char tmp[12]; js_usize n = 0;
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (js_usize i = 0; i < n; i++) buf[i] = tmp[n-1-i];
    buf[n] = '\0';
    return n;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ======================================================================
 * Percent-encoding character predicates
 * ====================================================================== */

/*
 * encodeURIComponent unreserved: A-Z a-z 0-9 - _ . ! ~ * ' ( )
 */
static int is_unreserved_component(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return (c=='-'||c=='_'||c=='.'||c=='!'||c=='~'||c=='*'||c=='\''||c=='('||c==')');
}

/*
 * encodeURI unreserved: unreserved_component PLUS :/?#[]@!$&'()*+,;=
 */
static int is_unreserved_uri(unsigned char c)
{
    if (is_unreserved_component(c)) return 1;
    return (c==':'||c=='/'||c=='?'||c=='#'||c=='['||c==']'||c=='@'||
            c=='!'||c=='$'||c=='&'||c=='\''||c=='('||c==')'||c=='*'||
            c=='+'||c==','||c==';'||c=='=');
}

static const char hex_chars[] = "0123456789ABCDEF";

/* ======================================================================
 * Static-buffer encode / decode (used by selftest and JS wrappers)
 * Both write into caller-supplied `out` buffers.
 *
 * pct_encode_buf: encodes `in` (len bytes) into `out` (outsz bytes).
 *   Returns 0 on success, -1 if out was too small.
 * pct_decode_buf: decodes `in` into `out`.
 *   `reserved_passthrough`: if non-NULL, keep %XX encoded when decoded
 *   byte passes this predicate (for decodeURI semantics).
 *   Returns 0 ok, -1 bad %XX, -2 buffer too small.
 * ====================================================================== */

static int pct_encode_buf(const char *in, js_usize len,
                          int (*keep)(unsigned char),
                          char *out, js_usize outsz)
{
    js_usize pos = 0;
    for (js_usize i = 0; i < len; i++) {
        unsigned char b = (unsigned char)in[i];
        if (keep(b)) {
            if (pos + 1 >= outsz) return -2;
            out[pos++] = (char)b;
        } else {
            if (pos + 3 >= outsz) return -2;
            out[pos++] = '%';
            out[pos++] = hex_chars[(b >> 4) & 0xF];
            out[pos++] = hex_chars[b & 0xF];
        }
    }
    out[pos] = '\0';
    return 0;
}

static int pct_decode_buf(const char *in, js_usize len,
                          int (*reserved_passthrough)(unsigned char),
                          char *out, js_usize outsz)
{
    js_usize pos = 0;
    for (js_usize i = 0; i < len; ) {
        if (in[i] == '%') {
            if (i + 2 >= len) return -1;  /* truncated */
            int hi = hex_val(in[i+1]);
            int lo = hex_val(in[i+2]);
            if (hi < 0 || lo < 0) return -1;
            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            if (reserved_passthrough && reserved_passthrough(decoded)) {
                /* keep encoded form */
                if (pos + 3 >= outsz) return -2;
                out[pos++] = in[i];
                out[pos++] = in[i+1];
                out[pos++] = in[i+2];
            } else {
                if (pos + 1 >= outsz) return -2;
                out[pos++] = (char)decoded;
            }
            i += 3;
        } else {
            if (pos + 1 >= outsz) return -2;
            out[pos++] = in[i++];
        }
    }
    out[pos] = '\0';
    return 0;
}

/* ======================================================================
 * Arena-based encode/decode for JS function returns
 * (Only compiled when NOT in standalone test mode)
 * ====================================================================== */

#ifndef JS_URL_STANDALONE_TEST
static const char *pct_encode_arena(js_vm *vm, const char *in, js_usize len,
                                    int (*keep)(unsigned char))
{
    js_usize cap = len * 3 + 1;
    char *out = (char *)js_arena_alloc(vm, cap);
    if (!out) return "";
    pct_encode_buf(in, len, keep, out, cap);
    return out;
}

static const char *pct_decode_arena(js_vm *vm, const char *in, js_usize len,
                                    int (*reserved)(unsigned char), int *err)
{
    char *out = (char *)js_arena_alloc(vm, len + 1);
    if (!out) { if (err) *err = 1; return ""; }
    int rc = pct_decode_buf(in, len, reserved, out, len + 1);
    if (rc < 0) { if (err) *err = 1; return NULL; }
    if (err) *err = 0;
    return out;
}
#endif /* !JS_URL_STANDALONE_TEST */

/* ======================================================================
 * URL struct  (NUL-terminated fixed string fields)
 * ====================================================================== */

#define URL_BUF 512

typedef struct {
    char scheme[64];
    char host[256];
    char port[8];
    char pathname[URL_BUF];
    char search[URL_BUF];
    char hash[URL_BUF];
} url_parsed;

static void url_clear(url_parsed *u)
{
    url_memset_c(u->scheme,   '\0', sizeof(u->scheme));
    url_memset_c(u->host,     '\0', sizeof(u->host));
    url_memset_c(u->port,     '\0', sizeof(u->port));
    url_memset_c(u->pathname, '\0', sizeof(u->pathname));
    url_memset_c(u->search,   '\0', sizeof(u->search));
    url_memset_c(u->hash,     '\0', sizeof(u->hash));
}

static js_usize buf_copy(char *dst, js_usize dsz, const char *src, js_usize len)
{
    if (dsz == 0) return 0;
    js_usize n = (len < dsz - 1) ? len : dsz - 1;
    url_memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

/* ======================================================================
 * RFC 3986 §5.2.4 -- remove_dot_segments (in-place on NUL-term path)
 * ====================================================================== */

static void remove_dot_segments(char *path)
{
    js_usize plen = url_strlen(path);
    char out[URL_BUF];
    js_usize oi = 0, i = 0;

    while (i < plen) {
        /* A: strip leading "./" or "../" */
        if (path[i]=='.' && path[i+1]=='/') { i+=2; continue; }
        if (path[i]=='.' && path[i+1]=='.' && path[i+2]=='/') { i+=3; continue; }
        /* B: "/." -> "/" */
        if (path[i]=='/' && path[i+1]=='.' &&
            (path[i+2]=='/' || path[i+2]=='\0')) {
            i += 2;
            if (path[i] == '\0') { if (oi < URL_BUF-1) out[oi++]='/'; break; }
            continue;
        }
        /* C: "/.." -> pop last segment (RFC 3986 §5.2.4 rule C)
         *
         * We need to remove everything from the last '/' to the end of
         * the output buffer, i.e. the rightmost segment including its
         * leading slash.  Example: out="/a/b", after pop -> out="/a".
         *
         * Algorithm: scan back past non-slash chars, then past the slash
         * itself.  Leave the cursor after any earlier slash (i.e. don't
         * remove the root slash of an absolute path unnecessarily). */
        if (path[i]=='/' && path[i+1]=='.' && path[i+2]=='.' &&
            (path[i+3]=='/' || path[i+3]=='\0')) {
            i += 3;
            /* Step back past trailing non-slash chars */
            while (oi > 0 && out[oi-1] != '/') oi--;
            /* Step back past the slash itself (but keep any earlier slash) */
            if (oi > 0) oi--;
            if (path[i] == '\0') { if (oi < URL_BUF-1) out[oi++]='/'; break; }
            continue;
        }
        /* D: lone "." or ".." at end */
        if (path[i]=='.' && path[i+1]=='\0') { i++; break; }
        if (path[i]=='.' && path[i+1]=='.' && path[i+2]=='\0') { i+=2; break; }
        /* E: emit up to and including next '/' */
        do {
            if (oi < URL_BUF - 1) out[oi++] = path[i];
            i++;
        } while (i < plen && path[i] != '/');
    }
    out[oi] = '\0';
    url_memcpy(path, out, oi + 1);
}

/* ======================================================================
 * Core URL parser
 * ====================================================================== */

static int url_parse(url_parsed *u, const char *href, js_usize len)
{
    url_clear(u);
    if (len == 0) return -1;

    const char *p   = href;
    const char *end = href + len;

    /* ---- scheme: alpha [alnum+.-]* ':' ---- */
    const char *colon = NULL;
    {
        const char *t = p;
        if ((*t>='A'&&*t<='Z') || (*t>='a'&&*t<='z')) {
            t++;
            while (t < end && ((*t>='A'&&*t<='Z')||(*t>='a'&&*t<='z')||
                                (*t>='0'&&*t<='9')||*t=='+'||*t=='-'||*t=='.'))
                t++;
            if (t < end && *t == ':') colon = t;
        }
    }

    if (colon) {
        js_usize slen = (js_usize)(colon - p);
        if (slen >= sizeof(u->scheme)) slen = sizeof(u->scheme) - 1;
        for (js_usize i = 0; i < slen; i++) {
            char c = p[i];
            if (c>='A' && c<='Z') c += 32;
            u->scheme[i] = c;
        }
        u->scheme[slen] = '\0';
        p = colon + 1;
    }

    /* ---- authority: //host[:port] ---- */
    if (p + 1 < end && p[0]=='/' && p[1]=='/') {
        p += 2;
        const char *auth_end = p;
        while (auth_end < end && *auth_end!='/' && *auth_end!='?' && *auth_end!='#')
            auth_end++;

        /* strip userinfo: anything before last '@' */
        const char *auth_start = p;
        for (const char *t = p; t < auth_end; t++)
            if (*t == '@') auth_start = t + 1;

        /* split host:port -- last ':' not inside IPv6 brackets */
        int in_bracket = 0;
        const char *port_colon = NULL;
        for (const char *t = auth_start; t < auth_end; t++) {
            if (*t=='[') in_bracket=1;
            if (*t==']') in_bracket=0;
            if (*t==':' && !in_bracket) port_colon=t;
        }
        if (port_colon) {
            buf_copy(u->host, sizeof(u->host), auth_start,
                     (js_usize)(port_colon - auth_start));
            js_usize portlen = (js_usize)(auth_end - port_colon - 1);
            if (portlen > 0)
                buf_copy(u->port, sizeof(u->port), port_colon+1, portlen);
        } else {
            buf_copy(u->host, sizeof(u->host), auth_start,
                     (js_usize)(auth_end - auth_start));
        }
        p = auth_end;
    }

    /* ---- path ---- */
    {
        const char *path_end = p;
        while (path_end < end && *path_end!='?' && *path_end!='#')
            path_end++;
        js_usize plen = (js_usize)(path_end - p);
        if (plen == 0) {
            if (u->host[0]) { u->pathname[0]='/'; u->pathname[1]='\0'; }
        } else {
            buf_copy(u->pathname, sizeof(u->pathname), p, plen);
        }
        p = path_end;
    }

    /* ---- query ---- */
    if (p < end && *p == '?') {
        const char *q_end = p;
        while (q_end < end && *q_end != '#') q_end++;
        buf_copy(u->search, sizeof(u->search), p, (js_usize)(q_end - p));
        p = q_end;
    }

    /* ---- fragment ---- */
    if (p < end && *p == '#')
        buf_copy(u->hash, sizeof(u->hash), p, (js_usize)(end - p));

    return 0;
}

/* ======================================================================
 * RFC 3986 §5.2 -- reference resolution
 * ====================================================================== */

static void url_resolve(url_parsed *out, const url_parsed *base,
                        const url_parsed *ref)
{
    url_clear(out);

    if (ref->scheme[0]) {
        /* ref is absolute */
        *out = *ref;
        remove_dot_segments(out->pathname);
        return;
    }

    url_strcpy(out->scheme, base->scheme);

    if (ref->host[0]) {
        url_strcpy(out->host, ref->host);
        url_strcpy(out->port, ref->port);
        buf_copy(out->pathname, sizeof(out->pathname),
                 ref->pathname, url_strlen(ref->pathname));
        remove_dot_segments(out->pathname);
        url_strcpy(out->search, ref->search);
    } else {
        url_strcpy(out->host, base->host);
        url_strcpy(out->port, base->port);

        if (ref->pathname[0] == '\0') {
            url_strcpy(out->pathname, base->pathname);
            url_strcpy(out->search, ref->search[0] ? ref->search : base->search);
        } else if (ref->pathname[0] == '/') {
            buf_copy(out->pathname, sizeof(out->pathname),
                     ref->pathname, url_strlen(ref->pathname));
            remove_dot_segments(out->pathname);
            url_strcpy(out->search, ref->search);
        } else {
            /* merge: replace everything after last '/' in base path */
            js_usize blen = url_strlen(base->pathname);
            js_usize last_slash = 0;
            for (js_usize k = 0; k < blen; k++)
                if (base->pathname[k]=='/') last_slash = k + 1;
            buf_copy(out->pathname, sizeof(out->pathname),
                     base->pathname, last_slash);
            js_usize already = url_strlen(out->pathname);
            js_usize rlen = url_strlen(ref->pathname);
            if (already + rlen < sizeof(out->pathname) - 1) {
                url_memcpy(out->pathname + already, ref->pathname, rlen);
                out->pathname[already + rlen] = '\0';
            }
            remove_dot_segments(out->pathname);
            url_strcpy(out->search, ref->search);
        }
    }
    url_strcpy(out->hash, ref->hash);
}

/* ======================================================================
 * Recompose href (static buffer version used by selftest and arena version)
 * ====================================================================== */

/* Write href into caller-supplied buf (len bytes).  Returns 0. */
static int url_href_buf(const url_parsed *u, char *buf, js_usize bufsz)
{
    if (bufsz == 0) return 0;
    buf[0] = '\0';
    if (u->scheme[0]) { url_strcat_n(buf, bufsz, u->scheme); url_strcat_n(buf, bufsz, "://"); }
    if (u->host[0]) {
        url_strcat_n(buf, bufsz, u->host);
        if (u->port[0]) { url_strcat_n(buf, bufsz, ":"); url_strcat_n(buf, bufsz, u->port); }
    }
    url_strcat_n(buf, bufsz, u->pathname[0] ? u->pathname : "/");
    if (u->search[0]) url_strcat_n(buf, bufsz, u->search);
    if (u->hash[0])   url_strcat_n(buf, bufsz, u->hash);
    return 0;
}

/* ======================================================================
 * The following section is only compiled when NOT in standalone-test mode
 * (i.e. when the full JS engine headers are available).
 * ====================================================================== */

#ifndef JS_URL_STANDALONE_TEST

/* ---- Arena-based href / origin ---- */

static const char *url_href_arena(js_vm *vm, const url_parsed *u)
{
    js_usize cap = 64+3+256+1+8+URL_BUF*3+1;
    char *buf = (char *)js_arena_alloc(vm, cap);
    if (!buf) return "";
    url_href_buf(u, buf, cap);
    return buf;
}

static const char *url_origin_arena(js_vm *vm, const url_parsed *u)
{
    const js_usize cap = 64+3+256+1+8+1;
    char *buf = (char *)js_arena_alloc(vm, cap);
    if (!buf) return "";
    buf[0] = '\0';
    if (!u->scheme[0]) { url_strcpy(buf, "null"); return buf; }
    url_strcat_n(buf, cap, u->scheme);
    url_strcat_n(buf, cap, "://");
    url_strcat_n(buf, cap, u->host);
    if (u->port[0]) { url_strcat_n(buf, cap, ":"); url_strcat_n(buf, cap, u->port); }
    return buf;
}

/* ======================================================================
 * JS native wrappers for encode/decode URI functions
 * ====================================================================== */

static js_value fn_encodeURIComponent(js_vm *vm, int argc, js_value *argv)
{
    js_value s = argc > 0 ? argv[0] : js_native_make_undefined();
    const char *cstr = js_native_to_cstr(vm, s);
    if (!cstr) return js_native_make_string(vm, "");
    const char *enc = pct_encode_arena(vm, cstr, url_strlen(cstr),
                                       is_unreserved_component);
    return js_native_make_string(vm, enc);
}

static js_value fn_decodeURIComponent(js_vm *vm, int argc, js_value *argv)
{
    js_value s = argc > 0 ? argv[0] : js_native_make_undefined();
    const char *cstr = js_native_to_cstr(vm, s);
    if (!cstr) return js_native_make_string(vm, "");
    int err = 0;
    const char *dec = pct_decode_arena(vm, cstr, url_strlen(cstr), NULL, &err);
    if (err || !dec) {
        js_throw_str(vm, "URIError: malformed %XX sequence");
        return js_native_make_undefined();
    }
    return js_native_make_string(vm, dec);
}

static js_value fn_encodeURI(js_vm *vm, int argc, js_value *argv)
{
    js_value s = argc > 0 ? argv[0] : js_native_make_undefined();
    const char *cstr = js_native_to_cstr(vm, s);
    if (!cstr) return js_native_make_string(vm, "");
    const char *enc = pct_encode_arena(vm, cstr, url_strlen(cstr),
                                       is_unreserved_uri);
    return js_native_make_string(vm, enc);
}

static js_value fn_decodeURI(js_vm *vm, int argc, js_value *argv)
{
    js_value s = argc > 0 ? argv[0] : js_native_make_undefined();
    const char *cstr = js_native_to_cstr(vm, s);
    if (!cstr) return js_native_make_string(vm, "");
    int err = 0;
    const char *dec = pct_decode_arena(vm, cstr, url_strlen(cstr),
                                       is_unreserved_uri, &err);
    if (err || !dec) {
        js_throw_str(vm, "URIError: malformed %XX sequence");
        return js_native_make_undefined();
    }
    return js_native_make_string(vm, dec);
}

/* ======================================================================
 * URLSearchParams -- heap-allocated, survives arena resets
 * ====================================================================== */

#define USP_MAX_ENTRIES 64

typedef struct { char *key; char *val; } usp_entry;

typedef struct {
    usp_entry entries[USP_MAX_ENTRIES];
    int       nentries;
} usp_t;

static char *usp_strdup(const char *s)
{
    if (!s) s = "";
    js_usize len = url_strlen(s);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    url_memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static usp_t *usp_new(void)
{
    usp_t *u = (usp_t *)malloc(sizeof(usp_t));
    if (!u) return NULL;
    u->nentries = 0;
    return u;
}

static void usp_free(usp_t *u)
{
    if (!u) return;
    for (int i = 0; i < u->nentries; i++) {
        free(u->entries[i].key);
        free(u->entries[i].val);
    }
    free(u);
}

static void usp_decode_kv(const char *p, js_usize len,
                           char *kbuf, js_usize ksz,
                           char *vbuf, js_usize vsz)
{
    /* find '=' */
    js_usize eq = 0;
    while (eq < len && p[eq] != '=') eq++;

    /* decode key */
    js_usize kn = 0;
    for (js_usize k = 0; k < eq && kn < ksz-1; ) {
        if (p[k]=='+') { kbuf[kn++]=' '; k++; continue; }
        if (p[k]=='%' && k+2 < eq) {
            int hi=hex_val(p[k+1]), lo=hex_val(p[k+2]);
            if (hi>=0&&lo>=0) { kbuf[kn++]=(char)((hi<<4)|lo); k+=3; continue; }
        }
        kbuf[kn++]=p[k++];
    }
    kbuf[kn]='\0';

    /* decode val */
    js_usize vn = 0;
    js_usize vs = (eq < len ? eq+1 : len);
    for (js_usize k = vs; k < len && vn < vsz-1; ) {
        if (p[k]=='+') { vbuf[vn++]=' '; k++; continue; }
        if (p[k]=='%' && k+2 < len) {
            int hi=hex_val(p[k+1]), lo=hex_val(p[k+2]);
            if (hi>=0&&lo>=0) { vbuf[vn++]=(char)((hi<<4)|lo); k+=3; continue; }
        }
        vbuf[vn++]=p[k++];
    }
    vbuf[vn]='\0';
}

static void usp_parse(usp_t *u, const char *init)
{
    if (!init || !init[0]) return;
    if (init[0] == '?') init++;
    js_usize len = url_strlen(init);
    js_usize i = 0;
    while (i <= len && u->nentries < USP_MAX_ENTRIES) {
        js_usize j = i;
        while (j < len && init[j] != '&') j++;
        if (j > i) {
            char kbuf[256], vbuf[512];
            usp_decode_kv(init+i, j-i, kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));
            u->entries[u->nentries].key = usp_strdup(kbuf);
            u->entries[u->nentries].val = usp_strdup(vbuf);
            if (u->entries[u->nentries].key && u->entries[u->nentries].val)
                u->nentries++;
        }
        i = j + 1;
    }
}

static char *usp_to_string(usp_t *u)
{
    js_usize cap = 0;
    for (int i = 0; i < u->nentries; i++)
        cap += (url_strlen(u->entries[i].key)+url_strlen(u->entries[i].val))*3+2;
    cap += 1;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    js_usize pos = 0;
    for (int i = 0; i < u->nentries; i++) {
        if (i) buf[pos++]='&';
        const char *k = u->entries[i].key;
        while (*k) {
            unsigned char c=(unsigned char)*k++;
            if (c==' '){buf[pos++]='+';continue;}
            if (is_unreserved_component(c)){buf[pos++]=(char)c;continue;}
            buf[pos++]='%'; buf[pos++]=hex_chars[(c>>4)&0xF]; buf[pos++]=hex_chars[c&0xF];
        }
        buf[pos++]='=';
        const char *v = u->entries[i].val;
        while (*v) {
            unsigned char c=(unsigned char)*v++;
            if (c==' '){buf[pos++]='+';continue;}
            if (is_unreserved_component(c)){buf[pos++]=(char)c;continue;}
            buf[pos++]='%'; buf[pos++]=hex_chars[(c>>4)&0xF]; buf[pos++]=hex_chars[c&0xF];
        }
    }
    buf[pos]='\0';
    return buf;
}

/* ---- URLSearchParams native class ---- */

static js_value usp_method_get(js_vm *vm, void *self, int argc, js_value *argv)
{
    usp_t *u = (usp_t *)self;
    const char *key = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    if (!key) return js_native_make_null();
    for (int i=0; i<u->nentries; i++)
        if (url_streq(u->entries[i].key, key))
            return js_native_make_string(vm, u->entries[i].val);
    return js_native_make_null();
}
static js_value usp_method_has(js_vm *vm, void *self, int argc, js_value *argv)
{
    usp_t *u = (usp_t *)self;
    const char *key = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    if (!key) return js_native_make_bool(vm, 0);
    for (int i=0; i<u->nentries; i++)
        if (url_streq(u->entries[i].key, key)) return js_native_make_bool(vm, 1);
    return js_native_make_bool(vm, 0);
}
static js_value usp_method_set(js_vm *vm, void *self, int argc, js_value *argv)
{
    usp_t *u = (usp_t *)self;
    const char *key = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    const char *val = argc>1 ? js_native_to_cstr(vm, argv[1]) : "";
    if (!key) return js_native_make_undefined();
    int found=0, di=0;
    for (int i=0; i<u->nentries; i++) {
        if (url_streq(u->entries[i].key, key)) {
            if (!found) {
                free(u->entries[i].val);
                u->entries[i].val = usp_strdup(val);
                found=1;
                u->entries[di++]=u->entries[i];
            } else { free(u->entries[i].key); free(u->entries[i].val); }
        } else { u->entries[di++]=u->entries[i]; }
    }
    if (!found && u->nentries < USP_MAX_ENTRIES) {
        u->entries[u->nentries].key = usp_strdup(key);
        u->entries[u->nentries].val = usp_strdup(val);
        u->nentries++;
    } else { u->nentries = di; }
    return js_native_make_undefined();
}
static js_value usp_method_delete(js_vm *vm, void *self, int argc, js_value *argv)
{
    usp_t *u = (usp_t *)self;
    const char *key = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    if (!key) return js_native_make_undefined();
    int di=0;
    for (int i=0; i<u->nentries; i++) {
        if (url_streq(u->entries[i].key, key)) {
            free(u->entries[i].key); free(u->entries[i].val);
        } else { u->entries[di++]=u->entries[i]; }
    }
    u->nentries=di;
    (void)vm;
    return js_native_make_undefined();
}
static js_value usp_method_append(js_vm *vm, void *self, int argc, js_value *argv)
{
    usp_t *u = (usp_t *)self;
    const char *key = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    const char *val = argc>1 ? js_native_to_cstr(vm, argv[1]) : "";
    if (!key || u->nentries >= USP_MAX_ENTRIES) return js_native_make_undefined();
    u->entries[u->nentries].key = usp_strdup(key);
    u->entries[u->nentries].val = usp_strdup(val);
    if (u->entries[u->nentries].key && u->entries[u->nentries].val)
        u->nentries++;
    (void)vm;
    return js_native_make_undefined();
}
static js_value usp_method_toString(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)argc; (void)argv;
    usp_t *u = (usp_t *)self;
    char *s = usp_to_string(u);
    if (!s) return js_native_make_string(vm, "");
    js_value v = js_native_make_string(vm, s);
    free(s);
    return v;
}

/* Use js_native_method_entry (same layout as anonymous struct in js_native_class)
 * and cast via void* at install time -- same pattern as js_storage.c. */
static const js_native_method_entry usp_methods[] = {
    { "get",      usp_method_get      },
    { "has",      usp_method_has      },
    { "set",      usp_method_set      },
    { "delete",   usp_method_delete   },
    { "append",   usp_method_append   },
    { "toString", usp_method_toString },
    { NULL,       NULL                }
};
static const js_native_class usp_class_base = {
    "URLSearchParams", NULL, NULL, (void *)0   /* methods patched in install */
};
static int g_usp_class_id = -1;

static js_value fn_URLSearchParams(js_vm *vm, int argc, js_value *argv)
{
    const char *init = "";
    if (argc>0 && !js_native_is_null_or_undefined(argv[0]))
        init = js_native_to_cstr(vm, argv[0]);
    if (!init) init = "";
    usp_t *u = usp_new();
    if (!u) return js_native_make_undefined();
    usp_parse(u, init);
    if (g_usp_class_id < 0) { usp_free(u); return js_native_make_undefined(); }
    return js_native_wrap(vm, g_usp_class_id, u);
}

/* ======================================================================
 * URL native class
 * ====================================================================== */

static js_value url_prop_get(js_vm *vm, void *self, const char *prop)
{
    url_parsed *u = (url_parsed *)self;
    if (url_streq(prop, "href"))
        return js_native_make_string(vm, url_href_arena(vm, u));
    if (url_streq(prop, "protocol")) {
        if (!u->scheme[0]) return js_native_make_string(vm, "");
        char buf[68]; buf[0] = '\0';
        url_strcat_n(buf, sizeof(buf), u->scheme);
        url_strcat_n(buf, sizeof(buf), ":");
        return js_native_make_string(vm, buf);
    }
    if (url_streq(prop, "host")) {
        if (!u->host[0]) return js_native_make_string(vm, "");
        if (!u->port[0]) return js_native_make_string(vm, u->host);
        char buf[270]; buf[0] = '\0';
        url_strcat_n(buf, sizeof(buf), u->host);
        url_strcat_n(buf, sizeof(buf), ":");
        url_strcat_n(buf, sizeof(buf), u->port);
        return js_native_make_string(vm, buf);
    }
    if (url_streq(prop, "hostname")) return js_native_make_string(vm, u->host);
    if (url_streq(prop, "port"))     return js_native_make_string(vm, u->port);
    if (url_streq(prop, "pathname")) return js_native_make_string(vm, u->pathname[0]?u->pathname:"/");
    if (url_streq(prop, "search"))   return js_native_make_string(vm, u->search);
    if (url_streq(prop, "hash"))     return js_native_make_string(vm, u->hash);
    if (url_streq(prop, "origin"))   return js_native_make_string(vm, url_origin_arena(vm, u));
    return js_native_make_undefined();
}

static int url_prop_set(js_vm *vm, void *self, const char *prop, js_value val)
{
    url_parsed *u = (url_parsed *)self;
    const char *s = js_native_to_cstr(vm, val);
    if (!s) return 1;
    if (url_streq(prop, "pathname")) {
        buf_copy(u->pathname, sizeof(u->pathname), s, url_strlen(s)); return 0;
    }
    if (url_streq(prop, "search")) {
        if (s[0] && s[0]!='?') { u->search[0]='?'; buf_copy(u->search+1, sizeof(u->search)-1, s, url_strlen(s)); }
        else buf_copy(u->search, sizeof(u->search), s, url_strlen(s));
        return 0;
    }
    if (url_streq(prop, "hash")) {
        if (s[0] && s[0]!='#') { u->hash[0]='#'; buf_copy(u->hash+1, sizeof(u->hash)-1, s, url_strlen(s)); }
        else buf_copy(u->hash, sizeof(u->hash), s, url_strlen(s));
        return 0;
    }
    return 1;
}

static const js_native_class url_class = {
    "URL",
    url_prop_get,
    url_prop_set,
    (void *)0     /* no methods; all via property get/set */
};
static int g_url_class_id = -1;

static js_value fn_URL(js_vm *vm, int argc, js_value *argv)
{
    const char *href = argc>0 ? js_native_to_cstr(vm, argv[0]) : NULL;
    const char *base = argc>1 ? js_native_to_cstr(vm, argv[1]) : NULL;
    if (!href) {
        js_throw_str(vm, "TypeError: URL requires at least one argument");
        return js_native_make_undefined();
    }
    url_parsed *u = (url_parsed *)malloc(sizeof(url_parsed));
    if (!u) return js_native_make_undefined();

    url_parsed ref_parsed;
    if (url_parse(&ref_parsed, href, url_strlen(href)) < 0) {
        free(u);
        js_throw_str(vm, "TypeError: Failed to construct URL: invalid URL");
        return js_native_make_undefined();
    }
    if (base && !ref_parsed.scheme[0]) {
        url_parsed base_parsed;
        if (url_parse(&base_parsed, base, url_strlen(base)) < 0) {
            free(u);
            js_throw_str(vm, "TypeError: Failed to construct URL: invalid base URL");
            return js_native_make_undefined();
        }
        url_resolve(u, &base_parsed, &ref_parsed);
    } else {
        *u = ref_parsed;
        if (u->host[0] && u->pathname[0]=='\0') { u->pathname[0]='/'; u->pathname[1]='\0'; }
    }
    if (g_url_class_id < 0) { free(u); return js_native_make_undefined(); }
    return js_native_wrap(vm, g_url_class_id, u);
}

/* ======================================================================
 * js_url_install
 * ====================================================================== */

void js_url_install(js_vm *vm)
{
    /* Patch the methods pointer via (void*) -- same cast js_native.c uses
     * for method_entry_t (identical layout to the anonymous struct).      */
    js_native_class usp_cls = usp_class_base;
    usp_cls.methods = (const void *)usp_methods;

    g_url_class_id = js_native_register_class(vm, &url_class);
    g_usp_class_id = js_native_register_class(vm, &usp_cls);

    js_native_register_function(vm, "encodeURIComponent", fn_encodeURIComponent);
    js_native_register_function(vm, "decodeURIComponent", fn_decodeURIComponent);
    js_native_register_function(vm, "encodeURI",          fn_encodeURI);
    js_native_register_function(vm, "decodeURI",          fn_decodeURI);
    js_native_register_function(vm, "URL",                fn_URL);
    js_native_register_function(vm, "URLSearchParams",    fn_URLSearchParams);
}

#endif /* !JS_URL_STANDALONE_TEST */

/* ======================================================================
 * Self-test
 *
 * Purely C-side: URL parser, encode/decode, URLSearchParams.
 * Uses only static buffers -- no JS engine, no arena, no vm.
 * In standalone mode we provide our own malloc (static bump allocator).
 * ====================================================================== */

#ifdef JS_URL_STANDALONE_TEST
/* --- Minimal static bump allocator for standalone test --- */
static char  st_heap[65536];
static js_usize st_heap_pos = 0;
static void *st_malloc(js_usize n)
{
    n = (n + 7) & ~(js_usize)7;   /* 8-byte align */
    if (st_heap_pos + n > sizeof(st_heap)) return NULL;
    void *p = &st_heap[st_heap_pos];
    st_heap_pos += n;
    return p;
}
static void st_free(void *p) { (void)p; }  /* no-op for bump allocator */

/* URLSearchParams standalone re-definition (depends on malloc/free above) */
#define USP_MAX_ENTRIES 64
typedef struct { char *key; char *val; } usp_entry;
typedef struct { usp_entry entries[USP_MAX_ENTRIES]; int nentries; } usp_t;

static char *usp_strdup(const char *s)
{
    if (!s) s = "";
    js_usize len = url_strlen(s);
    char *p = (char *)st_malloc(len+1);
    if (!p) return NULL;
    url_memcpy(p, s, len); p[len]='\0'; return p;
}
static usp_t *usp_new(void)
{
    usp_t *u = (usp_t *)st_malloc(sizeof(usp_t));
    if (!u) return NULL; u->nentries=0; return u;
}
static void usp_free(usp_t *u) { (void)u; }
static void usp_decode_kv(const char *p, js_usize len,
                           char *kbuf, js_usize ksz,
                           char *vbuf, js_usize vsz)
{
    js_usize eq=0;
    while (eq<len && p[eq]!='=') eq++;
    js_usize kn=0;
    for (js_usize k=0; k<eq && kn<ksz-1; ) {
        if (p[k]=='+'){ kbuf[kn++]=' '; k++; continue; }
        if (p[k]=='%'&&k+2<eq){ int hi=hex_val(p[k+1]),lo=hex_val(p[k+2]); if(hi>=0&&lo>=0){kbuf[kn++]=(char)((hi<<4)|lo);k+=3;continue;} }
        kbuf[kn++]=p[k++];
    }
    kbuf[kn]='\0';
    js_usize vn=0, vs=(eq<len?eq+1:len);
    for (js_usize k=vs; k<len && vn<vsz-1; ) {
        if (p[k]=='+'){ vbuf[vn++]=' '; k++; continue; }
        if (p[k]=='%'&&k+2<len){ int hi=hex_val(p[k+1]),lo=hex_val(p[k+2]); if(hi>=0&&lo>=0){vbuf[vn++]=(char)((hi<<4)|lo);k+=3;continue;} }
        vbuf[vn++]=p[k++];
    }
    vbuf[vn]='\0';
}
static void usp_parse(usp_t *u, const char *init)
{
    if (!init||!init[0]) return;
    if (init[0]=='?') init++;
    js_usize len=url_strlen(init), i=0;
    while (i<=len && u->nentries<USP_MAX_ENTRIES) {
        js_usize j=i;
        while (j<len && init[j]!='&') j++;
        if (j>i) {
            char kbuf[256], vbuf[512];
            usp_decode_kv(init+i, j-i, kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));
            u->entries[u->nentries].key=usp_strdup(kbuf);
            u->entries[u->nentries].val=usp_strdup(vbuf);
            if (u->entries[u->nentries].key&&u->entries[u->nentries].val) u->nentries++;
        }
        i=j+1;
    }
}
static char *usp_to_string(usp_t *u)
{
    js_usize cap=0;
    for (int i=0;i<u->nentries;i++) cap+=(url_strlen(u->entries[i].key)+url_strlen(u->entries[i].val))*3+2;
    cap++;
    char *buf=(char*)st_malloc(cap); if(!buf) return NULL;
    js_usize pos=0;
    for (int i=0;i<u->nentries;i++) {
        if (i) buf[pos++]='&';
        const char *k=u->entries[i].key;
        while (*k){ unsigned char c=(unsigned char)*k++; if(c==' '){buf[pos++]='+';continue;} if(is_unreserved_component(c)){buf[pos++]=(char)c;continue;} buf[pos++]='%';buf[pos++]=hex_chars[(c>>4)&0xF];buf[pos++]=hex_chars[c&0xF]; }
        buf[pos++]='=';
        const char *v=u->entries[i].val;
        while (*v){ unsigned char c=(unsigned char)*v++; if(c==' '){buf[pos++]='+';continue;} if(is_unreserved_component(c)){buf[pos++]=(char)c;continue;} buf[pos++]='%';buf[pos++]=hex_chars[(c>>4)&0xF];buf[pos++]=hex_chars[c&0xF]; }
    }
    buf[pos]='\0'; return buf;
}
#endif /* JS_URL_STANDALONE_TEST */

/* ======================================================================
 * Write to fd 1 (freestanding syscall)
 * ====================================================================== */

static void selftest_write(const char *s, js_usize n)
{
    long r;
    /* CRITICAL: AutomationOS SYS_WRITE = 3. Linux's write=1 must NOT be used:
     * syscall 1 on AutomationOS is SYS_FORK, and st_check calls this on EVERY
     * assertion (pass and fail), so write=1 fork-bombed the system (4000+
     * forks, exhausting the process table). */
    __asm__ volatile (
        "syscall"
        : "=a"(r)
        : "0"(3L), "D"(1L), "S"(s), "d"(n)
        : "rcx", "r11", "memory"
    );
    (void)r;
}
static void st_puts(const char *s)
{
    selftest_write(s, url_strlen(s));
    selftest_write("\n", 1);
}
static int st_check(int cond, const char *name, const char *got, const char *expected)
{
    if (cond) { selftest_write("[PASS] ", 7); st_puts(name); return 0; }
    selftest_write("[FAIL] ", 7);
    selftest_write(name, url_strlen(name));
    selftest_write(" -- got:      ", 14);
    st_puts(got ? got : "(null)");
    selftest_write("              expected: ", 24);
    st_puts(expected ? expected : "(null)");
    return 1;
}

int js_url_selftest(void)
{
    int fails = 0;
    char enc_buf[1024], dec_buf[1024], href_buf[1024];

    /* ---- encodeURIComponent ---- */
    pct_encode_buf("a b/c", 5, is_unreserved_component, enc_buf, sizeof(enc_buf));
    fails += st_check(url_streq(enc_buf, "a%20b%2Fc"),
                      "encodeURIComponent(\"a b/c\")", enc_buf, "a%20b%2Fc");

    pct_encode_buf("hello", 5, is_unreserved_component, enc_buf, sizeof(enc_buf));
    fails += st_check(url_streq(enc_buf, "hello"),
                      "encodeURIComponent(\"hello\")", enc_buf, "hello");

    /* All unreserved chars pass through */
    {
        const char *un = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";
        pct_encode_buf(un, url_strlen(un), is_unreserved_component, enc_buf, sizeof(enc_buf));
        fails += st_check(url_streq(enc_buf, un),
                          "encodeURIComponent unreserved passthrough", enc_buf, un);
    }

    /* ---- decodeURIComponent ---- */
    pct_decode_buf("a%20b", 5, NULL, dec_buf, sizeof(dec_buf));
    fails += st_check(url_streq(dec_buf, "a b"),
                      "decodeURIComponent(\"a%20b\")", dec_buf, "a b");

    pct_decode_buf("hello%21", 8, NULL, dec_buf, sizeof(dec_buf));
    fails += st_check(url_streq(dec_buf, "hello!"),
                      "decodeURIComponent(\"hello%21\")", dec_buf, "hello!");

    pct_decode_buf("%2F%3F%23", 9, NULL, dec_buf, sizeof(dec_buf));
    fails += st_check(url_streq(dec_buf, "/?#"),
                      "decodeURIComponent(\"%2F%3F%23\")", dec_buf, "/?#");

    /* ---- encodeURI ---- */
    {
        const char *in = "https://example.com/a b";
        pct_encode_buf(in, url_strlen(in), is_unreserved_uri, enc_buf, sizeof(enc_buf));
        fails += st_check(url_streq(enc_buf, "https://example.com/a%20b"),
                          "encodeURI(\"https://example.com/a b\")",
                          enc_buf, "https://example.com/a%20b");
    }
    {
        /* Reserved URI chars are NOT encoded by encodeURI */
        const char *in = "https://x.com/?q=a&b=1";
        pct_encode_buf(in, url_strlen(in), is_unreserved_uri, enc_buf, sizeof(enc_buf));
        fails += st_check(url_streq(enc_buf, in),
                          "encodeURI preserves :/?&= etc.", enc_buf, in);
    }

    /* ---- decodeURI (reserved chars preserved if found encoded) ---- */
    {
        /* %2F is '/', which is in encodeURI's reserved set; must NOT be decoded */
        pct_decode_buf("path%2Fsub", 10, is_unreserved_uri, dec_buf, sizeof(dec_buf));
        fails += st_check(url_streq(dec_buf, "path%2Fsub"),
                          "decodeURI preserves %2F (reserved)", dec_buf, "path%2Fsub");
    }
    {
        /* %20 is space, not in encodeURI reserved set; IS decoded */
        pct_decode_buf("hello%20world", 13, is_unreserved_uri, dec_buf, sizeof(dec_buf));
        fails += st_check(url_streq(dec_buf, "hello world"),
                          "decodeURI decodes %20 (not reserved)", dec_buf, "hello world");
    }

    /* ---- URL parser: absolute URL ---- */
    {
        url_parsed u;
        url_parse(&u, "https://example.com:8443/path?q=1#frag",
                  url_strlen("https://example.com:8443/path?q=1#frag"));
        fails += st_check(url_streq(u.scheme,"https"),
                          "parse abs: scheme", u.scheme, "https");
        fails += st_check(url_streq(u.host,"example.com"),
                          "parse abs: host", u.host, "example.com");
        fails += st_check(url_streq(u.port,"8443"),
                          "parse abs: port", u.port, "8443");
        fails += st_check(url_streq(u.pathname,"/path"),
                          "parse abs: pathname", u.pathname, "/path");
        fails += st_check(url_streq(u.search,"?q=1"),
                          "parse abs: search", u.search, "?q=1");
        fails += st_check(url_streq(u.hash,"#frag"),
                          "parse abs: hash", u.hash, "#frag");
    }

    /* ---- URL parser: no port ---- */
    {
        url_parsed u;
        url_parse(&u, "http://example.com/", url_strlen("http://example.com/"));
        fails += st_check(url_streq(u.host,"example.com"),
                          "parse no-port: host", u.host, "example.com");
        fails += st_check(u.port[0]=='\0',
                          "parse no-port: port empty", u.port, "");
        fails += st_check(url_streq(u.pathname,"/"),
                          "parse no-port: pathname", u.pathname, "/");
    }

    /* ---- URL resolver: absolute-path ref against base ---- */
    /* "/p?x=1" vs "https://h:8/dir/"  =>  "https://h:8/p?x=1" */
    {
        url_parsed base, ref, out;
        url_parse(&base, "https://h:8/dir/", url_strlen("https://h:8/dir/"));
        url_parse(&ref,  "/p?x=1",           url_strlen("/p?x=1"));
        url_resolve(&out, &base, &ref);
        url_href_buf(&out, href_buf, sizeof(href_buf));
        fails += st_check(url_streq(href_buf, "https://h:8/p?x=1"),
                          "resolve /p?x=1 vs https://h:8/dir/",
                          href_buf, "https://h:8/p?x=1");
    }

    /* ---- URL resolver: relative-path ref ---- */
    /* "sub/page" vs "https://h/a/b"  =>  "https://h/a/sub/page" */
    {
        url_parsed base, ref, out;
        url_parse(&base, "https://h/a/b",  url_strlen("https://h/a/b"));
        url_parse(&ref,  "sub/page",        url_strlen("sub/page"));
        url_resolve(&out, &base, &ref);
        url_href_buf(&out, href_buf, sizeof(href_buf));
        fails += st_check(url_streq(href_buf, "https://h/a/sub/page"),
                          "resolve sub/page vs https://h/a/b",
                          href_buf, "https://h/a/sub/page");
    }

    /* ---- URL resolver: "../" navigation ---- */
    /* "../other" vs "https://h/a/b/c"  =>  "https://h/a/other" */
    {
        url_parsed base, ref, out;
        url_parse(&base, "https://h/a/b/c", url_strlen("https://h/a/b/c"));
        url_parse(&ref,  "../other",         url_strlen("../other"));
        url_resolve(&out, &base, &ref);
        url_href_buf(&out, href_buf, sizeof(href_buf));
        fails += st_check(url_streq(href_buf, "https://h/a/other"),
                          "resolve ../other vs https://h/a/b/c",
                          href_buf, "https://h/a/other");
    }

    /* ---- remove_dot_segments ---- */
    {
        url_parsed u;
        url_parse(&u, "https://h/a/b/../c", url_strlen("https://h/a/b/../c"));
        remove_dot_segments(u.pathname);
        fails += st_check(url_streq(u.pathname, "/a/c"),
                          "remove_dot_segments /a/b/../c", u.pathname, "/a/c");
    }
    {
        char path[64]; url_strcpy(path, "/a/./b/./c");
        remove_dot_segments(path);
        fails += st_check(url_streq(path, "/a/b/c"),
                          "remove_dot_segments /a/./b/./c", path, "/a/b/c");
    }

    /* ---- URLSearchParams ---- */
    {
        usp_t *u = usp_new();
        usp_parse(u, "key=val&foo=bar&foo=baz");
        fails += st_check(url_streq(u->entries[0].val, "val"),
                          "USP parse key=val", u->entries[0].val, "val");
        int has_foo=0;
        for (int i=0;i<u->nentries;i++) if (url_streq(u->entries[i].key,"foo")) has_foo=1;
        fails += st_check(has_foo, "USP has foo", has_foo?"yes":"no", "yes");

        /* Simulate .toString() */
        char *s = usp_to_string(u);
        fails += st_check(url_streq(s,"key=val&foo=bar&foo=baz"),
                          "USP toString", s, "key=val&foo=bar&foo=baz");

        /* Simulate .delete("foo") */
        int di=0;
        for (int i=0;i<u->nentries;i++) {
            if (!url_streq(u->entries[i].key,"foo")) u->entries[di++]=u->entries[i];
        }
        u->nentries=di;
        char *s2 = usp_to_string(u);
        fails += st_check(url_streq(s2,"key=val"), "USP delete foo -> toString", s2, "key=val");
        usp_free(u);
    }

    /* ---- USP: + and %20 decoding ---- */
    {
        usp_t *u = usp_new();
        usp_parse(u, "q=hello+world&v=a%20b");
        fails += st_check(url_streq(u->entries[0].val,"hello world"),
                          "USP + decode", u->entries[0].val, "hello world");
        fails += st_check(url_streq(u->entries[1].val,"a b"),
                          "USP %20 decode", u->entries[1].val, "a b");
        usp_free(u);
    }

    /* ---- USP: .append and encoding ---- */
    {
        usp_t *u = usp_new();
        usp_parse(u, "a=1");
        /* append b=hello world */
        if (u->nentries < USP_MAX_ENTRIES) {
            u->entries[u->nentries].key = usp_strdup("b");
            u->entries[u->nentries].val = usp_strdup("hello world");
            u->nentries++;
        }
        char *s = usp_to_string(u);
        fails += st_check(url_streq(s, "a=1&b=hello+world"),
                          "USP append + encode spaces", s, "a=1&b=hello+world");
        usp_free(u);
    }

    /* Summary */
    {
        char nbuf[16];
        url_uitoa((unsigned int)fails, nbuf);
        if (fails == 0) st_puts("js_url_selftest: all tests PASSED");
        else {
            selftest_write("js_url_selftest: ", 17);
            selftest_write(nbuf, url_strlen(nbuf));
            st_puts(" test(s) FAILED");
        }
    }
    return fails;
}

#ifdef JS_URL_STANDALONE_TEST
/*
 * Standalone selftest entry point.
 * Linked with crt0.asm (or a trivial _start) and no libc.
 */
void _start(void)
{
    int result = js_url_selftest();
    /* SYS_EXIT = 60 on x86_64 Linux */
    __asm__ volatile (
        "syscall"
        :
        : "a"(60L), "D"((long)result)
        : "memory"
    );
    __builtin_unreachable();
}
#endif /* JS_URL_STANDALONE_TEST */

/*
 * json.c -- freestanding, allocation-free DOM JSON parser (RFC 8259).
 * =======================================================================
 *
 * Pure ring-3 userspace. NO libc, NO stdio, NO malloc, NO standard headers.
 * Everything (string length/compare, digit math, double assembly, UTF-8
 * encoding, whitespace handling) is implemented here on plain in-memory
 * buffers handed in by the caller.
 *
 * Parsing strategy: recursive descent with an explicit depth counter that
 * caps nesting at JSON_MAX_DEPTH (default 64) so a hostile/deep document
 * cannot blow the userspace stack. Container children are linked via
 * first_child / next_sibling indices into the caller's fixed node pool;
 * if the pool is exhausted we fail with a clear error rather than overflow.
 *
 * String policy: STRING values and OBJECT keys are stored as raw
 * (pointer,len) slices pointing into the source `text`. They are NOT
 * NUL-terminated and still contain escape sequences. json_unescape()
 * decodes a slice on demand. All key comparisons decode on the fly so the
 * caller always works with logical (decoded) strings.
 *
 * Build (objdump must show NO `fs:0x28` stack canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/json/json.c -o json.o
 */

#include "json.h"

#define JSON_MAX_DEPTH 64

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned long  usize;

/* =========================================================================
 *  Tiny freestanding character helpers (no <ctype.h>).
 * ========================================================================= */

static int j_is_ws(int c)
{
    /* RFC 8259 whitespace: space, tab, LF, CR. */
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int j_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* Hex digit value, or -1 if not a hex digit. */
static int j_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* C-string length (for caller-supplied keys). */
static usize j_strlen(const char *s)
{
    usize n = 0;
    while (s[n]) n++;
    return n;
}

/* =========================================================================
 *  Parser state.
 * ========================================================================= */

typedef struct {
    const char *s;     /* source buffer            */
    usize       len;   /* total length             */
    usize       pos;   /* current read offset      */
    json_doc   *doc;   /* owning doc (for nodes/err) */
    int         depth; /* current nesting depth    */
} jparser;

/* Set the error message exactly once (first error wins) and return -1. */
static int j_fail(jparser *p, const char *msg)
{
    if (!p->doc->err) p->doc->err = msg;
    return -1;
}

/* Allocate one node from the fixed pool. Returns its index, or -1 (and sets
 * the error) if the pool is exhausted. The node is fully initialised. */
static int j_alloc(jparser *p)
{
    json_doc *d = p->doc;
    if (d->used >= d->cap)
        return j_fail(p, "node pool exhausted");
    int idx = d->used++;
    json_node *n = &d->nodes[idx];
    n->type = JSON_NULL;
    n->str = 0;       n->slen = 0;
    n->inum = 0;      n->dnum = 0.0;
    n->first_child = -1;
    n->next_sibling = -1;
    n->key = 0;       n->klen = 0;
    return idx;
}

/* Skip RFC 8259 whitespace. */
static void j_skip_ws(jparser *p)
{
    while (p->pos < p->len && j_is_ws((u8)p->s[p->pos]))
        p->pos++;
}

/* Peek current byte, or -1 at EOF. */
static int j_peek(jparser *p)
{
    return (p->pos < p->len) ? (u8)p->s[p->pos] : -1;
}

/* Forward declaration: the one true recursive entry. */
static int j_value(jparser *p);

/* =========================================================================
 *  String escape decoding helpers.
 *
 *  j_scan_string()  -- validate a JSON string at p->pos and record its raw
 *                      slice (between the quotes) into a node, advancing past
 *                      the closing quote. Does NOT decode.
 *  j_decode_slice() -- decode a raw slice into an output buffer (used by both
 *                      json_unescape and the key-compare path).
 * ========================================================================= */

/* Append a Unicode code point to out as UTF-8. *op is the running write index.
 * Returns 0 on success, -1 if it would overflow out_cap. */
static int j_emit_utf8(char *out, usize out_cap, usize *op, u32 cp)
{
    usize o = *op;
    if (cp <= 0x7F) {
        if (o + 1 > out_cap) return -1;
        out[o++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (o + 2 > out_cap) return -1;
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (o + 3 > out_cap) return -1;
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (o + 4 > out_cap) return -1;
        out[o++] = (char)(0xF0 | (cp >> 18));
        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    *op = o;
    return 0;
}

/*
 * Decode a raw JSON string slice raw[0..rlen) (the bytes BETWEEN the quotes,
 * still containing escapes) into out[0..out_cap). Always NUL-terminates on
 * success. Returns decoded length (excluding NUL), or -1 on error.
 *
 * If out is NULL this runs in "measure/validate" mode: it still walks the
 * whole slice checking escape validity and returns the decoded length that
 * WOULD be produced (out_cap is ignored as a write bound but treated as
 * unbounded), or -1 on a malformed escape.
 */
static int j_decode_slice(const char *raw, usize rlen,
                          char *out, usize out_cap)
{
    usize i = 0, o = 0;
    const int measure = (out == 0);

    while (i < rlen) {
        u8 c = (u8)raw[i];
        if (c != '\\') {
            /* Ordinary byte (control chars were already rejected on scan). */
            if (measure) { o++; i++; continue; }
            if (o + 1 > out_cap) return -1;
            out[o++] = (char)c;
            i++;
            continue;
        }

        /* Escape sequence: need at least one more char. */
        i++;
        if (i >= rlen) return -1;
        u8 e = (u8)raw[i++];

        /* Single-byte escapes share one emit helper to keep the index math in
         * exactly one place (avoids double-increment hazards). */
        char one = 0;
        switch (e) {
            case '"':  one = '"';  break;
            case '\\': one = '\\'; break;
            case '/':  one = '/';  break;
            case 'b':  one = '\b'; break;
            case 'f':  one = '\f'; break;
            case 'n':  one = '\n'; break;
            case 'r':  one = '\r'; break;
            case 't':  one = '\t'; break;
            case 'u': {
                /* \uXXXX -- four hex digits, with surrogate-pair handling. */
                if (i + 4 > rlen) return -1;
                u32 cp = 0;
                for (int k = 0; k < 4; k++) {
                    int hv = j_hexval((u8)raw[i + k]);
                    if (hv < 0) return -1;
                    cp = (cp << 4) | (u32)hv;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* High surrogate: must be followed by \uDC00..\uDFFF. */
                    if (i + 6 > rlen || raw[i] != '\\' || raw[i + 1] != 'u')
                        return -1;
                    u32 lo = 0;
                    for (int k = 0; k < 4; k++) {
                        int hv = j_hexval((u8)raw[i + 2 + k]);
                        if (hv < 0) return -1;
                        lo = (lo << 4) | (u32)hv;
                    }
                    if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                    i += 6;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    /* Lone low surrogate is invalid. */
                    return -1;
                }
                if (measure) {
                    /* Count the UTF-8 bytes without writing. */
                    usize tmp = 0;
                    /* Use a scratch big enough for any code point (4). */
                    char dummy[4];
                    if (j_emit_utf8(dummy, sizeof dummy, &tmp, cp) < 0)
                        return -1;
                    o += tmp;
                } else {
                    if (j_emit_utf8(out, out_cap, &o, cp) < 0) return -1;
                }
                continue;
            }
            default:
                return -1; /* unknown escape */
        }

        /* Emit the single-byte result (the \u case used `continue` above). */
        if (measure) {
            o++;
        } else {
            if (o + 1 > out_cap) return -1;
            out[o++] = one;
        }
    }

    if (!measure) {
        if (o + 1 > out_cap) return -1; /* room for NUL */
        out[o] = '\0';
    }
    return (int)o;
}

/*
 * Scan a JSON string starting at p->pos which MUST point at the opening '"'.
 * On success records the raw inner slice into nd (str/slen for a value, or
 * caller copies into key/klen) and advances p->pos past the closing '"'.
 * Returns 0 on success, -1 on error.
 *
 * `out_str`/`out_len` receive the slice pointer/length so the caller can
 * route it to either the value slot or the key slot.
 */
static int j_scan_string(jparser *p, const char **out_str, usize *out_len)
{
    if (j_peek(p) != '"')
        return j_fail(p, "expected string");
    p->pos++; /* consume opening quote */

    usize start = p->pos;
    while (p->pos < p->len) {
        u8 c = (u8)p->s[p->pos];
        if (c == '"') {
            *out_str = p->s + start;
            *out_len = p->pos - start;
            p->pos++; /* consume closing quote */
            return 0;
        }
        if (c == '\\') {
            /* Validate the escape now so callers can trust the slice. */
            p->pos++;
            if (p->pos >= p->len)
                return j_fail(p, "unterminated escape");
            u8 e = (u8)p->s[p->pos];
            if (e == 'u') {
                if (p->pos + 4 >= p->len)
                    return j_fail(p, "truncated \\u escape");
                for (int k = 1; k <= 4; k++)
                    if (j_hexval((u8)p->s[p->pos + k]) < 0)
                        return j_fail(p, "bad \\u hex digit");
                p->pos += 4;
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' ||
                       e == 'f' || e == 'n' || e == 'r' || e == 't') {
                /* ok */
            } else {
                return j_fail(p, "invalid escape");
            }
            p->pos++;
            continue;
        }
        if (c < 0x20)
            return j_fail(p, "control char in string");
        p->pos++;
    }
    return j_fail(p, "unterminated string");
}

/* =========================================================================
 *  Number parsing (RFC 8259): int, fraction, exponent. Pure integer/double
 *  math -- no strtod, no libm.
 * ========================================================================= */

static int j_number(jparser *p, json_node *nd)
{
    usize start = p->pos;
    int neg = 0;

    if (j_peek(p) == '-') { neg = 1; p->pos++; }

    /* Integer part: either a single 0 or [1-9][0-9]* (no leading zeros). */
    if (j_peek(p) == '0') {
        p->pos++;
    } else if (j_is_digit(j_peek(p))) {
        while (j_is_digit(j_peek(p))) p->pos++;
    } else {
        return j_fail(p, "invalid number");
    }

    /* Accumulate the integer value with overflow-tolerant math. We build the
     * double from scratch for full range, and an integer for the common case. */
    long long inum = 0;
    int int_overflow = 0;
    {
        usize i = start + (neg ? 1 : 0);
        usize int_end = p->pos;
        for (; i < int_end; i++) {
            int dgt = p->s[i] - '0';
            /* Detect overflow of the magnitude. */
            if (inum > (9223372036854775807LL - dgt) / 10)
                int_overflow = 1;
            if (!int_overflow)
                inum = inum * 10 + dgt;
        }
        if (neg) inum = -inum;
    }

    int is_float = 0;

    /* Fraction. */
    if (j_peek(p) == '.') {
        is_float = 1;
        p->pos++;
        if (!j_is_digit(j_peek(p)))
            return j_fail(p, "missing fraction digits");
        while (j_is_digit(j_peek(p))) p->pos++;
    }

    /* Exponent. */
    if (j_peek(p) == 'e' || j_peek(p) == 'E') {
        is_float = 1;
        p->pos++;
        if (j_peek(p) == '+' || j_peek(p) == '-') p->pos++;
        if (!j_is_digit(j_peek(p)))
            return j_fail(p, "missing exponent digits");
        while (j_is_digit(j_peek(p))) p->pos++;
    }

    /* Build the double from the full lexeme so floats/exponents/huge ints are
     * represented faithfully. Manual parse: mantissa + base-10 exponent. */
    double dnum;
    {
        usize i = start;
        int dneg = 0;
        if (p->s[i] == '-') { dneg = 1; i++; }

        double mant = 0.0;
        int exp10 = 0;

        /* integer digits */
        while (i < p->pos && j_is_digit((u8)p->s[i])) {
            mant = mant * 10.0 + (double)(p->s[i] - '0');
            i++;
        }
        /* fraction digits */
        if (i < p->pos && p->s[i] == '.') {
            i++;
            while (i < p->pos && j_is_digit((u8)p->s[i])) {
                mant = mant * 10.0 + (double)(p->s[i] - '0');
                exp10--;
                i++;
            }
        }
        /* exponent */
        if (i < p->pos && (p->s[i] == 'e' || p->s[i] == 'E')) {
            i++;
            int esign = 1;
            if (i < p->pos && (p->s[i] == '+' || p->s[i] == '-')) {
                if (p->s[i] == '-') esign = -1;
                i++;
            }
            int ev = 0;
            while (i < p->pos && j_is_digit((u8)p->s[i])) {
                ev = ev * 10 + (p->s[i] - '0');
                i++;
            }
            exp10 += esign * ev;
        }

        /* Apply base-10 exponent by repeated multiply/divide using a scaled
         * power-of-ten ladder (avoids pulling in pow()). */
        double scale = 1.0;
        int e = exp10 < 0 ? -exp10 : exp10;
        double base = 10.0;
        while (e) {
            if (e & 1) scale *= base;
            base *= base;
            e >>= 1;
        }
        dnum = (exp10 < 0) ? (mant / scale) : (mant * scale);
        if (dneg) dnum = -dnum;
    }

    nd->type = JSON_NUMBER;
    nd->dnum = dnum;
    /* If the literal was a plain integer that fit, keep the exact value;
     * otherwise fall back to the truncated double. */
    if (!is_float && !int_overflow)
        nd->inum = inum;
    else
        nd->inum = (long long)dnum;

    return 0;
}

/* =========================================================================
 *  Literal matching: true / false / null.
 * ========================================================================= */

static int j_match_lit(jparser *p, const char *lit)
{
    usize i = 0;
    while (lit[i]) {
        if (p->pos + i >= p->len || p->s[p->pos + i] != lit[i])
            return 0;
        i++;
    }
    p->pos += i;
    return 1;
}

/* =========================================================================
 *  Container parsing.
 * ========================================================================= */

static int j_array(jparser *p, int self)
{
    p->pos++; /* consume '[' */
    j_skip_ws(p);

    json_node *arr = &p->doc->nodes[self];
    arr->type = JSON_ARRAY;

    if (j_peek(p) == ']') { p->pos++; return 0; } /* empty array */

    int prev = -1;
    for (;;) {
        int child = j_value(p);
        if (child < 0) return -1;

        /* `arr` may be stale after j_value allocated more nodes only if the
         * pool was an external buffer that moved -- it never moves (caller
         * fixed array), but indices are stable so re-fetch by index to be safe
         * across any future change. */
        if (prev < 0)
            p->doc->nodes[self].first_child = child;
        else
            p->doc->nodes[prev].next_sibling = child;
        prev = child;

        j_skip_ws(p);
        int c = j_peek(p);
        if (c == ',') { p->pos++; j_skip_ws(p); continue; }
        if (c == ']') { p->pos++; return 0; }
        return j_fail(p, "expected ',' or ']' in array");
    }
}

static int j_object(jparser *p, int self)
{
    p->pos++; /* consume '{' */
    j_skip_ws(p);

    p->doc->nodes[self].type = JSON_OBJECT;

    if (j_peek(p) == '}') { p->pos++; return 0; } /* empty object */

    int prev = -1;
    for (;;) {
        j_skip_ws(p);
        if (j_peek(p) != '"')
            return j_fail(p, "expected object key string");

        const char *kstr; usize klen;
        if (j_scan_string(p, &kstr, &klen) < 0) return -1;

        j_skip_ws(p);
        if (j_peek(p) != ':')
            return j_fail(p, "expected ':' after key");
        p->pos++;
        j_skip_ws(p);

        int child = j_value(p);
        if (child < 0) return -1;

        json_node *cn = &p->doc->nodes[child];
        cn->key = kstr;
        cn->klen = klen;

        if (prev < 0)
            p->doc->nodes[self].first_child = child;
        else
            p->doc->nodes[prev].next_sibling = child;
        prev = child;

        j_skip_ws(p);
        int c = j_peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == '}') { p->pos++; return 0; }
        return j_fail(p, "expected ',' or '}' in object");
    }
}

/* Parse exactly one JSON value at p->pos into a freshly allocated node.
 * Returns the node index, or -1 on error. */
static int j_value(jparser *p)
{
    if (p->depth >= JSON_MAX_DEPTH)
        return j_fail(p, "max nesting depth exceeded");

    j_skip_ws(p);
    int c = j_peek(p);
    if (c < 0)
        return j_fail(p, "unexpected end of input");

    int idx = j_alloc(p);
    if (idx < 0) return -1;
    json_node *nd = &p->doc->nodes[idx];

    switch (c) {
        case '{':
            p->depth++;
            if (j_object(p, idx) < 0) return -1;
            p->depth--;
            return idx;

        case '[':
            p->depth++;
            if (j_array(p, idx) < 0) return -1;
            p->depth--;
            return idx;

        case '"': {
            const char *s; usize l;
            if (j_scan_string(p, &s, &l) < 0) return -1;
            nd->type = JSON_STRING;
            nd->str = s;
            nd->slen = l;
            return idx;
        }

        case 't':
            if (!j_match_lit(p, "true")) return j_fail(p, "invalid literal");
            nd->type = JSON_BOOL;
            nd->inum = 1;
            return idx;

        case 'f':
            if (!j_match_lit(p, "false")) return j_fail(p, "invalid literal");
            nd->type = JSON_BOOL;
            nd->inum = 0;
            return idx;

        case 'n':
            if (!j_match_lit(p, "null")) return j_fail(p, "invalid literal");
            nd->type = JSON_NULL;
            return idx;

        default:
            if (c == '-' || j_is_digit(c)) {
                if (j_number(p, nd) < 0) return -1;
                return idx;
            }
            return j_fail(p, "unexpected character");
    }
}

/* =========================================================================
 *  Public API.
 * ========================================================================= */

int json_parse(json_doc *doc, json_node *pool, int pool_cap,
               const char *text, unsigned long len)
{
    if (!doc) return -1;
    doc->nodes = pool;
    doc->cap   = pool_cap;
    doc->used  = 0;
    doc->err   = 0;

    if (!pool || pool_cap <= 0) { doc->err = "no node pool"; return -1; }
    if (!text)                  { doc->err = "no input";     return -1; }

    jparser p;
    p.s = text; p.len = len; p.pos = 0; p.doc = doc; p.depth = 0;

    int root = j_value(&p);
    if (root < 0) return -1;

    /* Trailing content (after whitespace) is an error. */
    j_skip_ws(&p);
    if (p.pos != len) {
        doc->err = "trailing data after value";
        return -1;
    }
    return root;
}

/* Compare a decoded object-member key against a NUL-terminated C string.
 * Returns 1 on equal. Decodes the raw key on the fly, byte by byte, so the
 * comparison is over logical (unescaped) content. */
static int j_key_equals(const json_node *n, const char *key)
{
    /* Fast path: if the raw key contains no backslash and the lengths match,
     * compare directly. */
    usize klen = j_strlen(key);

    /* Detect escapes. */
    int has_esc = 0;
    for (usize i = 0; i < n->klen; i++) {
        if (n->key[i] == '\\') { has_esc = 1; break; }
    }

    if (!has_esc) {
        if (n->klen != klen) return 0;
        for (usize i = 0; i < klen; i++)
            if (n->key[i] != key[i]) return 0;
        return 1;
    }

    /* Escaped path: decode byte by byte and compare. We reuse j_decode_slice
     * in a bounded, streaming-friendly way by decoding into a small window is
     * awkward; instead decode the whole key into a stack buffer up to a sane
     * cap. Keys longer than the cap simply won't match (rare for API keys). */
    char buf[256];
    int dl = j_decode_slice(n->key, n->klen, buf, sizeof buf);
    if (dl < 0) return 0;
    if ((usize)dl != klen) return 0;
    for (usize i = 0; i < klen; i++)
        if (buf[i] != key[i]) return 0;
    return 1;
}

int json_object_get(const json_doc *doc, int obj_index, const char *key)
{
    if (!doc || obj_index < 0 || obj_index >= doc->used) return -1;
    const json_node *o = &doc->nodes[obj_index];
    if (o->type != JSON_OBJECT) return -1;

    for (int c = o->first_child; c >= 0; c = doc->nodes[c].next_sibling) {
        if (j_key_equals(&doc->nodes[c], key))
            return c;
    }
    return -1;
}

int json_array_get(const json_doc *doc, int arr_index, int i)
{
    if (!doc || arr_index < 0 || arr_index >= doc->used || i < 0) return -1;
    const json_node *a = &doc->nodes[arr_index];
    if (a->type != JSON_ARRAY) return -1;

    int c = a->first_child;
    while (c >= 0 && i > 0) {
        c = doc->nodes[c].next_sibling;
        i--;
    }
    return (i == 0) ? c : -1;
}

int json_array_len(const json_doc *doc, int arr_index)
{
    if (!doc || arr_index < 0 || arr_index >= doc->used) return -1;
    const json_node *a = &doc->nodes[arr_index];
    if (a->type != JSON_ARRAY) return -1;

    int n = 0;
    for (int c = a->first_child; c >= 0; c = doc->nodes[c].next_sibling)
        n++;
    return n;
}

int json_unescape(const json_doc *doc, int str_index,
                  char *out, unsigned long out_cap)
{
    if (!doc || str_index < 0 || str_index >= doc->used) return -1;
    const json_node *n = &doc->nodes[str_index];
    if (n->type != JSON_STRING) return -1;
    if (!out || out_cap == 0) return -1;
    return j_decode_slice(n->str, n->slen, out, out_cap);
}

/* =========================================================================
 *  Self test.  Returns 0 only if EVERY check passes.
 * ========================================================================= */

/* Compare a STRING node's decoded value against a C literal. */
static int j_str_is(const json_doc *doc, int idx, const char *want)
{
    if (idx < 0 || idx >= doc->used) return 0;
    if (doc->nodes[idx].type != JSON_STRING) return 0;
    char buf[64];
    int dl = json_unescape(doc, idx, buf, sizeof buf);
    if (dl < 0) return 0;
    usize wl = j_strlen(want);
    if ((usize)dl != wl) return 0;
    for (usize i = 0; i < wl; i++)
        if (buf[i] != want[i]) return 0;
    return 1;
}

int json_selftest(void)
{
    static const char sample[] =
        "{\"name\":\"AutomationOS\",\"ver\":1,\"net\":true,"
        "\"tags\":[\"os\",\"x86_64\"],\"mem\":{\"mb\":512}}";

    json_node pool[64];
    json_doc doc;

    int root = json_parse(&doc, pool, 64, sample, sizeof(sample) - 1);
    if (root < 0) return 1;
    if (doc.nodes[root].type != JSON_OBJECT) return 2;

    /* "name" -> STRING "AutomationOS" */
    int i_name = json_object_get(&doc, root, "name");
    if (i_name < 0) return 3;
    if (!j_str_is(&doc, i_name, "AutomationOS")) return 4;

    /* "ver" -> NUMBER 1 */
    int i_ver = json_object_get(&doc, root, "ver");
    if (i_ver < 0) return 5;
    if (doc.nodes[i_ver].type != JSON_NUMBER) return 6;
    if (doc.nodes[i_ver].inum != 1) return 7;

    /* "net" -> BOOL true */
    int i_net = json_object_get(&doc, root, "net");
    if (i_net < 0) return 8;
    if (doc.nodes[i_net].type != JSON_BOOL) return 9;
    if (doc.nodes[i_net].inum != 1) return 10;

    /* "tags" -> ARRAY len 2, [0] == "os" */
    int i_tags = json_object_get(&doc, root, "tags");
    if (i_tags < 0) return 11;
    if (doc.nodes[i_tags].type != JSON_ARRAY) return 12;
    if (json_array_len(&doc, i_tags) != 2) return 13;
    int i_t0 = json_array_get(&doc, i_tags, 0);
    if (!j_str_is(&doc, i_t0, "os")) return 14;
    int i_t1 = json_array_get(&doc, i_tags, 1);
    if (!j_str_is(&doc, i_t1, "x86_64")) return 15;

    /* "mem" -> OBJECT, "mb" == 512 */
    int i_mem = json_object_get(&doc, root, "mem");
    if (i_mem < 0) return 16;
    if (doc.nodes[i_mem].type != JSON_OBJECT) return 17;
    int i_mb = json_object_get(&doc, i_mem, "mb");
    if (i_mb < 0) return 18;
    if (doc.nodes[i_mb].type != JSON_NUMBER) return 19;
    if (doc.nodes[i_mb].inum != 512) return 20;

    /* Out-of-range / wrong-type guards must return -1. */
    if (json_array_get(&doc, i_tags, 2) != -1) return 21;
    if (json_object_get(&doc, root, "missing") != -1) return 22;
    if (json_array_len(&doc, i_mem) != -1) return 23;

    return 0;
}

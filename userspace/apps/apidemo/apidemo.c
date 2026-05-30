/*
 * apidemo.c -- end-to-end HTTP/HTTPS + JSON demo for AutomationOS (ring 3).
 * ==========================================================================
 *
 * FREESTANDING userspace: NO libc, NO stdio, NO malloc, NO standard headers.
 * Everything is inline syscalls + tiny self-contained helpers + fixed static
 * buffers. Linked with crt0 (provides _start -> main) and the net + json
 * libraries compiled separately.
 *
 * Usage:
 *   apidemo URL
 *     -- Parse the URL (http://HOST[:PORT]/PATH or https://HOST[:PORT]/PATH,
 *        default ports 80/443). Fetch via http_get or https_get into a static
 *        buffer. Print "HTTP <status>, <n> bytes". If the body looks like JSON
 *        (starts with '{' or '[') parse it with the json library and
 *        pretty-print the top-level structure: object keys + scalar values,
 *        arrays show element count; otherwise print the first ~1KB of the body
 *        raw.
 *
 *   apidemo  (argc <= 1)
 *     -- Deterministic self-test: NO network. Builds a static JSON string,
 *        parses it, verifies name=="AutomationOS", ver==1, tags length==2,
 *        then prints "APIDEMO SELFTEST: PASS" or "APIDEMO SELFTEST: FAIL".
 *        Returns 0 on pass.
 *
 * With the TLS stack working:
 *   apidemo https://host/path.json
 * fetches and parses real JSON API data from HTTPS-only endpoints.
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/apidemo/apidemo.c -o apidemo.o
 *   objdump -d apidemo.o | grep 'fs:0x28'   # must produce no output
 */

#include "../../lib/net/http.h"
#include "../../lib/net/dns.h"
#include "../../lib/json/json.h"

/* -------------------------------------------------------------------------
 * Syscall numbers.
 * ---------------------------------------------------------------------- */
#define SYS_EXIT  0
#define SYS_WRITE 3

/* -------------------------------------------------------------------------
 * 6-argument inline syscall wrapper.
 * rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6.
 * ---------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Minimal string / output helpers (no libc).
 * ---------------------------------------------------------------------- */
static unsigned long d_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void d_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int d_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* Slice compare: does the raw (possibly non-NUL-terminated) slice [s,slen)
 * decode-equal to the NUL-terminated key `k`? Used for unescaped string
 * matching without json_unescape overhead in the simple selftest path. */
static int d_slice_eq(const char *s, unsigned long slen, const char *k) {
    unsigned long kl = d_strlen(k);
    if (slen != kl) return 0;
    for (unsigned long i = 0; i < slen; i++)
        if (s[i] != k[i]) return 0;
    return 1;
}

/* Case-insensitive prefix test. */
static int d_starts_ci(const char *s, const char *pfx) {
    int i = 0;
    for (; pfx[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != pfx[i]) return 0;
    }
    return 1;
}

/* Write NUL-terminated string to fd 1. */
static void out(const char *s) {
    sc(SYS_WRITE, 1, (long)s, (long)d_strlen(s), 0, 0);
}

/* Write exactly n bytes to fd 1. */
static void out_raw(const char *s, long n) {
    long off = 0;
    while (off < n) {
        long w = sc(SYS_WRITE, 1, (long)(s + off), (long)(n - off), 0, 0);
        if (w <= 0) break;
        off += w;
    }
}

/* Append unsigned decimal into buf[cap] at *pos, NUL-terminate. */
static void put_udec(char *buf, int cap, int *pos, unsigned long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < cap - 1) buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

/* Print unsigned decimal directly to fd 1. */
static void out_udec(unsigned long v) {
    char buf[32];
    int pos = 0;
    put_udec(buf, (int)sizeof(buf), &pos, v);
    out(buf);
}

/* Print signed decimal to fd 1. */
static void out_sdec(long long v) {
    if (v < 0) { out("-"); out_udec((unsigned long long)(-v)); }
    else         out_udec((unsigned long long)v);
}

/* Print a double as integer (truncated) -- enough for typical API numerics. */
static void out_double(double d) {
    /* For the pretty-printer: print integer part, then up to 6 decimal places
     * if there is a fractional component.  No libc printf needed. */
    int neg = (d < 0.0);
    if (neg) { out("-"); d = -d; }
    /* Integer part */
    unsigned long long ipart = (unsigned long long)d;
    double fpart = d - (double)ipart;
    out_udec(ipart);
    /* Fractional part (up to 6 digits, strip trailing zeros). */
    if (fpart > 0.0000001) {
        char fbuf[10];
        fbuf[0] = '.';
        int fi = 1;
        for (int i = 0; i < 6 && fi < 8; i++) {
            fpart *= 10.0;
            int digit = (int)fpart;
            fbuf[fi++] = (char)('0' + digit);
            fpart -= (double)digit;
        }
        /* Strip trailing zeros. */
        while (fi > 2 && fbuf[fi - 1] == '0') fi--;
        fbuf[fi] = '\0';
        out(fbuf);
    }
}

/* Print `depth` levels of two-space indent. */
static void out_indent(int depth) {
    for (int i = 0; i < depth; i++) out("  ");
}

/* -------------------------------------------------------------------------
 * URL parser.
 *
 * Accepts:
 *   https://HOST[:PORT]/PATH   TLS, default 443
 *   http://HOST[:PORT]/PATH    plain, default 80
 *   HOST[:PORT]/PATH           no scheme -> HTTP
 *   HOST[:PORT]                bare host -> path "/"
 *
 * Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------------- */
static int url_parse(const char *url,
                     char *host,  int hostcap,
                     unsigned short *port,
                     char *path,  int pathcap,
                     int *is_https) {
    const char *p = url;
    if (!url || !url[0]) return -1;

    *is_https = 0;
    unsigned short defport = 80;

    if (d_starts_ci(p, "https://")) { *is_https = 1; defport = 443; p += 8; }
    else if (d_starts_ci(p, "http://"))              {               p += 7; }

    /* HOST */
    int hi = 0;
    while (*p && *p != ':' && *p != '/') {
        if (hi < hostcap - 1) host[hi++] = *p;
        p++;
    }
    host[hi] = '\0';
    if (hi == 0) return -1;

    /* Optional :PORT */
    unsigned short pt = defport;
    if (*p == ':') {
        p++;
        unsigned long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned long)(*p - '0'); digits++; p++; }
        if (digits == 0 || v == 0 || v > 65535) return -1;
        pt = (unsigned short)v;
    }
    *port = pt;

    /* PATH */
    if (*p == '/') {
        int pi = 0;
        while (*p) { if (pi < pathcap - 1) path[pi++] = *p; p++; }
        path[pi] = '\0';
    } else if (*p == '\0') {
        d_strlcpy(path, "/", pathcap);
    } else {
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * JSON pretty-printer.
 *
 * Walks the node tree rooted at `idx` and prints the top-level structure:
 *   - Objects: print each key + scalar value on its own indented line;
 *     for nested objects/arrays recurse one more level showing the type.
 *   - Arrays:  print "[N elements]" for the count, then each scalar element
 *     on its own line up to a small cap.
 *   - Scalars: print the value inline.
 *
 * depth controls indentation (two spaces per level).
 * max_depth caps recursion so a deeply nested document doesn't overflow.
 * ---------------------------------------------------------------------- */
#define PRETTY_MAX_DEPTH 6
#define PRETTY_MAX_ELEMS 32   /* cap array printing to this many elements */

static void json_print_scalar(const json_doc *doc, int idx) {
    const json_node *n = &doc->nodes[idx];
    switch (n->type) {
    case JSON_NULL:
        out("null");
        break;
    case JSON_BOOL:
        out(n->inum ? "true" : "false");
        break;
    case JSON_NUMBER:
        /* If it looks like an integer, print without decimals. */
        if (n->dnum == (double)n->inum)
            out_sdec(n->inum);
        else
            out_double(n->dnum);
        break;
    case JSON_STRING: {
        /* Decode the raw slice (handles JSON escapes). */
        char sbuf[512];
        int r = json_unescape(doc, idx, sbuf, (unsigned long)sizeof(sbuf));
        out("\"");
        if (r >= 0) out(sbuf);
        else        out("?");
        out("\"");
        break;
    }
    default:
        break;
    }
}

static void json_pretty(const json_doc *doc, int idx, int depth);

static void json_pretty_object(const json_doc *doc, int idx, int depth) {
    const json_node *obj = &doc->nodes[idx];
    int child = obj->first_child;
    if (child == -1) {
        out("{}");
        return;
    }
    out("{\n");
    while (child != -1) {
        const json_node *m = &doc->nodes[child];
        out_indent(depth + 1);
        /* Print key (decode slice). */
        char kbuf[256];
        if (m->klen < (unsigned long)sizeof(kbuf)) {
            /* Manual copy + NUL for the key slice. */
            for (unsigned long i = 0; i < m->klen; i++) kbuf[i] = m->key[i];
            kbuf[m->klen] = '\0';
        } else {
            d_strlcpy(kbuf, "(key-too-long)", (int)sizeof(kbuf));
        }
        out("\""); out(kbuf); out("\": ");
        json_pretty(doc, child, depth + 1);
        out("\n");
        child = m->next_sibling;
    }
    out_indent(depth);
    out("}");
}

static void json_pretty_array(const json_doc *doc, int idx, int depth) {
    int len = json_array_len(doc, idx);
    if (len == 0) { out("[] (0 elements)"); return; }

    char nbuf[32];
    int np = 0;
    put_udec(nbuf, (int)sizeof(nbuf), &np, (unsigned long)len);
    out("["); out(nbuf); out(len == 1 ? " element" : " elements");

    /* For arrays at a shallow depth, also enumerate scalars. */
    if (depth < PRETTY_MAX_DEPTH) {
        out(": ");
        int shown = 0;
        int child = doc->nodes[idx].first_child;
        while (child != -1 && shown < PRETTY_MAX_ELEMS) {
            const json_node *cn = &doc->nodes[child];
            if (cn->type == JSON_STRING || cn->type == JSON_NUMBER ||
                cn->type == JSON_BOOL   || cn->type == JSON_NULL) {
                if (shown > 0) out(", ");
                json_print_scalar(doc, child);
                shown++;
            } else {
                /* Nested container -- just note its type. */
                if (shown > 0) out(", ");
                out(cn->type == JSON_ARRAY ? "[...]" : "{...}");
                shown++;
            }
            child = cn->next_sibling;
        }
        if (shown < len) out(", ...");
    }
    out("]");
}

static void json_pretty(const json_doc *doc, int idx, int depth) {
    if (idx < 0 || idx >= doc->used) { out("?"); return; }
    const json_node *n = &doc->nodes[idx];
    if (depth >= PRETTY_MAX_DEPTH) {
        /* Just show type tag. */
        if (n->type == JSON_OBJECT) out("{...}");
        else if (n->type == JSON_ARRAY) out("[...]");
        else json_print_scalar(doc, idx);
        return;
    }
    switch (n->type) {
    case JSON_OBJECT: json_pretty_object(doc, idx, depth); break;
    case JSON_ARRAY:  json_pretty_array(doc, idx, depth);  break;
    default:          json_print_scalar(doc, idx);         break;
    }
}

/* -------------------------------------------------------------------------
 * Static buffers (off the small user stack).
 * ---------------------------------------------------------------------- */
#define BODY_CAP  (256 * 1024)   /* 256 KB fetch buffer */
#define NODE_CAP  4096            /* JSON node pool      */

static char       g_body[BODY_CAP];
static char       g_host[256];
static char       g_path[2048];
static json_node  g_pool[NODE_CAP];

/* -------------------------------------------------------------------------
 * fetch_and_show -- parse URL, fetch, print status + body / JSON tree.
 * Returns 0 on success, 1 on error.
 * ---------------------------------------------------------------------- */
static int fetch_and_show(const char *url) {
    unsigned short port = 80;
    int is_https = 0;

    int pr = url_parse(url,
                       g_host, (int)sizeof(g_host),
                       &port,
                       g_path, (int)sizeof(g_path),
                       &is_https);
    if (pr != 0) {
        out("apidemo: bad URL: "); out(url); out("\n");
        return 1;
    }

    int status = 0;
    long n;
    if (is_https) {
        n = https_get(g_host, port, g_path, g_body, (unsigned long)BODY_CAP, &status);
    } else {
        n = http_get(g_host, port, g_path, g_body, (unsigned long)BODY_CAP, &status);
    }

    if (n < 0) {
        out("apidemo: request failed (");
        out(is_https ? "https://" : "http://");
        out(g_host); out(g_path); out(")\n");
        return 1;
    }

    /* Print status line: "HTTP <status>, <n> bytes\n" */
    {
        char line[80];
        int pos = 0;
        d_strlcpy(line, "HTTP ", (int)sizeof(line));
        pos = (int)d_strlen(line);
        put_udec(line, (int)sizeof(line), &pos, (unsigned long)status);
        if (pos < (int)sizeof(line) - 3) { line[pos++] = ','; line[pos++] = ' '; line[pos] = '\0'; }
        put_udec(line, (int)sizeof(line), &pos, (unsigned long)n);
        d_strlcpy(line + pos, " bytes\n", (int)sizeof(line) - pos);
        out(line);
    }

    if (n == 0) return 0;

    /* Decide: JSON or raw? */
    char first = g_body[0];
    if (first == '{' || first == '[') {
        /* Attempt JSON parse. */
        json_doc doc;
        int root = json_parse(&doc, g_pool, NODE_CAP, g_body, (unsigned long)n);
        if (root < 0) {
            /* Parse error: fall through to raw display. */
            out("apidemo: JSON parse error: ");
            out(doc.err ? doc.err : "unknown");
            out("\n");
            out("--- raw body (first 1024 bytes) ---\n");
            long show = n < 1024 ? n : 1024;
            out_raw(g_body, show);
            if (show < n) out("\n[...truncated]");
            out("\n");
        } else {
            out("--- JSON ---\n");
            json_pretty(&doc, root, 0);
            out("\n");
        }
    } else {
        /* Raw: print first 1024 bytes. */
        long show = n < 1024 ? n : 1024;
        out("--- body (first 1024 bytes) ---\n");
        out_raw(g_body, show);
        if (show < n) out("\n[...truncated]");
        out("\n");
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * selftest -- deterministic, NO network, NO syscalls (except out).
 *
 * Builds a static JSON string, parses it, verifies:
 *   name == "AutomationOS"
 *   ver  == 1
 *   tags array length == 2
 * Prints "APIDEMO SELFTEST: PASS" or "APIDEMO SELFTEST: FAIL <reason>".
 * Returns 0 on pass, 1 on fail.
 * ---------------------------------------------------------------------- */
static int selftest(void) {
    /* Static JSON document (must outlive the json_doc). */
    static const char json_src[] =
        "{\"name\":\"AutomationOS\",\"ver\":1,\"net\":true,"
        "\"tags\":[\"os\",\"x86_64\"]}";

    static json_node st_pool[64];
    json_doc doc;

    int root = json_parse(&doc, st_pool, 64,
                          json_src, (unsigned long)(sizeof(json_src) - 1));
    if (root < 0) {
        out("APIDEMO SELFTEST: FAIL json_parse returned error: ");
        out(doc.err ? doc.err : "null");
        out("\n");
        return 1;
    }

    /* Verify doc is an object. */
    if (doc.nodes[root].type != JSON_OBJECT) {
        out("APIDEMO SELFTEST: FAIL root not JSON_OBJECT\n");
        return 1;
    }

    /* --- Check name == "AutomationOS" --- */
    int name_idx = json_object_get(&doc, root, "name");
    if (name_idx < 0) {
        out("APIDEMO SELFTEST: FAIL key 'name' not found\n");
        return 1;
    }
    if (doc.nodes[name_idx].type != JSON_STRING) {
        out("APIDEMO SELFTEST: FAIL 'name' not a string\n");
        return 1;
    }
    {
        char nbuf[64];
        int r = json_unescape(&doc, name_idx, nbuf, (unsigned long)sizeof(nbuf));
        if (r < 0) {
            out("APIDEMO SELFTEST: FAIL json_unescape(name) failed\n");
            return 1;
        }
        if (!d_streq(nbuf, "AutomationOS")) {
            out("APIDEMO SELFTEST: FAIL name != 'AutomationOS' (got '");
            out(nbuf); out("')\n");
            return 1;
        }
    }

    /* --- Check ver == 1 --- */
    int ver_idx = json_object_get(&doc, root, "ver");
    if (ver_idx < 0) {
        out("APIDEMO SELFTEST: FAIL key 'ver' not found\n");
        return 1;
    }
    if (doc.nodes[ver_idx].type != JSON_NUMBER) {
        out("APIDEMO SELFTEST: FAIL 'ver' not a number\n");
        return 1;
    }
    if (doc.nodes[ver_idx].inum != 1) {
        out("APIDEMO SELFTEST: FAIL ver != 1 (got ");
        out_sdec(doc.nodes[ver_idx].inum); out(")\n");
        return 1;
    }

    /* --- Check net == true (bonus: verify BOOL works) --- */
    int net_idx = json_object_get(&doc, root, "net");
    if (net_idx < 0) {
        out("APIDEMO SELFTEST: FAIL key 'net' not found\n");
        return 1;
    }
    if (doc.nodes[net_idx].type != JSON_BOOL || doc.nodes[net_idx].inum != 1) {
        out("APIDEMO SELFTEST: FAIL 'net' not true\n");
        return 1;
    }

    /* --- Check tags array length == 2 --- */
    int tags_idx = json_object_get(&doc, root, "tags");
    if (tags_idx < 0) {
        out("APIDEMO SELFTEST: FAIL key 'tags' not found\n");
        return 1;
    }
    if (doc.nodes[tags_idx].type != JSON_ARRAY) {
        out("APIDEMO SELFTEST: FAIL 'tags' not an array\n");
        return 1;
    }
    int tlen = json_array_len(&doc, tags_idx);
    if (tlen != 2) {
        out("APIDEMO SELFTEST: FAIL tags.length != 2 (got ");
        out_sdec(tlen); out(")\n");
        return 1;
    }

    /* Optionally verify tags[0]=="os", tags[1]=="x86_64". */
    int t0 = json_array_get(&doc, tags_idx, 0);
    int t1 = json_array_get(&doc, tags_idx, 1);
    if (t0 < 0 || doc.nodes[t0].type != JSON_STRING ||
        !d_slice_eq(doc.nodes[t0].str, doc.nodes[t0].slen, "os")) {
        out("APIDEMO SELFTEST: FAIL tags[0] != 'os'\n");
        return 1;
    }
    if (t1 < 0 || doc.nodes[t1].type != JSON_STRING ||
        !d_slice_eq(doc.nodes[t1].str, doc.nodes[t1].slen, "x86_64")) {
        out("APIDEMO SELFTEST: FAIL tags[1] != 'x86_64'\n");
        return 1;
    }

    out("APIDEMO SELFTEST: PASS\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * main -- crt0 turns the return value into SYS_EXIT.
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (argc <= 1) {
        return selftest();
    }
    return fetch_and_show(argv[1]);
}

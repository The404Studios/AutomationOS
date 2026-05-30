/* userspace/lib/html/html_parse.c
 * ============================================================================
 * HTML5-subset tokenizer + tree builder.
 *
 * FREESTANDING ring-3 -- no libc/stdio/standard headers; uses only
 * userspace/libc/{malloc.h, string.h}.
 *
 * The DOM contract this file codes against (must match ../dom/dom.h that
 * the architect track is writing concurrently):
 *
 *     enum { DOM_NODE_DOCUMENT, DOM_NODE_ELEMENT, DOM_NODE_TEXT, DOM_NODE_COMMENT };
 *     struct dom_node {
 *         int type;
 *         struct dom_node *parent, *first_child, *last_child,
 *                          *prev_sibling, *next_sibling;
 *         char *tag;
 *         struct dom_attr *attrs;
 *         char *text;
 *         void *user;
 *     };
 *     struct dom_attr { char *name; char *value; struct dom_attr *next; };
 *     struct dom_document { struct dom_node *root; char *url; };
 *     struct dom_document *dom_document_new(void);
 *     struct dom_node *dom_create_element(const char *tag);
 *     struct dom_node *dom_create_text(const char *text);
 *     struct dom_node *dom_create_comment(const char *text);
 *     void dom_append_child(struct dom_node *parent, struct dom_node *child);
 *     void dom_set_attribute(struct dom_node *el, const char *name,
 *                            const char *value);
 *
 * Anything the dom.h author adds on top (free helpers, query helpers) is
 * fine; we only depend on the above seven calls.
 *
 * See html_parse.h for the supported tag/entity subset and the explicit list
 * of HTML5 features we drop.
 * ============================================================================
 */

#include "html_parse.h"
#include "../../libc/malloc.h"
#include "../../libc/string.h"

/* ------------------------------------------------------------------ */
/* Small helpers (kept local; the project's libc has these too but    */
/* we don't want to pull more than we need).                          */
/* ------------------------------------------------------------------ */

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static int ascii_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int ascii_isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ascii_isalnum(int c)
{
    return ascii_isalpha(c) || (c >= '0' && c <= '9');
}

/* dup a fixed-length byte run as a NUL-terminated heap string. */
static char *strndup_heap(const char *p, unsigned long n)
{
    char *out = (char *)malloc(n + 1);
    if (!out) return 0;
    for (unsigned long i = 0; i < n; i++) out[i] = p[i];
    out[n] = 0;
    return out;
}

static char *strdup_heap(const char *p)
{
    if (!p) return 0;
    return strndup_heap(p, strlen(p));
}

/* ------------------------------------------------------------------ */
/* Growable byte buffer (text/comment accumulation, attribute names). */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    unsigned long len;
    unsigned long cap;
} sbuf;

static void sbuf_init(sbuf *b)
{
    b->data = 0;
    b->len = 0;
    b->cap = 0;
}

static int sbuf_grow(sbuf *b, unsigned long need)
{
    if (need <= b->cap) return 1;
    unsigned long nc = b->cap ? b->cap : 32;
    while (nc < need) nc *= 2;
    char *nd = (char *)realloc(b->data, nc);
    if (!nd) return 0;
    b->data = nd;
    b->cap = nc;
    return 1;
}

static int sbuf_putc(sbuf *b, char c)
{
    if (!sbuf_grow(b, b->len + 2)) return 0;
    b->data[b->len++] = c;
    b->data[b->len] = 0;
    return 1;
}

static int sbuf_puts(sbuf *b, const char *s)
{
    while (*s) {
        if (!sbuf_putc(b, *s++)) return 0;
    }
    return 1;
}

static void sbuf_reset(sbuf *b)
{
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

static void sbuf_free(sbuf *b)
{
    if (b->data) free(b->data);
    b->data = 0;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Entity decoding -- writes UTF-8 bytes into `out`, advances *pp.    */
/* Returns bytes written. Falls back to literal '&' on unknown.       */
/* ------------------------------------------------------------------ */

static int emit_utf8(unsigned long cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static int decode_entity(const char **pp, const char *end, char *out /* >=4 */)
{
    const char *p = *pp;            /* points at '&' */
    if (p >= end || *p != '&') { out[0] = '&'; *pp = p + 1; return 1; }
    const char *q = p + 1;

    /* numeric: &#NN; or &#xHH; */
    if (q < end && *q == '#') {
        q++;
        unsigned long val = 0;
        int hex = 0;
        if (q < end && (*q == 'x' || *q == 'X')) { hex = 1; q++; }
        const char *start = q;
        if (hex) {
            while (q < end) {
                char c = *q;
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                val = val * 16 + (unsigned long)d;
                q++;
            }
        } else {
            while (q < end && *q >= '0' && *q <= '9') {
                val = val * 10 + (unsigned long)(*q - '0');
                q++;
            }
        }
        if (q == start) {
            /* malformed: keep '&' literal */
            out[0] = '&';
            *pp = p + 1;
            return 1;
        }
        if (q < end && *q == ';') q++;
        *pp = q;
        return emit_utf8(val, out);
    }

    /* named */
    static const struct { const char *name; unsigned long cp; } ents[] = {
        { "amp",    '&'    },
        { "lt",     '<'    },
        { "gt",     '>'    },
        { "quot",   '"'    },
        { "apos",   '\''   },
        { "nbsp",   0xA0   },
        { "copy",   0xA9   },
        { "reg",    0xAE   },
        { "mdash",  0x2014 },
        { "ndash",  0x2013 },
        { "hellip", 0x2026 },
        { "lsquo",  0x2018 },
        { "rsquo",  0x2019 },
        { "ldquo",  0x201C },
        { "rdquo",  0x201D },
        { "trade",  0x2122 },
        { "middot", 0xB7   },
        { "laquo",  0xAB   },
        { "raquo",  0xBB   },
    };
    for (unsigned i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        const char *name = ents[i].name;
        unsigned long nlen = strlen(name);
        if ((unsigned long)(end - q) < nlen) continue;
        int ok = 1;
        for (unsigned long j = 0; j < nlen; j++) {
            if (q[j] != name[j]) { ok = 0; break; }
        }
        if (!ok) continue;
        /* terminator: ';' preferred; tolerate missing ';' */
        const char *after = q + nlen;
        if (after < end && *after == ';') after++;
        *pp = after;
        return emit_utf8(ents[i].cp, out);
    }

    /* unknown: pass '&' literally */
    out[0] = '&';
    *pp = p + 1;
    return 1;
}

static void append_decoded(sbuf *b, const char *p, const char *end)
{
    char tmp[8];
    while (p < end) {
        if (*p == '&') {
            int n = decode_entity(&p, end, tmp);
            for (int i = 0; i < n; i++) sbuf_putc(b, tmp[i]);
        } else {
            sbuf_putc(b, *p++);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Element classification.                                            */
/* ------------------------------------------------------------------ */

static int is_void_element(const char *tag)
{
    /* HTML5 void elements (subset we handle): br hr img input meta link */
    static const char *vs[] = {
        "br", "hr", "img", "input", "meta", "link", "wbr", "source",
        "track", "area", "base", "col", "embed", "param", 0
    };
    for (int i = 0; vs[i]; i++) if (strcmp(tag, vs[i]) == 0) return 1;
    return 0;
}

static int is_rawtext_element(const char *tag)
{
    return strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}

/* Block-level elements that auto-close a currently-open <p>. (Simplified.) */
static int auto_closes_p(const char *tag)
{
    static const char *list[] = {
        "p", "div", "section", "article", "aside", "header", "footer",
        "nav", "main", "form", "ul", "ol", "li", "table", "tr", "td", "th",
        "h1", "h2", "h3", "h4", "h5", "h6", "pre", "hr", "blockquote", 0
    };
    for (int i = 0; list[i]; i++) if (strcmp(tag, list[i]) == 0) return 1;
    return 0;
}

/* <li> auto-closes a previous <li>; <td>/<th>/<tr> behave similarly inside
 * tables.  We use a tiny rule table.  Returns 1 if `open` should be popped
 * when we are about to insert a new `incoming` open tag. */
static int auto_closes_sibling(const char *open, const char *incoming)
{
    if (strcmp(open, "li") == 0 && strcmp(incoming, "li") == 0) return 1;
    if ((strcmp(open, "td") == 0 || strcmp(open, "th") == 0) &&
        (strcmp(incoming, "td") == 0 || strcmp(incoming, "th") == 0 ||
         strcmp(incoming, "tr") == 0)) return 1;
    if (strcmp(open, "tr") == 0 && strcmp(incoming, "tr") == 0) return 1;
    if (strcmp(open, "option") == 0 &&
        (strcmp(incoming, "option") == 0 ||
         strcmp(incoming, "optgroup") == 0)) return 1;
    if (strcmp(open, "dt") == 0 &&
        (strcmp(incoming, "dt") == 0 || strcmp(incoming, "dd") == 0)) return 1;
    if (strcmp(open, "dd") == 0 &&
        (strcmp(incoming, "dt") == 0 || strcmp(incoming, "dd") == 0)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open-element stack (simple array of dom_node*).                    */
/* ------------------------------------------------------------------ */

typedef struct {
    struct dom_node **a;
    int len;
    int cap;
} estk;

static void estk_init(estk *s) { s->a = 0; s->len = 0; s->cap = 0; }

static int estk_push(estk *s, struct dom_node *n)
{
    if (s->len + 1 > s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        struct dom_node **na = (struct dom_node **)realloc(
            s->a, (unsigned long)nc * sizeof(struct dom_node *));
        if (!na) return 0;
        s->a = na;
        s->cap = nc;
    }
    s->a[s->len++] = n;
    return 1;
}

static struct dom_node *estk_top(estk *s)
{
    return s->len ? s->a[s->len - 1] : 0;
}

static void estk_pop(estk *s)
{
    if (s->len) s->len--;
}

static void estk_free(estk *s)
{
    if (s->a) free(s->a);
    s->a = 0;
    s->len = s->cap = 0;
}

/* Find an open element with the given tag; return its stack index or -1. */
static int estk_find(estk *s, const char *tag)
{
    for (int i = s->len - 1; i >= 0; i--) {
        struct dom_node *n = s->a[i];
        if (n->tag && strcmp(n->tag, tag) == 0) return i;
    }
    return -1;
}

/* Pop the stack down to (and including) index `idx`. */
static void estk_pop_through(estk *s, int idx)
{
    while (s->len > idx) s->len--;
}

/* ------------------------------------------------------------------ */
/* Insertion modes.                                                   */
/* ------------------------------------------------------------------ */

enum {
    IM_INITIAL = 0,
    IM_BEFORE_HTML,
    IM_BEFORE_HEAD,
    IM_IN_HEAD,
    IM_AFTER_HEAD,
    IM_IN_BODY,
    IM_AFTER_BODY
};

/* ------------------------------------------------------------------ */
/* Parser context.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;
    const char *end;
    const char *p;

    struct dom_document *doc;
    struct dom_node *html;     /* <html> */
    struct dom_node *head;     /* <head> */
    struct dom_node *body;     /* <body> */

    estk stack;
    int  mode;

    sbuf text_buf;             /* accumulating text between tags */
} parser;

/* ------------------------------------------------------------------ */
/* Tokenizer primitives.                                              */
/* ------------------------------------------------------------------ */

static void skip_ws(parser *P)
{
    while (P->p < P->end && ascii_isspace((unsigned char)*P->p)) P->p++;
}

/* Read an attribute name into `b` (lowercased). Stops at '=', whitespace,
 * '>', '/' or EOF. */
static void read_attr_name(parser *P, sbuf *b)
{
    sbuf_reset(b);
    while (P->p < P->end) {
        char c = *P->p;
        if (c == '=' || c == '>' || c == '/' || ascii_isspace((unsigned char)c)) break;
        sbuf_putc(b, (char)ascii_tolower((unsigned char)c));
        P->p++;
    }
}

/* Read an attribute value into `b`. Handles "..." / '...' / unquoted. */
static void read_attr_value(parser *P, sbuf *b)
{
    sbuf_reset(b);
    if (P->p >= P->end) return;
    char q = *P->p;
    if (q == '"' || q == '\'') {
        P->p++;
        const char *start = P->p;
        while (P->p < P->end && *P->p != q) P->p++;
        /* decode entities while copying */
        append_decoded(b, start, P->p);
        if (P->p < P->end) P->p++;   /* skip closing quote */
    } else {
        const char *start = P->p;
        while (P->p < P->end) {
            char c = *P->p;
            if (ascii_isspace((unsigned char)c) || c == '>' || c == '/') break;
            P->p++;
        }
        append_decoded(b, start, P->p);
    }
}

/* ------------------------------------------------------------------ */
/* Tree builder helpers.                                              */
/* ------------------------------------------------------------------ */

/* Flush accumulated text into a TEXT node under the current insertion point. */
static void flush_text(parser *P)
{
    if (P->text_buf.len == 0) return;
    struct dom_node *parent = estk_top(&P->stack);
    if (!parent) {
        /* Before <html> -- create implicit <html> + <body>. */
        sbuf_reset(&P->text_buf);
        return;
    }
    struct dom_node *t = dom_create_text(P->text_buf.data);
    if (t) dom_append_child(parent, t);
    sbuf_reset(&P->text_buf);
}

/* Ensure <html>, <head>, <body> are created and on the stack as appropriate.
 * Called lazily when content forces us into a particular mode. */
static void ensure_html(parser *P)
{
    if (P->html) return;
    P->html = dom_create_element("html");
    if (!P->html) return;
    dom_append_child(P->doc->root, P->html);
    estk_push(&P->stack, P->html);
}

static void ensure_head(parser *P)
{
    ensure_html(P);
    if (P->head) return;
    P->head = dom_create_element("head");
    if (!P->head) return;
    dom_append_child(P->html, P->head);
}

static void ensure_body(parser *P)
{
    ensure_html(P);
    if (P->body) return;
    P->body = dom_create_element("body");
    if (!P->body) return;
    dom_append_child(P->html, P->body);
    /* Pop anything above <html>, then push <body>. */
    while (estk_top(&P->stack) && estk_top(&P->stack) != P->html) estk_pop(&P->stack);
    estk_push(&P->stack, P->body);
    P->mode = IM_IN_BODY;
}

/* Insert an element node under the current open element and (if not void)
 * push it onto the stack. */
static struct dom_node *insert_element(parser *P, const char *tag, int self_closing)
{
    flush_text(P);

    /* Open-implied head/body. */
    if (strcmp(tag, "html") == 0) {
        if (!P->html) {
            P->html = dom_create_element("html");
            dom_append_child(P->doc->root, P->html);
            estk_push(&P->stack, P->html);
        }
        P->mode = IM_BEFORE_HEAD;
        return P->html;
    }
    if (strcmp(tag, "head") == 0) {
        ensure_html(P);
        if (!P->head) {
            P->head = dom_create_element("head");
            dom_append_child(P->html, P->head);
        }
        estk_push(&P->stack, P->head);
        P->mode = IM_IN_HEAD;
        return P->head;
    }
    if (strcmp(tag, "body") == 0) {
        ensure_html(P);
        if (!P->body) {
            P->body = dom_create_element("body");
            dom_append_child(P->html, P->body);
        }
        /* Pop down to <html>, then push body. */
        while (estk_top(&P->stack) && estk_top(&P->stack) != P->html) estk_pop(&P->stack);
        estk_push(&P->stack, P->body);
        P->mode = IM_IN_BODY;
        return P->body;
    }

    /* Anything else seen before <body> opens implies <body>. */
    if (P->mode == IM_INITIAL || P->mode == IM_BEFORE_HTML ||
        P->mode == IM_BEFORE_HEAD || P->mode == IM_AFTER_HEAD ||
        P->mode == IM_AFTER_BODY) {
        /* Tags that BELONG in <head> stay there. */
        static const char *head_tags[] = {
            "title", "meta", "link", "style", "script", "base", 0
        };
        int in_head = 0;
        for (int i = 0; head_tags[i]; i++) {
            if (strcmp(tag, head_tags[i]) == 0) { in_head = 1; break; }
        }
        if (in_head) {
            ensure_head(P);
            /* Push head if not already on stack (so children attach there). */
            if (estk_top(&P->stack) != P->head) estk_push(&P->stack, P->head);
            P->mode = IM_IN_HEAD;
        } else {
            ensure_body(P);
        }
    }

    /* Auto-close <p> for block-level openers. */
    {
        int idx = estk_find(&P->stack, "p");
        if (idx >= 0 && auto_closes_p(tag)) {
            estk_pop_through(&P->stack, idx);
        }
    }
    /* Sibling auto-close (li/li, td/td, ...). */
    {
        struct dom_node *top = estk_top(&P->stack);
        if (top && top->tag && auto_closes_sibling(top->tag, tag)) {
            estk_pop(&P->stack);
        }
    }

    struct dom_node *parent = estk_top(&P->stack);
    if (!parent) {
        ensure_body(P);
        parent = estk_top(&P->stack);
    }

    struct dom_node *el = dom_create_element(tag);
    if (!el) return 0;
    dom_append_child(parent, el);

    if (!self_closing && !is_void_element(tag)) {
        estk_push(&P->stack, el);
    }
    return el;
}

/* Close the nearest open element with the given tag, popping the stack. */
static void close_element(parser *P, const char *tag)
{
    flush_text(P);

    if (strcmp(tag, "html") == 0) {
        /* Pop everything; transition to AFTER_BODY. */
        while (estk_top(&P->stack)) estk_pop(&P->stack);
        P->mode = IM_AFTER_BODY;
        return;
    }
    if (strcmp(tag, "body") == 0) {
        /* Pop down to <html>. */
        while (estk_top(&P->stack) && estk_top(&P->stack) != P->html) estk_pop(&P->stack);
        P->mode = IM_AFTER_BODY;
        return;
    }
    if (strcmp(tag, "head") == 0) {
        if (estk_top(&P->stack) == P->head) estk_pop(&P->stack);
        P->mode = IM_AFTER_HEAD;
        return;
    }

    /* Misnested forgiveness: pop until match or stack hits a "scope barrier".
     * We use <html>/<body> as scope barriers (don't ever pop them implicitly). */
    int idx = estk_find(&P->stack, tag);
    if (idx < 0) return;   /* stray end tag */
    /* Don't pop above <body>. */
    int barrier = 0;
    for (int i = 0; i < P->stack.len; i++) {
        struct dom_node *n = P->stack.a[i];
        if (n == P->body || n == P->html) barrier = i + 1;
    }
    if (idx < barrier) return;
    estk_pop_through(&P->stack, idx);
}

/* ------------------------------------------------------------------ */
/* Raw-text (<script>, <style>) consumption.                          */
/* ------------------------------------------------------------------ */

static void consume_rawtext(parser *P, const char *close_tag, struct dom_node *el)
{
    /* Find "</close_tag" (case-insensitive). Everything before it is one
     * TEXT node child of `el`. */
    unsigned long ctlen = strlen(close_tag);
    const char *start = P->p;
    while (P->p < P->end) {
        if (*P->p == '<' && P->p + 1 < P->end && P->p[1] == '/') {
            const char *q = P->p + 2;
            if ((unsigned long)(P->end - q) >= ctlen) {
                int ok = 1;
                for (unsigned long i = 0; i < ctlen; i++) {
                    if (ascii_tolower((unsigned char)q[i]) != close_tag[i]) { ok = 0; break; }
                }
                if (ok) {
                    char after = (q + ctlen < P->end) ? q[ctlen] : 0;
                    if (after == '>' || after == ' ' || after == '\t' ||
                        after == '\n' || after == '\r' || after == 0) {
                        /* Found close. */
                        if (P->p > start) {
                            char *body = strndup_heap(start, (unsigned long)(P->p - start));
                            if (body) {
                                struct dom_node *t = dom_create_text(body);
                                if (t) dom_append_child(el, t);
                                free(body);
                            }
                        }
                        /* Skip "</close_tag" + any space + ">" */
                        P->p = q + ctlen;
                        while (P->p < P->end && *P->p != '>') P->p++;
                        if (P->p < P->end) P->p++;
                        return;
                    }
                }
            }
        }
        P->p++;
    }
    /* EOF before close: emit what we have as text. */
    if (P->p > start) {
        char *body = strndup_heap(start, (unsigned long)(P->p - start));
        if (body) {
            struct dom_node *t = dom_create_text(body);
            if (t) dom_append_child(el, t);
            free(body);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Top-level tokenize+build loop.                                     */
/* ------------------------------------------------------------------ */

static void parse_loop(parser *P)
{
    sbuf tagname; sbuf_init(&tagname);

    while (P->p < P->end) {
        char c = *P->p;
        if (c == '<') {
            if (P->p + 1 >= P->end) {
                sbuf_putc(&P->text_buf, c);
                P->p++;
                continue;
            }
            char d = P->p[1];

            /* Comment */
            if (d == '!' && P->p + 3 < P->end &&
                P->p[2] == '-' && P->p[3] == '-') {
                flush_text(P);
                P->p += 4;
                const char *cs = P->p;
                while (P->p + 2 < P->end &&
                       !(P->p[0] == '-' && P->p[1] == '-' && P->p[2] == '>')) {
                    P->p++;
                }
                unsigned long clen = (unsigned long)(P->p - cs);
                char *txt = strndup_heap(cs, clen);
                if (txt) {
                    struct dom_node *parent = estk_top(&P->stack);
                    if (!parent) { ensure_body(P); parent = estk_top(&P->stack); }
                    struct dom_node *cn = dom_create_comment(txt);
                    if (cn && parent) dom_append_child(parent, cn);
                    free(txt);
                }
                if (P->p + 2 < P->end) P->p += 3;
                else P->p = P->end;
                continue;
            }
            /* DOCTYPE / unknown <! */
            if (d == '!') {
                flush_text(P);
                while (P->p < P->end && *P->p != '>') P->p++;
                if (P->p < P->end) P->p++;
                continue;
            }
            if (d == '?') {
                while (P->p < P->end && *P->p != '>') P->p++;
                if (P->p < P->end) P->p++;
                continue;
            }
            /* </name> */
            if (d == '/') {
                P->p += 2;
                sbuf_reset(&tagname);
                while (P->p < P->end) {
                    char e = *P->p;
                    if (e == '>' || ascii_isspace((unsigned char)e)) break;
                    if (!ascii_isalnum((unsigned char)e) && e != '-' && e != '_')
                        break;
                    sbuf_putc(&tagname, (char)ascii_tolower((unsigned char)e));
                    P->p++;
                }
                while (P->p < P->end && *P->p != '>') P->p++;
                if (P->p < P->end) P->p++;
                if (tagname.len > 0) {
                    close_element(P, tagname.data);
                }
                continue;
            }
            /* <name ...> */
            if (ascii_isalpha((unsigned char)d)) {
                P->p++;     /* skip '<' */
                sbuf_reset(&tagname);
                while (P->p < P->end) {
                    char e = *P->p;
                    if (e == '>' || e == '/' || ascii_isspace((unsigned char)e)) break;
                    if (!ascii_isalnum((unsigned char)e) && e != '-' && e != '_')
                        break;
                    sbuf_putc(&tagname, (char)ascii_tolower((unsigned char)e));
                    P->p++;
                }
                if (tagname.len == 0) {
                    sbuf_putc(&P->text_buf, '<');
                    continue;
                }
                /* Tentatively decide whether this tag will be self-closing
                 * by scanning attrs.  We create the element after parsing
                 * attrs into a small buffer chain. To keep it simple we
                 * create the element NOW and feed attrs into it directly,
                 * then ALSO note self-closing. */
                flush_text(P);
                /* Pre-create the element so we have somewhere to attach
                 * attributes; but we don't push it on the stack until
                 * after self-closing is known.  We do this by inserting
                 * lazily inside read_attributes_into. */

                /* Build attribute key/value lists in a temp container. */
                sbuf an; sbuf_init(&an);
                sbuf av; sbuf_init(&av);
                struct kv { char *k; char *v; struct kv *next; } *kvs = 0, *kvs_tail = 0;
                int self_closing = 0;

                while (P->p < P->end) {
                    skip_ws(P);
                    if (P->p >= P->end) break;
                    char x = *P->p;
                    if (x == '>') { P->p++; break; }
                    if (x == '/') { self_closing = 1; P->p++; continue; }
                    read_attr_name(P, &an);
                    if (an.len == 0) { P->p++; continue; }
                    skip_ws(P);
                    sbuf_reset(&av);
                    if (P->p < P->end && *P->p == '=') {
                        P->p++;
                        skip_ws(P);
                        read_attr_value(P, &av);
                    }
                    struct kv *e = (struct kv *)malloc(sizeof(struct kv));
                    if (e) {
                        e->k = strdup_heap(an.data ? an.data : "");
                        e->v = strdup_heap(av.data ? av.data : "");
                        e->next = 0;
                        if (kvs_tail) kvs_tail->next = e;
                        else kvs = e;
                        kvs_tail = e;
                    }
                    sbuf_reset(&an);
                }
                sbuf_free(&an);
                sbuf_free(&av);

                struct dom_node *el = insert_element(P, tagname.data, self_closing);

                /* Attach attributes to the element. */
                if (el) {
                    for (struct kv *e = kvs; e; e = e->next) {
                        dom_set_attribute(el, e->k ? e->k : "", e->v ? e->v : "");
                    }
                }
                /* Free kv list. */
                while (kvs) {
                    struct kv *n = kvs->next;
                    if (kvs->k) free(kvs->k);
                    if (kvs->v) free(kvs->v);
                    free(kvs);
                    kvs = n;
                }

                /* Raw-text consume for script/style. */
                if (el && !self_closing && is_rawtext_element(tagname.data)) {
                    consume_rawtext(P, tagname.data, el);
                    /* Pop the element (closer was consumed by rawtext). */
                    if (estk_top(&P->stack) == el) estk_pop(&P->stack);
                }
                continue;
            }
            /* Stray '<' */
            sbuf_putc(&P->text_buf, '<');
            P->p++;
            continue;
        }
        if (c == '&') {
            char tmp[8];
            int n = decode_entity(&P->p, P->end, tmp);
            for (int i = 0; i < n; i++) sbuf_putc(&P->text_buf, tmp[i]);
            continue;
        }
        sbuf_putc(&P->text_buf, c);
        P->p++;
    }

    flush_text(P);
    sbuf_free(&tagname);
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

struct dom_document *html_parse(const char *html, unsigned long len)
{
    if (!html) return 0;

    struct dom_document *doc = dom_document_new();
    if (!doc) return 0;

    parser P;
    P.src = html;
    P.end = html + len;
    P.p   = html;
    P.doc = doc;
    P.html = 0;
    P.head = 0;
    P.body = 0;
    estk_init(&P.stack);
    P.mode = IM_INITIAL;
    sbuf_init(&P.text_buf);

    /* Push root onto stack (DOCUMENT). */
    estk_push(&P.stack, doc->root);

    parse_loop(&P);

    /* Ensure body exists even for empty input -- the renderer expects one. */
    ensure_body(&P);

    sbuf_free(&P.text_buf);
    estk_free(&P.stack);
    return doc;
}

struct dom_node *html_parse_fragment(const char *html, unsigned long len)
{
    if (!html) return 0;
    struct dom_node *wrap = dom_create_element("fragment");
    if (!wrap) return 0;

    parser P;
    /* For a fragment we synthesize a fake doc whose root is `wrap`. We
     * still go through the full parser to share code; the `html`/`body`
     * lazy creation in insert_element will trigger, but since we want
     * children directly under `wrap`, we pre-seed body = wrap so any
     * element ends up there. */
    struct dom_document fake;
    fake.root = wrap;
    fake.url = 0;

    P.src = html;
    P.end = html + len;
    P.p   = html;
    P.doc = &fake;
    P.html = wrap;       /* tricks ensure_html into a no-op */
    P.head = wrap;       /* same for head */
    P.body = wrap;       /* and body */
    estk_init(&P.stack);
    P.mode = IM_IN_BODY;
    sbuf_init(&P.text_buf);
    estk_push(&P.stack, wrap);

    parse_loop(&P);

    sbuf_free(&P.text_buf);
    estk_free(&P.stack);
    return wrap;
}

/* ------------------------------------------------------------------ */
/* Script harvesting.                                                  */
/* ------------------------------------------------------------------ */

static const char *attr_value(const struct dom_node *el, const char *name)
{
    if (!el) return 0;
    for (struct dom_attr *a = el->attrs; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0) return a->value ? a->value : "";
    }
    return 0;
}

static void collect_scripts(const struct dom_node *n,
                            char ***srcs_out, int *srcs_cap, int *srcs_len,
                            char ***inl_out,  int *inl_cap,  int *inl_len)
{
    if (!n) return;
    if (n->type == DOM_NODE_ELEMENT && n->tag && strcmp(n->tag, "script") == 0) {
        const char *src = attr_value(n, "src");
        if (src && src[0]) {
            if (*srcs_len + 1 > *srcs_cap) {
                int nc = *srcs_cap ? *srcs_cap * 2 : 4;
                char **na = (char **)realloc(*srcs_out,
                                             (unsigned long)nc * sizeof(char *));
                if (!na) return;
                *srcs_out = na;
                *srcs_cap = nc;
            }
            (*srcs_out)[(*srcs_len)++] = strdup_heap(src);
        } else {
            /* Concatenate text children into one body. */
            sbuf b; sbuf_init(&b);
            for (struct dom_node *c = n->first_child; c; c = c->next_sibling) {
                if (c->type == DOM_NODE_TEXT && c->text) sbuf_puts(&b, c->text);
            }
            if (*inl_len + 1 > *inl_cap) {
                int nc = *inl_cap ? *inl_cap * 2 : 4;
                char **na = (char **)realloc(*inl_out,
                                             (unsigned long)nc * sizeof(char *));
                if (!na) { sbuf_free(&b); return; }
                *inl_out = na;
                *inl_cap = nc;
            }
            (*inl_out)[(*inl_len)++] = b.data ? b.data : strdup_heap("");
            /* don't sbuf_free(): we transferred ownership of b.data */
            b.data = 0;
            sbuf_free(&b);
        }
    }
    for (struct dom_node *c = n->first_child; c; c = c->next_sibling) {
        collect_scripts(c, srcs_out, srcs_cap, srcs_len, inl_out, inl_cap, inl_len);
    }
}

char **html_get_script_srcs(const struct dom_document *doc, int *count_out)
{
    if (count_out) *count_out = 0;
    if (!doc) return 0;
    char **srcs = 0; int sc = 0, sl = 0;
    char **inl = 0;  int ic = 0, il = 0;
    collect_scripts(doc->root, &srcs, &sc, &sl, &inl, &ic, &il);
    /* Free the inline-script list we collected as a side effect. */
    if (inl) {
        for (int i = 0; i < il; i++) if (inl[i]) free(inl[i]);
        free(inl);
    }
    if (count_out) *count_out = sl;
    return srcs;
}

char **html_get_inline_scripts(const struct dom_document *doc, int *count_out)
{
    if (count_out) *count_out = 0;
    if (!doc) return 0;
    char **srcs = 0; int sc = 0, sl = 0;
    char **inl = 0;  int ic = 0, il = 0;
    collect_scripts(doc->root, &srcs, &sc, &sl, &inl, &ic, &il);
    /* Free the script-src list we collected as a side effect. */
    if (srcs) {
        for (int i = 0; i < sl; i++) if (srcs[i]) free(srcs[i]);
        free(srcs);
    }
    if (count_out) *count_out = il;
    return inl;
}

/* ------------------------------------------------------------------ */
/* Self-test.                                                          */
/* ------------------------------------------------------------------ */

static struct dom_node *find_first_element(struct dom_node *root, const char *tag)
{
    if (!root) return 0;
    if (root->type == DOM_NODE_ELEMENT && root->tag && strcmp(root->tag, tag) == 0)
        return root;
    for (struct dom_node *c = root->first_child; c; c = c->next_sibling) {
        struct dom_node *r = find_first_element(c, tag);
        if (r) return r;
    }
    return 0;
}

static int count_elements(struct dom_node *root)
{
    if (!root) return 0;
    int n = (root->type == DOM_NODE_ELEMENT) ? 1 : 0;
    for (struct dom_node *c = root->first_child; c; c = c->next_sibling) {
        n += count_elements(c);
    }
    return n;
}

int html_selftest(void)
{
    /* Test 1: A small structured doc. */
    static const char src[] =
        "<!DOCTYPE html>\n"
        "<html><head><title>T</title></head>"
        "<body>"
        "<h1>Hello</h1>"
        "<p>World &amp; everyone</p>"
        "<ul><li>one<li>two</ul>"
        "<a href=\"https://x\">link</a>"
        "<br>"
        "<script src=\"a.js\"></script>"
        "<script>var x = 1; if (x < 2) {}</script>"
        "<!-- a comment -->"
        "</body></html>";

    struct dom_document *doc = html_parse(src, sizeof(src) - 1);
    if (!doc) return -1;
    if (!doc->root) return -2;

    struct dom_node *html = find_first_element(doc->root, "html");
    if (!html) return -3;
    struct dom_node *head = find_first_element(html, "head");
    if (!head) return -4;
    struct dom_node *body = find_first_element(html, "body");
    if (!body) return -5;

    struct dom_node *h1 = find_first_element(body, "h1");
    if (!h1) return -6;
    /* h1's text child must be "Hello". */
    if (!h1->first_child || h1->first_child->type != DOM_NODE_TEXT) return -7;
    if (strcmp(h1->first_child->text, "Hello") != 0) return -8;

    struct dom_node *p = find_first_element(body, "p");
    if (!p) return -9;
    /* p's text child must decode the entity. */
    if (!p->first_child || p->first_child->type != DOM_NODE_TEXT) return -10;
    if (strcmp(p->first_child->text, "World & everyone") != 0) return -11;

    /* Two <li>s? (li auto-closes li.) */
    struct dom_node *ul = find_first_element(body, "ul");
    if (!ul) return -12;
    int li_count = 0;
    for (struct dom_node *c = ul->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_NODE_ELEMENT && strcmp(c->tag, "li") == 0) li_count++;
    }
    if (li_count != 2) return -13;

    /* anchor with href */
    struct dom_node *a = find_first_element(body, "a");
    if (!a) return -14;
    const char *href = attr_value(a, "href");
    if (!href || strcmp(href, "https://x") != 0) return -15;

    /* Script src harvest. */
    int sc = 0;
    char **srcs = html_get_script_srcs(doc, &sc);
    if (sc != 1) return -16;
    if (!srcs || !srcs[0] || strcmp(srcs[0], "a.js") != 0) return -17;
    for (int i = 0; i < sc; i++) free(srcs[i]);
    free(srcs);

    /* Inline scripts: exactly one (the second <script>). */
    int ic = 0;
    char **inl = html_get_inline_scripts(doc, &ic);
    if (ic != 1) return -18;
    /* Body should contain "x < 2" verbatim since rawtext doesn't entity-decode. */
    if (!inl || !inl[0] || !strstr(inl[0], "x < 2")) return -19;
    for (int i = 0; i < ic; i++) free(inl[i]);
    free(inl);

    /* Comment captured. */
    int found_comment = 0;
    for (struct dom_node *c = body->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_NODE_COMMENT) { found_comment = 1; break; }
    }
    if (!found_comment) return -20;

    /* Test 2: void element doesn't get pushed. */
    static const char src2[] = "<body><br>after</body>";
    struct dom_document *doc2 = html_parse(src2, sizeof(src2) - 1);
    if (!doc2) return -21;
    struct dom_node *body2 = find_first_element(doc2->root, "body");
    if (!body2) return -22;
    /* "after" should be a text sibling of <br>, not a child. */
    int saw_text_after_br = 0;
    int seen_br = 0;
    for (struct dom_node *c = body2->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_NODE_ELEMENT && strcmp(c->tag, "br") == 0) seen_br = 1;
        else if (seen_br && c->type == DOM_NODE_TEXT &&
                 strstr(c->text, "after")) saw_text_after_br = 1;
    }
    if (!saw_text_after_br) return -23;

    /* Test 3: numeric entity. */
    static const char src3[] = "<p>A&#65;B</p>";
    struct dom_document *doc3 = html_parse(src3, sizeof(src3) - 1);
    if (!doc3) return -24;
    struct dom_node *p3 = find_first_element(doc3->root, "p");
    if (!p3 || !p3->first_child) return -25;
    if (strcmp(p3->first_child->text, "AAB") != 0) return -26;

    /* Test 4: at least N elements (sanity). */
    int n = count_elements(doc->root);
    if (n < 8) return -27;

    /* ------------------------------------------------------------------ */
    /* Test 5: &apos; and &#39; entity decoding in text content.          */
    /* Both represent ASCII 39 (single-quote / apostrophe).               */
    /* ------------------------------------------------------------------ */
    static const char src5[] = "<p>it&apos;s &#39;ok&#39;</p>";
    struct dom_document *doc5 = html_parse(src5, sizeof(src5) - 1);
    if (!doc5) return -28;
    struct dom_node *p5 = find_first_element(doc5->root, "p");
    if (!p5 || !p5->first_child) return -29;
    if (strcmp(p5->first_child->text, "it's 'ok'") != 0) return -30;

    /* Test 5b: &apos; and &quot; in an attribute value. */
    static const char src5b[] = "<div title=\"say &apos;hi&apos;\"></div>";
    struct dom_document *doc5b = html_parse(src5b, sizeof(src5b) - 1);
    if (!doc5b) return -31;
    struct dom_node *div5b = find_first_element(doc5b->root, "div");
    if (!div5b) return -32;
    {
        const char *tv = attr_value(div5b, "title");
        if (!tv || strcmp(tv, "say 'hi'") != 0) return -33;
    }

    /* ------------------------------------------------------------------ */
    /* Test 6: boolean attribute (no "=value" part) → stored with         */
    /* an empty-string value, element still parses correctly.             */
    /* ------------------------------------------------------------------ */
    static const char src6[] = "<input disabled type=\"checkbox\">";
    struct dom_document *doc6 = html_parse(src6, sizeof(src6) - 1);
    if (!doc6) return -34;
    struct dom_node *inp6 = find_first_element(doc6->root, "input");
    if (!inp6) return -35;
    if (!dom_has_attribute(inp6, "disabled")) return -36;
    {
        const char *dv = attr_value(inp6, "disabled");
        /* boolean attr stored as empty string */
        if (!dv || dv[0] != '\0') return -37;
    }
    if (!dom_has_attribute(inp6, "type")) return -38;
    {
        const char *tv = attr_value(inp6, "type");
        if (!tv || strcmp(tv, "checkbox") != 0) return -39;
    }

    /* ------------------------------------------------------------------ */
    /* Test 7: unclosed tag — parser must not crash or infinite-loop,     */
    /* and the following text must be attached somewhere in the tree      */
    /* (either as a child of the unclosed element or as its sibling,      */
    /* both are acceptable; just verify it's reachable from <body>).      */
    /* ------------------------------------------------------------------ */
    static const char src7[] =
        "<body><div><p>unclosed paragraph<span>text after</span></div></body>";
    struct dom_document *doc7 = html_parse(src7, sizeof(src7) - 1);
    if (!doc7) return -40;
    {
        struct dom_node *body7 = find_first_element(doc7->root, "body");
        if (!body7) return -41;
        /* The <span> must be somewhere under <body>. */
        struct dom_node *span7 = find_first_element(body7, "span");
        if (!span7) return -42;
        /* <span>'s text child must be "text after". */
        if (!span7->first_child ||
            span7->first_child->type != DOM_NODE_TEXT ||
            strcmp(span7->first_child->text, "text after") != 0) return -43;
    }

    /* Test 7b: completely unclosed tag at end of input. */
    static const char src7b[] = "<body><div>no close</body>";
    struct dom_document *doc7b = html_parse(src7b, sizeof(src7b) - 1);
    if (!doc7b) return -44;
    {
        struct dom_node *body7b = find_first_element(doc7b->root, "body");
        if (!body7b) return -45;
        struct dom_node *div7b = find_first_element(body7b, "div");
        if (!div7b) return -46;
        /* "no close" text must be reachable from <div>. */
        if (!div7b->first_child ||
            div7b->first_child->type != DOM_NODE_TEXT ||
            strcmp(div7b->first_child->text, "no close") != 0) return -47;
    }

    /* ------------------------------------------------------------------ */
    /* Test 8: <script> body containing literal '<' and '>' characters.  */
    /* Raw-text mode must NOT treat them as tag delimiters; the script    */
    /* text must be captured verbatim.                                    */
    /* ------------------------------------------------------------------ */
    static const char src8[] =
        "<script>if (a < b && b > 0) { return a < b; }</script>";
    struct dom_document *doc8 = html_parse(src8, sizeof(src8) - 1);
    if (!doc8) return -48;
    {
        struct dom_node *sc8 = find_first_element(doc8->root, "script");
        if (!sc8) return -49;
        if (!sc8->first_child ||
            sc8->first_child->type != DOM_NODE_TEXT) return -50;
        /* Must contain "a < b" and "b > 0" verbatim. */
        if (!strstr(sc8->first_child->text, "a < b")) return -51;
        if (!strstr(sc8->first_child->text, "b > 0")) return -52;
    }

    /* Test 8b: <style> with CSS containing '<' / '>'. */
    static const char src8b[] =
        "<style>/* comment: a < b */ div > p { color: red; }</style>";
    struct dom_document *doc8b = html_parse(src8b, sizeof(src8b) - 1);
    if (!doc8b) return -53;
    {
        struct dom_node *st8b = find_first_element(doc8b->root, "style");
        if (!st8b) return -54;
        if (!st8b->first_child ||
            st8b->first_child->type != DOM_NODE_TEXT) return -55;
        if (!strstr(st8b->first_child->text, "div > p")) return -56;
    }

    /* Test 8c: <script> containing a fake-close-like sequence that is  */
    /* NOT the real </script> (e.g., </scriptx> must not terminate it).  */
    static const char src8c[] =
        "<script>var s = \"</scriptx>\"; var y=1;</script>after";
    struct dom_document *doc8c = html_parse(src8c, sizeof(src8c) - 1);
    if (!doc8c) return -57;
    {
        struct dom_node *sc8c = find_first_element(doc8c->root, "script");
        if (!sc8c) return -58;
        if (!sc8c->first_child ||
            sc8c->first_child->type != DOM_NODE_TEXT) return -59;
        /* The body must contain the </scriptx> substring verbatim. */
        if (!strstr(sc8c->first_child->text, "</scriptx>")) return -60;
    }

    return 0;
}

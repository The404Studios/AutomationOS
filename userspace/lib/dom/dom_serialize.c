/*
 * dom_serialize.c -- HTML serialiser for AutomationOS browser DOM.
 * ================================================================
 *
 * Implements dom_serialize_inner / dom_serialize_outer / dom_html_escape_text
 * / dom_html_escape_attr / dom_serialize_selftest.
 *
 * Design constraints (all freestanding ring-3):
 *   - NO standard headers.  Uses only malloc.h + string.h (via dom.h).
 *   - Every loop is bounded (sibling scan, recursion depth).
 *   - Buffer writes are always bounds-checked; returns -1 on overflow.
 *   - Recursion depth is capped at DOM_SER_MAX_DEPTH (256).
 *
 * Void elements (no closing tag):
 *   area, base, br, col, embed, hr, img, input, link, meta,
 *   source, track, wbr
 *
 * Document node:  serialises its first ELEMENT child only.
 * Element node:   <tag attr="val"...>CHILDREN</tag>  (or <tag .../> if void)
 * Text node:      HTML-escaped text content.
 * Comment node:   <!-- text content -->
 *
 * Return convention used throughout:
 *   >= 0  : bytes written (excl. NUL)
 *    < 0  : error (overflow / depth-exceeded / NULL input)
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2 -c dom_serialize.c -o dom_serialize.o
 */

#include "dom_serialize.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ------------------------------------------------------------------ */
/*  Internal constants                                                */
/* ------------------------------------------------------------------ */

/* Maximum recursion depth for tree serialisation. */
#define DOM_SER_MAX_DEPTH       256

/* Maximum siblings/attributes scanned per node (defensive). */
#define DOM_SER_MAX_SCAN        (1u << 20)

/* Number of void-element tag names. */
#define VOID_ELEM_COUNT         13

/* ------------------------------------------------------------------ */
/*  Void-element table                                                */
/*                                                                    */
/*  Tags stored lowercased to match dom_node.tag convention.          */
/* ------------------------------------------------------------------ */
static const char *const void_tags[VOID_ELEM_COUNT] = {
    "area", "base", "br", "col", "embed",
    "hr", "img", "input", "link", "meta",
    "source", "track", "wbr"
};

/* ------------------------------------------------------------------ */
/*  Private helper: is the given (already-lowercased) tag a void elm? */
/* ------------------------------------------------------------------ */
static int is_void_element(const char *tag)
{
    if (!tag) return 0;
    for (int i = 0; i < VOID_ELEM_COUNT; i++) {
        if (strcmp(tag, void_tags[i]) == 0) return 1;
    }
    return 0;
}

/* ================================================================== */
/*  Buffer-append helpers                                             */
/*                                                                    */
/*  All operate on a (char *out, unsigned long *pos, unsigned long cap)*/
/*  triple.  *pos tracks bytes written so far (excl. NUL).           */
/*  They return 0 on success, -1 if the buffer would overflow.        */
/*                                                                    */
/*  Convention: cap must be >= 1 so there is always room for the NUL. */
/* ================================================================== */

/*
 * buf_putc -- append a single character.
 */
static int buf_putc(char *out, unsigned long *pos, unsigned long cap, char c)
{
    if (*pos + 1 >= cap) return -1;   /* need room for c + NUL */
    out[(*pos)++] = c;
    out[*pos] = '\0';
    return 0;
}

/*
 * buf_puts -- append a NUL-terminated string.
 */
static int buf_puts(char *out, unsigned long *pos, unsigned long cap,
                    const char *s)
{
    if (!s) return 0;
    for (unsigned long i = 0; s[i]; i++) {
        if (buf_putc(out, pos, cap, s[i]) < 0) return -1;
    }
    return 0;
}

/*
 * buf_puts_n -- append exactly `n` bytes from `s`.
 */
static int buf_puts_n(char *out, unsigned long *pos, unsigned long cap,
                      const char *s, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++) {
        if (buf_putc(out, pos, cap, s[i]) < 0) return -1;
    }
    return 0;
}

/* ================================================================== */
/*  Public: escape functions                                          */
/* ================================================================== */

long dom_html_escape_text(const char *s, char *out, unsigned long cap)
{
    if (!s || !out) return -1;
    if (cap == 0) return -1;
    out[0] = '\0';
    if (cap < 1) return -1;

    unsigned long pos = 0;

    for (unsigned long i = 0; s[i]; i++) {
        char c = s[i];
        int r = 0;
        if (c == '&') {
            r = buf_puts(out, &pos, cap, "&amp;");
        } else if (c == '<') {
            r = buf_puts(out, &pos, cap, "&lt;");
        } else if (c == '>') {
            r = buf_puts(out, &pos, cap, "&gt;");
        } else {
            r = buf_putc(out, &pos, cap, c);
        }
        if (r < 0) return -1;
    }

    return (long)pos;
}

long dom_html_escape_attr(const char *s, char *out, unsigned long cap)
{
    if (!s || !out) return -1;
    if (cap == 0) return -1;
    out[0] = '\0';

    unsigned long pos = 0;

    for (unsigned long i = 0; s[i]; i++) {
        char c = s[i];
        int r = 0;
        if (c == '&') {
            r = buf_puts(out, &pos, cap, "&amp;");
        } else if (c == '<') {
            r = buf_puts(out, &pos, cap, "&lt;");
        } else if (c == '>') {
            r = buf_puts(out, &pos, cap, "&gt;");
        } else if (c == '"') {
            r = buf_puts(out, &pos, cap, "&quot;");
        } else {
            r = buf_putc(out, &pos, cap, c);
        }
        if (r < 0) return -1;
    }

    return (long)pos;
}

/* ================================================================== */
/*  Internal: recursive serialiser                                    */
/* ================================================================== */

/*
 * ser_node -- serialise `node` into (out, pos, cap) at the given depth.
 *
 * mode == 0 : emit the node itself + its children  (outerHTML of node)
 * mode == 1 : emit only the node's children         (innerHTML of node)
 *
 * Returns 0 on success, -1 on overflow / depth-exceeded.
 */
static int ser_node(const dom_node *node,
                    char *out, unsigned long *pos, unsigned long cap,
                    int depth, int mode);

/*
 * ser_children -- iterate siblings of first_child, calling ser_node(mode=0).
 */
static int ser_children(const dom_node *first_child,
                         char *out, unsigned long *pos, unsigned long cap,
                         int depth)
{
    unsigned long guard = 0;
    for (const dom_node *c = first_child;
         c && guard++ < DOM_SER_MAX_SCAN;
         c = c->next_sibling)
    {
        if (ser_node(c, out, pos, cap, depth, /*mode=outer*/0) < 0) return -1;
    }
    return 0;
}

static int ser_node(const dom_node *node,
                    char *out, unsigned long *pos, unsigned long cap,
                    int depth, int mode)
{
    if (!node) return 0;
    if (depth > DOM_SER_MAX_DEPTH) return -1;

    switch (node->type) {

    /* -------------------------------------------------------------- */
    case DOM_NODE_TEXT:
        if (mode == 0) {
            /* Escape text content. */
            if (node->text) {
                for (unsigned long i = 0; node->text[i]; i++) {
                    char c = node->text[i];
                    int r = 0;
                    if (c == '&')      r = buf_puts(out, pos, cap, "&amp;");
                    else if (c == '<') r = buf_puts(out, pos, cap, "&lt;");
                    else if (c == '>') r = buf_puts(out, pos, cap, "&gt;");
                    else               r = buf_putc(out, pos, cap, c);
                    if (r < 0) return -1;
                }
            }
        }
        /* mode==1 (innerHTML of a text node) emits nothing -- text nodes
           have no children. */
        return 0;

    /* -------------------------------------------------------------- */
    case DOM_NODE_COMMENT:
        if (mode == 0) {
            if (buf_puts(out, pos, cap, "<!--") < 0) return -1;
            /* Comment text is NOT entity-escaped per HTML spec. */
            if (node->text) {
                if (buf_puts(out, pos, cap, node->text) < 0) return -1;
            }
            if (buf_puts(out, pos, cap, "-->") < 0) return -1;
        }
        return 0;

    /* -------------------------------------------------------------- */
    case DOM_NODE_DOCUMENT: {
        /* For a document node, serialise the first ELEMENT child only. */
        const dom_node *doc_el = NULL;
        unsigned long guard = 0;
        for (const dom_node *c = node->first_child;
             c && guard++ < DOM_SER_MAX_SCAN;
             c = c->next_sibling)
        {
            if (c->type == DOM_NODE_ELEMENT) { doc_el = c; break; }
        }
        if (!doc_el) return 0;
        return ser_node(doc_el, out, pos, cap, depth, mode);
    }

    /* -------------------------------------------------------------- */
    case DOM_NODE_ELEMENT: {
        if (mode == 1) {
            /* innerHTML: only children, skip the element tag itself. */
            return ser_children(node->first_child, out, pos, cap, depth + 1);
        }

        /* outerHTML: emit opening tag. */
        if (buf_putc(out, pos, cap, '<') < 0) return -1;
        if (buf_puts(out, pos, cap, node->tag ? node->tag : "?") < 0) return -1;

        /* Attributes. */
        unsigned long aguard = 0;
        for (const dom_attr *a = node->attrs;
             a && aguard++ < DOM_SER_MAX_SCAN;
             a = a->next)
        {
            if (!a->name) continue;
            if (buf_putc(out, pos, cap, ' ') < 0) return -1;
            if (buf_puts(out, pos, cap, a->name) < 0) return -1;
            if (buf_putc(out, pos, cap, '=') < 0) return -1;
            if (buf_putc(out, pos, cap, '"') < 0) return -1;
            /* Escape the attribute value. */
            const char *val = a->value ? a->value : "";
            for (unsigned long vi = 0; val[vi]; vi++) {
                char c = val[vi];
                int r = 0;
                if (c == '&')      r = buf_puts(out, pos, cap, "&amp;");
                else if (c == '<') r = buf_puts(out, pos, cap, "&lt;");
                else if (c == '>') r = buf_puts(out, pos, cap, "&gt;");
                else if (c == '"') r = buf_puts(out, pos, cap, "&quot;");
                else               r = buf_putc(out, pos, cap, c);
                if (r < 0) return -1;
            }
            if (buf_putc(out, pos, cap, '"') < 0) return -1;
        }

        /* Void elements: close opening tag, no children, no closing tag. */
        if (is_void_element(node->tag)) {
            if (buf_putc(out, pos, cap, '>') < 0) return -1;
            return 0;
        }

        /* Non-void: close opening tag, recurse children, emit closing tag. */
        if (buf_putc(out, pos, cap, '>') < 0) return -1;

        if (ser_children(node->first_child, out, pos, cap, depth + 1) < 0)
            return -1;

        if (buf_puts(out, pos, cap, "</") < 0) return -1;
        if (buf_puts(out, pos, cap, node->tag ? node->tag : "?") < 0) return -1;
        if (buf_putc(out, pos, cap, '>') < 0) return -1;

        return 0;
    }

    default:
        return 0;
    }
}

/* ================================================================== */
/*  Public: dom_serialize_inner / dom_serialize_outer                 */
/* ================================================================== */

long dom_serialize_inner(const struct dom_node *el, char *out, unsigned long cap)
{
    if (!el || !out) return -1;
    if (cap == 0) return -1;

    out[0] = '\0';
    unsigned long pos = 0;

    if (ser_node(el, out, &pos, cap, 0, /*mode=inner*/1) < 0) return -1;

    return (long)pos;
}

long dom_serialize_outer(const struct dom_node *el, char *out, unsigned long cap)
{
    if (!el || !out) return -1;
    if (cap == 0) return -1;

    out[0] = '\0';
    unsigned long pos = 0;

    if (ser_node(el, out, &pos, cap, 0, /*mode=outer*/0) < 0) return -1;

    return (long)pos;
}

/* ================================================================== */
/*  Selftest                                                          */
/* ================================================================== */

/*
 * Minimal string comparison helper used by the selftest (avoids pulling in
 * additional helpers; strcmp is already available via dom.h -> string.h).
 */

int dom_serialize_selftest(void)
{
    /*
     * Build:
     *   <div id="x" class="y">Hello &amp; bye</div>
     *
     * Expected outerHTML (byte-for-byte):
     *   <div id="x" class="y">Hello &amp; bye</div>
     *
     * The text content stored in the DOM is the decoded form "Hello & bye".
     * The serialiser must re-encode '&' as '&amp;'.
     */

    /* ------------------------------------------------------------------ */
    /* 1. Construct the DOM fragment.                                      */
    /* ------------------------------------------------------------------ */
    dom_node *div = dom_create_element("div");
    if (!div) return -1;

    dom_set_attribute(div, "id", "x");
    dom_set_attribute(div, "class", "y");
    dom_set_text(div, "Hello & bye");   /* '&' is raw in the DOM */

    /* ------------------------------------------------------------------ */
    /* 2. Serialise with dom_serialize_outer.                              */
    /* ------------------------------------------------------------------ */
    char buf[256];
    long n = dom_serialize_outer(div, buf, sizeof(buf));
    if (n < 0) { dom_node_free(div); return -2; }

    /* ------------------------------------------------------------------ */
    /* 3. Compare byte-for-byte against the known expected string.         */
    /* ------------------------------------------------------------------ */
    const char *expected = "<div id=\"x\" class=\"y\">Hello &amp; bye</div>";
    if (strcmp(buf, expected) != 0) { dom_node_free(div); return -3; }

    /* ------------------------------------------------------------------ */
    /* 4. Test dom_serialize_inner (should yield just the text child).     */
    /* ------------------------------------------------------------------ */
    long ni = dom_serialize_inner(div, buf, sizeof(buf));
    if (ni < 0) { dom_node_free(div); return -4; }
    const char *expected_inner = "Hello &amp; bye";
    if (strcmp(buf, expected_inner) != 0) { dom_node_free(div); return -5; }

    /* ------------------------------------------------------------------ */
    /* 5. Test dom_html_escape_text.                                       */
    /* ------------------------------------------------------------------ */
    long ne = dom_html_escape_text("<br> & \"test\"", buf, sizeof(buf));
    if (ne < 0) { dom_node_free(div); return -6; }
    if (strcmp(buf, "&lt;br&gt; &amp; \"test\"") != 0) {
        dom_node_free(div);
        return -7;
    }

    /* ------------------------------------------------------------------ */
    /* 6. Test dom_html_escape_attr (also escapes ").                      */
    /* ------------------------------------------------------------------ */
    long na = dom_html_escape_attr("say \"hello\" & <bye>", buf, sizeof(buf));
    if (na < 0) { dom_node_free(div); return -8; }
    if (strcmp(buf, "say &quot;hello&quot; &amp; &lt;bye&gt;") != 0) {
        dom_node_free(div);
        return -9;
    }

    /* ------------------------------------------------------------------ */
    /* 7. Test void element (br) -- no closing tag, no children.           */
    /* ------------------------------------------------------------------ */
    dom_node *br = dom_create_element("br");
    if (!br) { dom_node_free(div); return -10; }
    long nbr = dom_serialize_outer(br, buf, sizeof(buf));
    if (nbr < 0 || strcmp(buf, "<br>") != 0) {
        dom_node_free(br);
        dom_node_free(div);
        return -11;
    }
    dom_node_free(br);

    /* ------------------------------------------------------------------ */
    /* 8. Test comment node.                                               */
    /* ------------------------------------------------------------------ */
    dom_node *cmt = dom_create_comment("a comment");
    if (!cmt) { dom_node_free(div); return -12; }
    long ncmt = dom_serialize_outer(cmt, buf, sizeof(buf));
    if (ncmt < 0 || strcmp(buf, "<!--a comment-->") != 0) {
        dom_node_free(cmt);
        dom_node_free(div);
        return -13;
    }
    dom_node_free(cmt);

    /* ------------------------------------------------------------------ */
    /* 9. Test nested structure: <p class="lead"><span>world</span></p>    */
    /* ------------------------------------------------------------------ */
    dom_node *p = dom_create_element("p");
    dom_node *span = dom_create_element("span");
    if (!p || !span) {
        if (p) dom_node_free(p);
        if (span) dom_node_free(span);
        dom_node_free(div);
        return -14;
    }
    dom_set_attribute(p, "class", "lead");
    dom_set_text(span, "world");
    dom_append_child(p, span);

    long np = dom_serialize_outer(p, buf, sizeof(buf));
    if (np < 0 || strcmp(buf, "<p class=\"lead\"><span>world</span></p>") != 0) {
        dom_node_free(p);
        dom_node_free(div);
        return -15;
    }
    dom_node_free(p);

    /* ------------------------------------------------------------------ */
    /* 10. Test document node serialises first element child.              */
    /* ------------------------------------------------------------------ */
    dom_document *doc = dom_document_new();
    if (!doc) { dom_node_free(div); return -16; }
    dom_node *html = dom_create_element("html");
    if (!html) { dom_document_free(doc); dom_node_free(div); return -17; }
    dom_append_child(doc->root, html);
    dom_set_text(html, "doc");
    long ndoc = dom_serialize_outer(doc->root, buf, sizeof(buf));
    if (ndoc < 0 || strcmp(buf, "<html>doc</html>") != 0) {
        dom_document_free(doc);
        dom_node_free(div);
        return -18;
    }
    dom_document_free(doc);

    /* ------------------------------------------------------------------ */
    /* 11. Buffer-overflow protection: tiny buffer.                        */
    /* ------------------------------------------------------------------ */
    char tiny[4];
    long nov = dom_serialize_outer(div, tiny, sizeof(tiny));
    if (nov >= 0) {
        /* Should have returned an error because "<div..." won't fit in 3 bytes. */
        dom_node_free(div);
        return -19;
    }

    /* ------------------------------------------------------------------ */
    /* Done.                                                               */
    /* ------------------------------------------------------------------ */
    dom_node_free(div);
    return 0;
}

/*
 * dom.c -- AutomationOS browser DOM data model (implementation).
 * ==============================================================
 *
 * Freestanding ring-3. Backed by malloc/free (NOT the JS engine arena),
 * because DOM nodes outlive any single JS evaluation. See dom.h.
 *
 * Strings are stored as malloc'd, NUL-terminated buffers. ASCII-only
 * casing for tag names; attribute names are also lowercased (HTML rules).
 *
 * All loops here are bounded: tree walks cap at DOM_WALK_MAX_DEPTH and
 * sibling iteration cap at DOM_MAX_CHILDREN_SCAN (defensive).
 */
#include "dom.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Generous defensive caps; the real ceiling is heap memory. */
#define DOM_WALK_MAX_DEPTH        256
#define DOM_MAX_CHILDREN_SCAN     (1u << 20)   /* one million siblings  */
#define DOM_TEXT_CONCAT_CAP_BASE  64

/* ================================================================== */
/*  Tiny private helpers                                              */
/* ================================================================== */
static char ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Lowercased duplicate of `s` (malloc'd). Returns NULL on OOM or NULL in. */
static char *dup_lower(const char *s)
{
    if (!s) return NULL;
    unsigned long n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    for (unsigned long i = 0; i < n; i++) out[i] = ascii_tolower(s[i]);
    out[n] = 0;
    return out;
}

/* Plain duplicate (malloc'd). Accepts NULL (returns NULL). */
static char *dup_str(const char *s)
{
    if (!s) return NULL;
    unsigned long n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* Case-insensitive ASCII compare of two NUL-terminated strings. */
static int ascii_icmp(const char *a, const char *b)
{
    if (a == b) return 0;
    if (!a || !b) return a ? 1 : -1;
    while (*a && *b) {
        char ca = ascii_tolower(*a), cb = ascii_tolower(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)ascii_tolower(*a)
         - (int)(unsigned char)ascii_tolower(*b);
}

/* ================================================================== */
/*  Document                                                          */
/* ================================================================== */
dom_document *dom_document_new(void)
{
    dom_document *doc = (dom_document *)calloc(1, sizeof(dom_document));
    if (!doc) return NULL;
    /* Root document node */
    doc->root = (dom_node *)calloc(1, sizeof(dom_node));
    if (!doc->root) { free(doc); return NULL; }
    doc->root->type = DOM_NODE_DOCUMENT;
    doc->url = NULL;
    return doc;
}

void dom_document_free(dom_document *doc)
{
    if (!doc) return;
    if (doc->root) dom_node_free(doc->root);
    if (doc->url)  free(doc->url);
    free(doc);
}

/* ================================================================== */
/*  Node creation / destruction                                       */
/* ================================================================== */
dom_node *dom_create_element(const char *tag)
{
    if (!tag) return NULL;
    dom_node *n = (dom_node *)calloc(1, sizeof(dom_node));
    if (!n) return NULL;
    n->type = DOM_NODE_ELEMENT;
    n->tag = dup_lower(tag);
    if (!n->tag) { free(n); return NULL; }
    return n;
}

dom_node *dom_create_text(const char *text)
{
    dom_node *n = (dom_node *)calloc(1, sizeof(dom_node));
    if (!n) return NULL;
    n->type = DOM_NODE_TEXT;
    n->text = dup_str(text ? text : "");
    if (!n->text) { free(n); return NULL; }
    return n;
}

dom_node *dom_create_comment(const char *text)
{
    dom_node *n = (dom_node *)calloc(1, sizeof(dom_node));
    if (!n) return NULL;
    n->type = DOM_NODE_COMMENT;
    n->text = dup_str(text ? text : "");
    if (!n->text) { free(n); return NULL; }
    return n;
}

/* Free attribute list. */
static void free_attrs(dom_attr *a)
{
    unsigned long guard = 0;
    while (a && guard++ < DOM_MAX_CHILDREN_SCAN) {
        dom_attr *next = a->next;
        if (a->name)  free(a->name);
        if (a->value) free(a->value);
        free(a);
        a = next;
    }
}

void dom_node_free(dom_node *node)
{
    if (!node) return;

    /* Free child subtree iteratively-bounded to avoid pathological stack
       use. We snapshot first_child and walk siblings; each child does its
       own recursion (depth bounded by DOM_WALK_MAX_DEPTH in practice). */
    dom_node *c = node->first_child;
    unsigned long guard = 0;
    while (c && guard++ < DOM_MAX_CHILDREN_SCAN) {
        dom_node *next = c->next_sibling;
        dom_node_free(c);
        c = next;
    }

    free_attrs(node->attrs);
    if (node->tag)  free(node->tag);
    if (node->text) free(node->text);
    /* node->user is owned by the consumer (layout/render); we don't free. */
    free(node);
}

/* ================================================================== */
/*  Tree mutation                                                     */
/* ================================================================== */

/* Internal: detach `child` from its current parent (if any). */
static void detach_child(dom_node *child)
{
    if (!child || !child->parent) return;
    dom_node *p = child->parent;
    if (child->prev_sibling) child->prev_sibling->next_sibling = child->next_sibling;
    else                     p->first_child = child->next_sibling;
    if (child->next_sibling) child->next_sibling->prev_sibling = child->prev_sibling;
    else                     p->last_child = child->prev_sibling;
    child->parent       = NULL;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;
}

void dom_append_child(dom_node *parent, dom_node *child)
{
    if (!parent || !child) return;
    if (child->parent) detach_child(child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else                    parent->first_child = child;
    parent->last_child = child;
}

void dom_remove_child(dom_node *parent, dom_node *child)
{
    if (!parent || !child) return;
    if (child->parent != parent) return;
    detach_child(child);
}

void dom_insert_before(dom_node *parent, dom_node *child, dom_node *ref)
{
    if (!parent || !child) return;
    if (!ref) { dom_append_child(parent, child); return; }
    if (ref->parent != parent) return;
    if (child->parent) detach_child(child);

    child->parent = parent;
    child->prev_sibling = ref->prev_sibling;
    child->next_sibling = ref;
    if (ref->prev_sibling) ref->prev_sibling->next_sibling = child;
    else                   parent->first_child = child;
    ref->prev_sibling = child;
}

/* ================================================================== */
/*  Attributes                                                        */
/* ================================================================== */
static dom_attr *find_attr(const dom_node *el, const char *name)
{
    if (!el || el->type != DOM_NODE_ELEMENT || !name) return NULL;
    unsigned long guard = 0;
    for (dom_attr *a = el->attrs; a && guard++ < DOM_MAX_CHILDREN_SCAN; a = a->next) {
        if (a->name && ascii_icmp(a->name, name) == 0) return a;
    }
    return NULL;
}

void dom_set_attribute(dom_node *el, const char *name, const char *value)
{
    if (!el || el->type != DOM_NODE_ELEMENT || !name) return;

    dom_attr *a = find_attr(el, name);
    if (a) {
        char *nv = dup_str(value ? value : "");
        if (!nv) return;
        if (a->value) free(a->value);
        a->value = nv;
        return;
    }
    a = (dom_attr *)calloc(1, sizeof(dom_attr));
    if (!a) return;
    a->name  = dup_lower(name);
    a->value = dup_str(value ? value : "");
    if (!a->name || !a->value) {
        if (a->name)  free(a->name);
        if (a->value) free(a->value);
        free(a);
        return;
    }
    /* Append to keep insertion order. */
    if (!el->attrs) {
        el->attrs = a;
    } else {
        dom_attr *t = el->attrs;
        unsigned long guard = 0;
        while (t->next && guard++ < DOM_MAX_CHILDREN_SCAN) t = t->next;
        t->next = a;
    }
}

const char *dom_get_attribute(const dom_node *el, const char *name)
{
    dom_attr *a = find_attr(el, name);
    return a ? a->value : NULL;
}

int dom_has_attribute(const dom_node *el, const char *name)
{
    return find_attr(el, name) != NULL;
}

void dom_remove_attribute(dom_node *el, const char *name)
{
    if (!el || el->type != DOM_NODE_ELEMENT || !name) return;
    dom_attr *prev = NULL;
    unsigned long guard = 0;
    for (dom_attr *a = el->attrs; a && guard++ < DOM_MAX_CHILDREN_SCAN; a = a->next) {
        if (a->name && ascii_icmp(a->name, name) == 0) {
            if (prev) prev->next = a->next;
            else      el->attrs  = a->next;
            if (a->name)  free(a->name);
            if (a->value) free(a->value);
            free(a);
            return;
        }
        prev = a;
    }
}

/* ================================================================== */
/*  Text content                                                      */
/* ================================================================== */
void dom_set_text(dom_node *node, const char *text)
{
    if (!node) return;
    if (node->type == DOM_NODE_TEXT || node->type == DOM_NODE_COMMENT) {
        char *nv = dup_str(text ? text : "");
        if (!nv) return;
        if (node->text) free(node->text);
        node->text = nv;
        return;
    }
    if (node->type == DOM_NODE_ELEMENT) {
        /* Replace all children with a single TEXT node. */
        dom_node *c = node->first_child;
        unsigned long guard = 0;
        while (c && guard++ < DOM_MAX_CHILDREN_SCAN) {
            dom_node *next = c->next_sibling;
            c->parent = NULL;
            dom_node_free(c);
            c = next;
        }
        node->first_child = node->last_child = NULL;

        if (text && text[0]) {
            dom_node *t = dom_create_text(text);
            if (t) dom_append_child(node, t);
        }
        return;
    }
    /* DOCUMENT: ignored. */
}

/* --- text concatenation buffer (malloc + grow) --- */
typedef struct {
    char        *buf;
    unsigned long len;
    unsigned long cap;
    int           oom;
} text_buf;

static void tb_append(text_buf *t, const char *s)
{
    if (t->oom || !s) return;
    unsigned long n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        unsigned long ncap = t->cap ? t->cap * 2 : DOM_TEXT_CONCAT_CAP_BASE;
        while (ncap < t->len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(t->buf, ncap);
        if (!nb) { t->oom = 1; return; }
        t->buf = nb;
        t->cap = ncap;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = 0;
}

static void collect_text(const dom_node *n, text_buf *t, int depth)
{
    if (!n || depth > DOM_WALK_MAX_DEPTH) return;
    if (n->type == DOM_NODE_TEXT) {
        if (n->text) tb_append(t, n->text);
        return;
    }
    if (n->type == DOM_NODE_ELEMENT || n->type == DOM_NODE_DOCUMENT) {
        unsigned long guard = 0;
        for (dom_node *c = n->first_child;
             c && guard++ < DOM_MAX_CHILDREN_SCAN;
             c = c->next_sibling) {
            collect_text(c, t, depth + 1);
        }
    }
}

const char *dom_get_text(const dom_node *node)
{
    if (!node) return NULL;
    if (node->type == DOM_NODE_TEXT || node->type == DOM_NODE_COMMENT) {
        return node->text;
    }
    if (node->type != DOM_NODE_ELEMENT && node->type != DOM_NODE_DOCUMENT)
        return NULL;

    text_buf t = { NULL, 0, 0, 0 };
    collect_text(node, &t, 0);
    if (t.oom) {
        if (t.buf) free(t.buf);
        return NULL;
    }
    if (!t.buf) {
        /* return malloc'd empty string so contract (malloc'd if non-NULL)
           is consistent. */
        t.buf = (char *)malloc(1);
        if (!t.buf) return NULL;
        t.buf[0] = 0;
    }
    return t.buf;
}

/* ================================================================== */
/*  Queries                                                           */
/* ================================================================== */
static dom_node *find_id(dom_node *n, const char *id, int depth)
{
    if (!n || depth > DOM_WALK_MAX_DEPTH) return NULL;
    if (n->type == DOM_NODE_ELEMENT) {
        const char *aid = dom_get_attribute(n, "id");
        if (aid && strcmp(aid, id) == 0) return n;
    }
    unsigned long guard = 0;
    for (dom_node *c = n->first_child;
         c && guard++ < DOM_MAX_CHILDREN_SCAN;
         c = c->next_sibling) {
        dom_node *r = find_id(c, id, depth + 1);
        if (r) return r;
    }
    return NULL;
}

dom_node *dom_get_element_by_id(dom_document *doc, const char *id)
{
    if (!doc || !doc->root || !id) return NULL;
    return find_id(doc->root, id, 0);
}

static void walk_rec(dom_node *n, void (*visit)(dom_node *, void *),
                     void *ctx, int depth)
{
    if (!n || depth > DOM_WALK_MAX_DEPTH) return;
    visit(n, ctx);
    unsigned long guard = 0;
    for (dom_node *c = n->first_child;
         c && guard++ < DOM_MAX_CHILDREN_SCAN;
         c = c->next_sibling) {
        walk_rec(c, visit, ctx, depth + 1);
    }
}

void dom_walk(dom_node *root, void (*visit)(dom_node *, void *), void *ctx)
{
    if (!root || !visit) return;
    walk_rec(root, visit, ctx, 0);
}

/* ================================================================== */
/*  Selftest                                                          */
/* ================================================================== */
struct count_ctx { int elements; int texts; int comments; int docs; };
static void count_visitor(dom_node *n, void *ctx)
{
    struct count_ctx *c = (struct count_ctx *)ctx;
    switch (n->type) {
    case DOM_NODE_ELEMENT:  c->elements++; break;
    case DOM_NODE_TEXT:     c->texts++;    break;
    case DOM_NODE_COMMENT:  c->comments++; break;
    case DOM_NODE_DOCUMENT: c->docs++;     break;
    }
}

int dom_selftest(void)
{
    /* Build:
         <html>
           <head><title>Hi</title></head>
           <body>
             <div id="x">hello</div>
             <p class="lead">world<!--mark--></p>
           </body>
         </html>
    */
    dom_document *doc = dom_document_new();
    if (!doc) return -1;

    dom_node *html  = dom_create_element("HTML");      /* test lowering */
    dom_node *head  = dom_create_element("head");
    dom_node *title = dom_create_element("title");
    dom_node *body  = dom_create_element("body");
    dom_node *divv  = dom_create_element("div");
    dom_node *p     = dom_create_element("p");
    dom_node *cmt   = dom_create_comment("mark");

    if (!html || !head || !title || !body || !divv || !p || !cmt) {
        dom_document_free(doc);
        return -2;
    }

    dom_append_child(doc->root, html);
    dom_append_child(html, head);
    dom_append_child(head, title);
    dom_set_text(title, "Hi");

    dom_append_child(html, body);
    dom_append_child(body, divv);
    dom_set_attribute(divv, "ID", "x");                /* attr lowering */
    dom_set_text(divv, "hello");

    dom_append_child(body, p);
    dom_set_attribute(p, "class", "lead");
    dom_set_text(p, "world");
    /* re-add the comment after set_text (set_text wiped children) */
    dom_append_child(p, cmt);

    /* --- assertions --- */
    /* Tag lowering */
    if (!html->tag || strcmp(html->tag, "html") != 0) goto fail;
    /* Attribute name lowered, lookup case-insensitive */
    const char *id = dom_get_attribute(divv, "id");
    if (!id || strcmp(id, "x") != 0) goto fail;
    if (!dom_has_attribute(divv, "Id")) goto fail;

    /* getElementById */
    if (dom_get_element_by_id(doc, "x") != divv) goto fail;
    if (dom_get_element_by_id(doc, "nope") != NULL) goto fail;

    /* textContent on element with mixed children */
    const char *t = dom_get_text(p);
    if (!t) goto fail;
    /* p has "world" text + a comment (excluded by spec). */
    if (strcmp(t, "world") != 0) { free((void *)t); goto fail; }
    free((void *)t);

    /* set_text on element drops children and adds a single text child.
     * IMPORTANT: body's current children are `divv` and `p` (with their
     * subtrees), so this call FREES those nodes. The local `divv`/`p`
     * pointers are now dangling -- null them so the cleanup below (and the
     * `fail:` path) does not double-free them. The prior code read the
     * freed nodes' stale `->parent` (left NULL by dom_set_text just before
     * the free) and, seeing NULL, called dom_node_free(p)/dom_node_free(divv)
     * a SECOND time. That double-free pushed the same blocks onto the
     * allocator free-list/tcache twice; a later malloc then handed one block
     * to two live objects, corrupting (e.g.) a freshly-created <h1> node's
     * ->tag and making the dom_selector_selftest h1 type-match fail (-896). */
    dom_set_text(body, "replaced");
    divv = NULL;   /* freed by dom_set_text(body, ...) above */
    p    = NULL;   /* freed by dom_set_text(body, ...) above */
    if (!body->first_child || body->first_child != body->last_child) goto fail;
    if (body->first_child->type != DOM_NODE_TEXT) goto fail;
    if (strcmp(body->first_child->text, "replaced") != 0) goto fail;

    /* dom_walk counts */
    struct count_ctx cnt = {0,0,0,0};
    dom_walk(doc->root, count_visitor, &cnt);
    /* document=1, html, head, title, body = 4 elements (we removed div/p
       via set_text(body,..)); texts: title's "Hi" + body's "replaced" = 2 */
    if (cnt.docs != 1 || cnt.elements != 4 || cnt.texts != 2 || cnt.comments != 0)
        goto fail;

    /* Remove + reinsert */
    dom_remove_child(html, head);
    if (head->parent != NULL) goto fail;
    if (html->first_child != body) goto fail;
    dom_insert_before(html, head, body);
    if (html->first_child != head) goto fail;
    if (head->next_sibling != body) goto fail;

    /* NOTE: `divv` and `p` were already freed (and nulled) by
     * dom_set_text(body, "replaced") above, so there is nothing to free or
     * remove here -- doing so would be a use-after-free / double-free. The
     * attribute-removal contract is covered by domtest.c's dedicated
     * set/remove-attribute round-trip on a live node. */

    dom_document_free(doc);
    return 0;

fail:
    /* Best-effort cleanup of any still-detached nodes. `divv`/`p` are NULL
     * once dom_set_text(body, ...) has freed them, so the NULL guards below
     * correctly skip them (no double-free). */
    if (p && !p->parent) dom_node_free(p);
    if (divv && !divv->parent) dom_node_free(divv);
    dom_document_free(doc);
    return -3;
}

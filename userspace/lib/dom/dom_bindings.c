/*
 * dom_bindings.c -- expose the DOM to JavaScript.
 * ================================================
 *
 * See dom_bindings.h. This file registers three JS native classes
 * (Document, Element, TextNode) with the JS engine, wraps a
 * dom_document, and binds the result as the JS global `document`.
 *
 * Wrapper objects live in the JS arena and are reset between
 * js_eval() calls; the underlying dom_node graph (malloc-backed) is
 * stable across resets. Reusing wrappers across resets is therefore
 * unsupported -- query the DOM again from JS after each script.
 *
 * Property dispatch convention:
 *   - getter returns js_native_make_undefined() to mean "not handled"
 *     (the engine then tries own-prop / prototype chain).
 *   - setter returns 0 = handled OK, positive = pass through to JS
 *     own-prop store, negative = error already thrown.
 */
#include "dom_bindings.h"
#include "../js/js_internal.h"   /* js_object/js_value internals      */
#include "dom_selector.h"        /* dom_query_selector[_all] + dom_selector_match */
#include "dom_event.h"           /* dom_add_event_listener / dispatch */
#include "dom_util.h"            /* dom_clone_node                    */
#include "../html/html_parse.h"  /* html_parse_fragment (innerHTML)   */

/* Freestanding: pull NULL from a libc header. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Module-local state. The engine has a single VM instance and so do
 * we; that's enough for the current browser milestone. */
static int           g_doc_class_id   = 0;
static int           g_elem_class_id  = 0;
static int           g_text_class_id  = 0;
static int           g_event_class_id = 0;
static int           g_clist_class_id = 0;   /* classList wrapper class */
static dom_document *g_doc            = NULL;
static js_vm        *g_vm             = NULL;

/* ================================================================== */
/*  Tiny ASCII helpers (no libc beyond malloc/string)                 */
/* ================================================================== */
static int str_eq(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* str_len: freestanding strlen (avoid libc dependency in this module). */
static unsigned long str_len(const char *s)
{
    if (!s) return 0;
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* str_starts_with_word: check if a space-separated token list contains `tok`.
 * Used for classList helpers. */
static int cls_contains_tok(const char *cls, const char *tok)
{
    if (!cls || !tok) return 0;
    unsigned long tlen = str_len(tok);
    if (!tlen) return 0;
    const char *p = cls;
    while (*p) {
        /* skip spaces */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        /* measure this token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        unsigned long len = (unsigned long)(p - start);
        if (len == tlen) {
            unsigned long i = 0;
            while (i < len && start[i] == tok[i]) i++;
            if (i == len) return 1;
        }
    }
    return 0;
}

/* Build a new class-attribute string with tok added (arena-allocated).
 * If already present returns NULL (no change needed). */
static const char *cls_add_tok(js_vm *vm, const char *cls, const char *tok)
{
    if (cls_contains_tok(cls, tok)) return NULL;
    unsigned long clen = str_len(cls);
    unsigned long tlen = str_len(tok);
    /* result: existing (trimmed) + space + tok, or just tok */
    unsigned long need = clen + tlen + 2;
    char *out = (char *)js_arena_alloc(vm, need);
    if (!out) return tok;  /* OOM: best effort */
    unsigned long i = 0;
    if (clen) {
        for (unsigned long j = 0; j < clen; j++) out[i++] = cls[j];
        out[i++] = ' ';
    }
    for (unsigned long j = 0; j < tlen; j++) out[i++] = tok[j];
    out[i] = 0;
    return out;
}

/* Build a new class-attribute string with tok removed (arena-allocated).
 * Tokens are space-separated; collapse extra spaces. Returns NULL if not present. */
static const char *cls_remove_tok(js_vm *vm, const char *cls, const char *tok)
{
    if (!cls_contains_tok(cls, tok)) return NULL;
    unsigned long tlen = str_len(tok);
    unsigned long clen = str_len(cls);
    /* worst-case output same length as input */
    char *out = (char *)js_arena_alloc(vm, clen + 1);
    if (!out) return cls;  /* OOM: no change */
    unsigned long oi = 0;
    const char *p = cls;
    int first = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        unsigned long len = (unsigned long)(p - start);
        /* skip the token we're removing */
        if (len == tlen) {
            unsigned long k = 0;
            while (k < len && start[k] == tok[k]) k++;
            if (k == len) continue;
        }
        if (!first) out[oi++] = ' ';
        for (unsigned long k = 0; k < len; k++) out[oi++] = start[k];
        first = 0;
    }
    out[oi] = 0;
    return out;
}

/* Collect all elements in the subtree `root` whose tag equals `tag`
 * (lowercased) into `out[0..max-1]`. Returns number written. */
static int collect_by_tag(dom_node *root, const char *tag, dom_node **out, int max)
{
    if (!root) return 0;
    int n = 0;
    unsigned long guard = 0;
    /* iterative pre-order using the child/sibling links */
    /* We use a tiny explicit stack to avoid deep recursion. */
    /* Max practical depth: nested ~256 nodes is fine. */
    dom_node *stack[256];
    int top = 0;
    /* Push first child of root (root itself not tested, mirrors querySelectorAll) */
    for (dom_node *c = root->last_child; c && top < 256; c = c->prev_sibling)
        stack[top++] = c;
    while (top > 0 && n < max && guard++ < (1u << 20)) {
        dom_node *cur = stack[--top];
        if (cur->type == DOM_NODE_ELEMENT) {
            int match = (tag[0] == '*' && tag[1] == 0)
                ? 1
                : (cur->tag && str_eq(cur->tag, tag));
            if (match && n < max) out[n++] = cur;
            /* push children in reverse order */
            for (dom_node *c = cur->last_child; c && top < 256; c = c->prev_sibling)
                stack[top++] = c;
        }
    }
    return n;
}

/* Collect all elements in the subtree `root` that have `cls` in their
 * class attribute (space-separated). Returns number written. */
static int collect_by_class(dom_node *root, const char *cls, dom_node **out, int max)
{
    if (!root || !cls) return 0;
    int n = 0;
    unsigned long guard = 0;
    dom_node *stack[256];
    int top = 0;
    for (dom_node *c = root->last_child; c && top < 256; c = c->prev_sibling)
        stack[top++] = c;
    while (top > 0 && n < max && guard++ < (1u << 20)) {
        dom_node *cur = stack[--top];
        if (cur->type == DOM_NODE_ELEMENT) {
            const char *cv = dom_get_attribute(cur, "class");
            if (cv && cls_contains_tok(cv, cls) && n < max) out[n++] = cur;
            for (dom_node *c = cur->last_child; c && top < 256; c = c->prev_sibling)
                stack[top++] = c;
        }
    }
    return n;
}

/* Lowercase a tag string into the VM arena (for getElementsByTagName
 * which accepts "DIV" or "div" indifferently). */
static const char *arena_lower(js_vm *vm, const char *s)
{
    if (!s) return "";
    unsigned long n = str_len(s);
    char *out = (char *)js_arena_alloc(vm, n + 1);
    if (!out) return s;
    for (unsigned long i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = 0;
    return out;
}

/* Uppercase ASCII duplicate into the VM arena. */
static const char *arena_upper(js_vm *vm, const char *s)
{
    if (!s) return "";
    unsigned long n = 0; while (s[n]) n++;
    char *out = (char *)js_arena_alloc(vm, n + 1);
    if (!out) return "";
    for (unsigned long i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    out[n] = 0;
    return out;
}

/* ================================================================== */
/*  Node / element wrappers                                           */
/* ================================================================== */
static js_value wrap_node(js_vm *vm, dom_node *n)
{
    if (!n) return js_mk_null();
    if (n->type == DOM_NODE_ELEMENT)
        return js_native_wrap(vm, g_elem_class_id, n);
    if (n->type == DOM_NODE_TEXT || n->type == DOM_NODE_COMMENT)
        return js_native_wrap(vm, g_text_class_id, n);
    if (n->type == DOM_NODE_DOCUMENT)
        return js_native_wrap(vm, g_doc_class_id, g_doc);
    return js_mk_null();
}

static dom_node *unwrap_node(js_value v)
{
    if (v.type != JS_OBJECT || !v.u.o) return NULL;
    if (!(v.u.o->flags & JS_OBJ_NATIVE)) return NULL;
    int cid = v.u.o->native_class_id;
    if (cid != g_elem_class_id && cid != g_text_class_id) return NULL;
    return (dom_node *)v.u.o->native_ptr;
}

/* Find first child element with the given tag (lowercased). */
static dom_node *find_child_tag_r(dom_node *root, const char *tag, int depth)
{
    if (!root || !tag || depth >= 256) return NULL;
    unsigned long guard = 0;
    for (dom_node *c = root->first_child; c && guard++ < (1u << 20);
         c = c->next_sibling) {
        if (c->type == DOM_NODE_ELEMENT && c->tag && str_eq(c->tag, tag))
            return c;
        dom_node *r = find_child_tag_r(c, tag, depth + 1);
        if (r) return r;
    }
    return NULL;
}

static dom_node *find_child_tag(dom_node *root, const char *tag)
{
    return find_child_tag_r(root, tag, 0);
}

/* ================================================================== */
/*  Element getters                                                   */
/* ================================================================== */
static js_value elem_get(js_vm *vm, void *self_ptr, const char *prop)
{
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return js_native_make_undefined();

    if (str_eq(prop, "tagName") || str_eq(prop, "nodeName")) {
        return js_native_make_string(vm, arena_upper(vm, n->tag ? n->tag : ""));
    }
    if (str_eq(prop, "nodeType")) {
        int t = (n->type == DOM_NODE_ELEMENT) ? 1
              : (n->type == DOM_NODE_TEXT)    ? 3
              : (n->type == DOM_NODE_COMMENT) ? 8
              : 9;
        return js_native_make_number(vm, (double)t);
    }
    if (str_eq(prop, "id")) {
        const char *v = dom_get_attribute(n, "id");
        return js_native_make_string(vm, v ? v : "");
    }
    if (str_eq(prop, "className")) {
        const char *v = dom_get_attribute(n, "class");
        return js_native_make_string(vm, v ? v : "");
    }
    if (str_eq(prop, "textContent") || str_eq(prop, "innerText")) {
        const char *t = dom_get_text(n);
        if (!t) return js_native_make_string(vm, "");
        /* Copy into arena then free the malloc'd text returned by
         * dom_get_text. */
        js_value v = js_native_make_string(vm, t);
        free((void *)t);
        return v;
    }
    if (str_eq(prop, "innerHTML")) {
        /* No HTML serializer yet -- return concatenated text as a
         * conservative approximation. Round-trips set/get for the
         * "text only" case. */
        const char *t = dom_get_text(n);
        js_value v = js_native_make_string(vm, t ? t : "");
        if (t) free((void *)t);
        return v;
    }
    if (str_eq(prop, "parentNode") || str_eq(prop, "parentElement")) {
        if (!n->parent || n->parent->type == DOM_NODE_DOCUMENT) {
            /* parentElement: only if parent is an element. parentNode:
             * may return the document. For simplicity return null when
             * parent is the document root. */
            return js_mk_null();
        }
        return wrap_node(vm, n->parent);
    }
    if (str_eq(prop, "firstChild")) {
        return n->first_child ? wrap_node(vm, n->first_child) : js_mk_null();
    }
    if (str_eq(prop, "lastChild")) {
        return n->last_child ? wrap_node(vm, n->last_child) : js_mk_null();
    }
    if (str_eq(prop, "nextSibling")) {
        return n->next_sibling ? wrap_node(vm, n->next_sibling) : js_mk_null();
    }
    if (str_eq(prop, "previousSibling")) {
        return n->prev_sibling ? wrap_node(vm, n->prev_sibling) : js_mk_null();
    }
    if (str_eq(prop, "children")) {
        /* Element children only (no text/comment). */
        js_object *arr = js_array_new(vm);
        if (!arr) return js_mk_null();
        unsigned long guard = 0;
        for (dom_node *c = n->first_child;
             c && guard++ < (1u << 20);
             c = c->next_sibling) {
            if (c->type == DOM_NODE_ELEMENT)
                js_arr_push(vm, arr, wrap_node(vm, c));
        }
        return js_mk_obj(arr);
    }
    if (str_eq(prop, "childNodes")) {
        js_object *arr = js_array_new(vm);
        if (!arr) return js_mk_null();
        unsigned long guard = 0;
        for (dom_node *c = n->first_child;
             c && guard++ < (1u << 20);
             c = c->next_sibling) {
            js_arr_push(vm, arr, wrap_node(vm, c));
        }
        return js_mk_obj(arr);
    }
    if (str_eq(prop, "childElementCount")) {
        int k = 0;
        unsigned long guard = 0;
        for (dom_node *c = n->first_child;
             c && guard++ < (1u << 20);
             c = c->next_sibling) {
            if (c->type == DOM_NODE_ELEMENT) k++;
        }
        return js_native_make_number(vm, (double)k);
    }

    /* nextElementSibling / previousElementSibling */
    if (str_eq(prop, "nextElementSibling")) {
        dom_node *s = n->next_sibling;
        unsigned long guard = 0;
        while (s && guard++ < (1u << 20)) {
            if (s->type == DOM_NODE_ELEMENT) return wrap_node(vm, s);
            s = s->next_sibling;
        }
        return js_mk_null();
    }
    if (str_eq(prop, "previousElementSibling")) {
        dom_node *s = n->prev_sibling;
        unsigned long guard = 0;
        while (s && guard++ < (1u << 20)) {
            if (s->type == DOM_NODE_ELEMENT) return wrap_node(vm, s);
            s = s->prev_sibling;
        }
        return js_mk_null();
    }

    /* firstElementChild / lastElementChild */
    if (str_eq(prop, "firstElementChild")) {
        dom_node *c = n->first_child;
        unsigned long guard = 0;
        while (c && guard++ < (1u << 20)) {
            if (c->type == DOM_NODE_ELEMENT) return wrap_node(vm, c);
            c = c->next_sibling;
        }
        return js_mk_null();
    }
    if (str_eq(prop, "lastElementChild")) {
        dom_node *c = n->last_child;
        unsigned long guard = 0;
        while (c && guard++ < (1u << 20)) {
            if (c->type == DOM_NODE_ELEMENT) return wrap_node(vm, c);
            c = c->prev_sibling;
        }
        return js_mk_null();
    }

    /* classList: a native wrapper whose self_ptr is the dom_node*,
     * exposing add/remove/toggle/contains/value via the dedicated
     * g_clist_class registered later in this file. */
    if (str_eq(prop, "classList")) {
        return js_native_wrap(vm, g_clist_class_id, n);
    }

    return js_native_make_undefined();
}

/* ================================================================== */
/*  Element setters                                                   */
/* ================================================================== */
static int elem_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return 1;

    if (str_eq(prop, "id")) {
        const char *s = js_native_to_cstr(vm, val);
        dom_set_attribute(n, "id", s ? s : "");
        return 0;
    }
    if (str_eq(prop, "className")) {
        const char *s = js_native_to_cstr(vm, val);
        dom_set_attribute(n, "class", s ? s : "");
        return 0;
    }
    if (str_eq(prop, "textContent") || str_eq(prop, "innerText")) {
        const char *s = js_native_to_cstr(vm, val);
        dom_set_text(n, s ? s : "");
        return 0;
    }
    if (str_eq(prop, "innerHTML")) {
        const char *s = js_native_to_cstr(vm, val);
        if (!s) s = "";
        unsigned long slen = 0; while (s[slen]) slen++;
        dom_node *frag = html_parse_fragment(s, slen);
        if (!frag) {
            /* Parser OOM / unparseable: fall back to plain text so the
             * assignment is never silently dropped. */
            dom_set_text(n, s);
            return 0;
        }
        /* Drop existing children of `n` (purge any listeners first so we
         * don't leak side-table entries / dangling node pointers). */
        while (n->first_child) {
            dom_node *c = n->first_child;
            dom_event_purge_node(c);
            dom_remove_child(n, c);
            dom_node_free(c);
        }
        /* Move the fragment's children into `n`. dom_append_child detaches
         * each child from `frag` first, so capture the next sibling before
         * the move (the link is rewritten by the append). */
        for (dom_node *c = frag->first_child; c; ) {
            dom_node *next = c->next_sibling;
            dom_append_child(n, c);
            c = next;
        }
        /* `frag` is now an empty wrapper element; free it. */
        dom_node_free(frag);
        return 0;
    }
    /* tagName / parent / sibling pointers / nodeType are read-only --
     * silently no-op (matches browser behaviour for these). */
    if (str_eq(prop, "tagName")    || str_eq(prop, "nodeName")    ||
        str_eq(prop, "nodeType")   || str_eq(prop, "parentNode")  ||
        str_eq(prop, "parentElement") || str_eq(prop, "firstChild") ||
        str_eq(prop, "lastChild")  || str_eq(prop, "nextSibling") ||
        str_eq(prop, "previousSibling") || str_eq(prop, "children") ||
        str_eq(prop, "childNodes") || str_eq(prop, "childElementCount") ||
        str_eq(prop, "nextElementSibling") ||
        str_eq(prop, "previousElementSibling") ||
        str_eq(prop, "firstElementChild") ||
        str_eq(prop, "lastElementChild") ||
        str_eq(prop, "classList")) {
        return 0;
    }
    return 1;  /* fall through to own-prop store */
}

/* ================================================================== */
/*  Element methods                                                   */
/* ================================================================== */
static js_value m_getAttribute(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_null();
    const char *name = js_native_to_cstr(vm, argv[0]);
    if (!name) return js_mk_null();
    const char *v = dom_get_attribute(n, name);
    if (!v) return js_mk_null();
    return js_native_make_string(vm, v);
}

static js_value m_setAttribute(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 2) return js_mk_undef();
    const char *name = js_native_to_cstr(vm, argv[0]);
    const char *val  = js_native_to_cstr(vm, argv[1]);
    if (name) dom_set_attribute(n, name, val ? val : "");
    return js_mk_undef();
}

static js_value m_hasAttribute(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_bool(0);
    const char *name = js_native_to_cstr(vm, argv[0]);
    if (!name) return js_mk_bool(0);
    return js_mk_bool(dom_has_attribute(n, name) ? 1 : 0);
}

static js_value m_removeAttribute(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_undef();
    const char *name = js_native_to_cstr(vm, argv[0]);
    if (name) dom_remove_attribute(n, name);
    return js_mk_undef();
}

static js_value m_appendChild(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm;
    dom_node *parent = (dom_node *)self;
    if (!parent || argc < 1) return js_mk_null();
    dom_node *child = unwrap_node(argv[0]);
    if (!child) return js_mk_null();
    dom_append_child(parent, child);
    return argv[0];                /* return the appended child */
}

static js_value m_removeChild(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm;
    dom_node *parent = (dom_node *)self;
    if (!parent || argc < 1) return js_mk_null();
    dom_node *child = unwrap_node(argv[0]);
    if (!child || child->parent != parent) return js_mk_null();
    dom_remove_child(parent, child);
    return argv[0];
}

static js_value m_insertBefore(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm;
    dom_node *parent = (dom_node *)self;
    if (!parent || argc < 1) return js_mk_null();
    dom_node *child = unwrap_node(argv[0]);
    dom_node *ref   = (argc >= 2) ? unwrap_node(argv[1]) : NULL;
    if (!child) return js_mk_null();
    dom_insert_before(parent, child, ref);
    return argv[0];
}

/* Resolve the dom_node search root for a querySelector* `self`. The
 * method table is shared between Element wrappers (self == dom_node*)
 * and the Document wrapper (self == dom_document*). Distinguish by the
 * document's stable identity. */
static dom_node *qs_root(void *self)
{
    if (!self) return NULL;
    if (self == (void *)g_doc) return g_doc ? g_doc->root : NULL;
    return (dom_node *)self;
}

/* querySelector / querySelectorAll: backed by dom_selector. */
static js_value m_querySelector(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *root = qs_root(self);
    if (!root || argc < 1) return js_mk_null();
    const char *sel = js_native_to_cstr(vm, argv[0]);
    if (!sel) return js_mk_null();
    dom_node *hit = dom_query_selector(root, sel);
    return hit ? wrap_node(vm, hit) : js_mk_null();
}

/* Upper bound on matches returned by a single querySelectorAll call. */
#define DOM_QSA_MAX 256

static js_value m_querySelectorAll(js_vm *vm, void *self, int argc, js_value *argv)
{
    js_object *arr = js_array_new(vm);
    if (!arr) return js_mk_null();
    dom_node *root = qs_root(self);
    if (!root || argc < 1) return js_mk_obj(arr);
    const char *sel = js_native_to_cstr(vm, argv[0]);
    if (!sel) return js_mk_obj(arr);

    dom_node *hits[DOM_QSA_MAX];
    int n = dom_query_selector_all(root, sel, hits, DOM_QSA_MAX);
    for (int i = 0; i < n; i++)
        js_arr_push(vm, arr, wrap_node(vm, hits[i]));
    return js_mk_obj(arr);
}

/* Upper bound for getElementsByTagName/ClassName results. */
#define DOM_GETE_MAX 256

/* element.getElementsByTagName(tag) */
static js_value m_getElementsByTagName(js_vm *vm, void *self, int argc,
                                       js_value *argv)
{
    js_object *arr = js_array_new(vm);
    if (!arr) return js_mk_null();
    dom_node *root = qs_root(self);
    if (!root || argc < 1) return js_mk_obj(arr);
    const char *tag = js_native_to_cstr(vm, argv[0]);
    if (!tag) return js_mk_obj(arr);
    const char *ltag = arena_lower(vm, tag);
    dom_node *hits[DOM_GETE_MAX];
    int n = collect_by_tag(root, ltag, hits, DOM_GETE_MAX);
    for (int i = 0; i < n; i++)
        js_arr_push(vm, arr, wrap_node(vm, hits[i]));
    return js_mk_obj(arr);
}

/* element.getElementsByClassName(cls) */
static js_value m_getElementsByClassName(js_vm *vm, void *self, int argc,
                                         js_value *argv)
{
    js_object *arr = js_array_new(vm);
    if (!arr) return js_mk_null();
    dom_node *root = qs_root(self);
    if (!root || argc < 1) return js_mk_obj(arr);
    const char *cls = js_native_to_cstr(vm, argv[0]);
    if (!cls) return js_mk_obj(arr);
    dom_node *hits[DOM_GETE_MAX];
    int n = collect_by_class(root, cls, hits, DOM_GETE_MAX);
    for (int i = 0; i < n; i++)
        js_arr_push(vm, arr, wrap_node(vm, hits[i]));
    return js_mk_obj(arr);
}

/* element.getAttributeNames() -- returns array of attribute name strings. */
static js_value m_getAttributeNames(js_vm *vm, void *self, int argc,
                                    js_value *argv)
{
    (void)argc; (void)argv;
    dom_node *n = (dom_node *)self;
    js_object *arr = js_array_new(vm);
    if (!arr) return js_mk_null();
    if (!n) return js_mk_obj(arr);
    unsigned long guard = 0;
    for (dom_attr *a = n->attrs; a && guard++ < 1024; a = a->next) {
        if (a->name)
            js_arr_push(vm, arr, js_native_make_string(vm, a->name));
    }
    return js_mk_obj(arr);
}

/* element.matches(selector) -- returns true if this element matches. */
static js_value m_matches(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_bool(0);
    const char *sel = js_native_to_cstr(vm, argv[0]);
    if (!sel) return js_mk_bool(0);
    int r = dom_selector_match(n, sel);
    return js_mk_bool(r > 0 ? 1 : 0);
}

/* element.closest(selector) -- walk ancestors (including self) for first match. */
static js_value m_closest(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_null();
    const char *sel = js_native_to_cstr(vm, argv[0]);
    if (!sel) return js_mk_null();
    unsigned long guard = 0;
    dom_node *cur = n;
    while (cur && cur->type == DOM_NODE_ELEMENT && guard++ < (1u << 20)) {
        if (dom_selector_match(cur, sel) > 0)
            return wrap_node(vm, cur);
        cur = cur->parent;
    }
    return js_mk_null();
}

/* element.remove() -- detach from parent. */
static js_value m_remove(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm; (void)argc; (void)argv;
    dom_node *n = (dom_node *)self;
    if (n && n->parent)
        dom_remove_child(n->parent, n);
    return js_mk_undef();
}

/* element.cloneNode(deep) -- shallow or deep clone.
 * Returns the cloned node as a JS wrapper. */
static js_value m_cloneNode(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n) return js_mk_null();
    int deep = (argc >= 1) ? (js_truthy(argv[0]) ? 1 : 0) : 0;
    dom_node *clone = dom_clone_node(n, deep);
    if (!clone) return js_mk_null();
    return wrap_node(vm, clone);
}

/* ================================================================== */
/*  classList native class                                            */
/* ================================================================== */
/*
 * classList is a native wrapper whose self_ptr is the dom_node* of the
 * owning element. The class exposes:
 *   .contains(cls)     -> boolean
 *   .add(cls)          -> undefined (mutates class attr)
 *   .remove(cls)       -> undefined (mutates class attr)
 *   .toggle(cls[,force]) -> boolean (new contains state)
 *   .value             -> full class attribute string (getter)
 */
static js_value clist_get(js_vm *vm, void *self_ptr, const char *prop)
{
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return js_native_make_undefined();
    if (str_eq(prop, "value")) {
        const char *v = dom_get_attribute(n, "class");
        return js_native_make_string(vm, v ? v : "");
    }
    return js_native_make_undefined();
}

static int clist_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    (void)vm; (void)val;
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return 1;
    if (str_eq(prop, "value")) {
        const char *s = js_native_to_cstr(vm, val);
        dom_set_attribute(n, "class", s ? s : "");
        return 0;
    }
    return 0; /* treat all other assignments as no-op (read-only) */
}

static js_value m_clist_contains(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_bool(0);
    const char *tok = js_native_to_cstr(vm, argv[0]);
    if (!tok) return js_mk_bool(0);
    const char *cls = dom_get_attribute(n, "class");
    return js_mk_bool(cls_contains_tok(cls ? cls : "", tok) ? 1 : 0);
}

static js_value m_clist_add(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n) return js_mk_undef();
    for (int i = 0; i < argc; i++) {
        const char *tok = js_native_to_cstr(vm, argv[i]);
        if (!tok) continue;
        const char *cls = dom_get_attribute(n, "class");
        const char *newcls = cls_add_tok(vm, cls ? cls : "", tok);
        if (newcls) dom_set_attribute(n, "class", newcls);
    }
    return js_mk_undef();
}

static js_value m_clist_remove(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n) return js_mk_undef();
    for (int i = 0; i < argc; i++) {
        const char *tok = js_native_to_cstr(vm, argv[i]);
        if (!tok) continue;
        const char *cls = dom_get_attribute(n, "class");
        const char *newcls = cls_remove_tok(vm, cls ? cls : "", tok);
        if (newcls) dom_set_attribute(n, "class", newcls);
    }
    return js_mk_undef();
}

/* toggle(cls [, force]): if force is given use it; otherwise flip.
 * Returns new contains state. */
static js_value m_clist_toggle(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_bool(0);
    const char *tok = js_native_to_cstr(vm, argv[0]);
    if (!tok) return js_mk_bool(0);
    const char *cls = dom_get_attribute(n, "class");
    if (!cls) cls = "";
    int has = cls_contains_tok(cls, tok);
    int add_it;
    if (argc >= 2) {
        add_it = js_truthy(argv[1]) ? 1 : 0;
    } else {
        add_it = !has;
    }
    if (add_it && !has) {
        const char *newcls = cls_add_tok(vm, cls, tok);
        if (newcls) dom_set_attribute(n, "class", newcls);
    } else if (!add_it && has) {
        const char *newcls = cls_remove_tok(vm, cls, tok);
        if (newcls) dom_set_attribute(n, "class", newcls);
    }
    return js_mk_bool(add_it ? 1 : 0);
}

static const struct { const char *name; js_native_method fn; }
clist_methods[] = {
    { "contains", m_clist_contains },
    { "add",      m_clist_add      },
    { "remove",   m_clist_remove   },
    { "toggle",   m_clist_toggle   },
    { NULL, NULL }
};

static const js_native_class g_clist_class = {
    "DOMTokenList",
    clist_get,
    clist_set,
    (const void *)clist_methods,
};

/* ================================================================== */
/*  Events  (addEventListener / dispatchEvent)                         */
/* ================================================================== */
/*
 * dom_event carries an opaque `void *user` to each C callback but no
 * place to stash a JS value. We therefore keep a fixed side-table of
 * (node, type) -> JS callback. dom_add_event_listener gets a single
 * shared C trampoline plus the slot address as `user`; the trampoline
 * recovers the JS function from the slot, wraps the live dom_event_t in
 * a JS Event object, and invokes the handler via js_call_function. Slots
 * live for the VM lifetime; the JS callback itself lives in the arena, so
 * listeners are only callable within the same eval/arena epoch in which
 * they were registered (the documented lifecycle: install/parse/run
 * between arena resets -- dom_bindings_install() clears the table to mark
 * a new epoch).
 *
 * Capacity: 512 slots. Each slot is ~48 bytes, so the whole table is
 * ~24 KB of BSS -- cheap, and large enough that realistic pages (which
 * rarely exceed a few hundred live handlers per script epoch) never hit
 * the cap and silently drop a listener. The previous 128 was observably
 * too small for content-heavy pages that wire one handler per row/cell.
 */
#define DOM_EVT_SLOTS 512
typedef struct {
    int       used;
    dom_node *node;
    char      type[32];
    js_value  cb;             /* JS function value (arena-resident) */
} dom_evt_slot;

static dom_evt_slot g_evt_slots[DOM_EVT_SLOTS];

/* Copy a NUL-terminated type string into a fixed buffer (truncating). */
static void evt_copy_type(char *dst, const char *src)
{
    int i = 0;
    if (src) {
        for (; i < 31 && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

/* ------------------------------------------------------------------ */
/*  JS Event object                                                    */
/* ------------------------------------------------------------------ */
/*
 * The Event passed to a JS handler is a native wrapper whose native_ptr
 * is the live dom_event_t* currently being dispatched (stack-allocated in
 * m_dispatchEvent / dom_dispatch_event). Because dispatch is fully
 * synchronous -- the handler runs inside dom_dispatch_event_full, which
 * is itself called while that dom_event_t is still on the stack -- the
 * pointer is valid for the entire duration of the handler call. The
 * wrapper is not meant to outlive dispatch (it lives in the arena and is
 * a fresh wrapper per handler invocation); stashing it on a JS global and
 * reading it after dispatch returns is unsupported, matching the existing
 * node-wrapper lifecycle.
 *
 * preventDefault() / stopPropagation() are native methods: the engine's
 * trampoline recovers `self` from `this.native_ptr` (the dom_event_t*),
 * so they mutate the very event struct the dispatcher is reading between
 * nodes. That is how stopPropagation halts the capture/bubble walk and
 * how preventDefault makes dispatchEvent return false.
 */
static js_value event_get(js_vm *vm, void *self_ptr, const char *prop)
{
    dom_event_t *ev = (dom_event_t *)self_ptr;
    if (!ev) return js_native_make_undefined();

    if (str_eq(prop, "type")) {
        return js_native_make_string(vm, ev->type);
    }
    if (str_eq(prop, "target") || str_eq(prop, "srcElement")) {
        return ev->target ? wrap_node(vm, ev->target) : js_mk_null();
    }
    if (str_eq(prop, "currentTarget")) {
        return ev->current ? wrap_node(vm, ev->current) : js_mk_null();
    }
    if (str_eq(prop, "eventPhase")) {
        return js_native_make_number(vm, (double)ev->phase);
    }
    if (str_eq(prop, "defaultPrevented")) {
        return js_native_make_bool(vm, ev->default_prevented ? 1 : 0);
    }
    if (str_eq(prop, "bubbles") || str_eq(prop, "cancelable")) {
        /* This implementation dispatches every event through the full
         * capture/target/bubble walk and honours preventDefault, so both
         * are conceptually true. */
        return js_native_make_bool(vm, 1);
    }
    return js_native_make_undefined();
}

/* Event properties are all derived from the dom_event_t; assignment is a
 * silent no-op (matches browsers: e.type = ... does nothing) so a script
 * doing `e.foo = 1` still gets a normal own-prop store via the +1 path. */
static int event_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    (void)vm; (void)val;
    dom_event_t *ev = (dom_event_t *)self_ptr;
    if (!ev) return 1;
    if (str_eq(prop, "type")          || str_eq(prop, "target")     ||
        str_eq(prop, "srcElement")    || str_eq(prop, "currentTarget") ||
        str_eq(prop, "eventPhase")    || str_eq(prop, "defaultPrevented") ||
        str_eq(prop, "bubbles")       || str_eq(prop, "cancelable")) {
        return 0;  /* recognised + read-only */
    }
    return 1;      /* unknown: fall through to own-prop store */
}

/* e.preventDefault() -- set the cancelled flag on the live event. */
static js_value m_preventDefault(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm; (void)argc; (void)argv;
    dom_event_t *ev = (dom_event_t *)self;
    if (ev) ev->default_prevented = 1;
    return js_mk_undef();
}

/* e.stopPropagation() -- halt the capture/bubble walk after this node.
 * The dispatcher re-reads ev->stop_propagation between nodes/phases. */
static js_value m_stopPropagation(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm; (void)argc; (void)argv;
    dom_event_t *ev = (dom_event_t *)self;
    if (ev) ev->stop_propagation = 1;
    return js_mk_undef();
}

/* stopImmediatePropagation: we don't yet halt *within* a single node's
 * listener list, but stopping further nodes is the dominant observable
 * effect, so alias it to stopPropagation rather than expose nothing. */
static js_value m_stopImmediatePropagation(js_vm *vm, void *self, int argc,
                                           js_value *argv)
{
    return m_stopPropagation(vm, self, argc, argv);
}

static const struct { const char *name; js_native_method fn; }
event_methods[] = {
    { "preventDefault",          m_preventDefault          },
    { "stopPropagation",         m_stopPropagation         },
    { "stopImmediatePropagation",m_stopImmediatePropagation },
    { NULL, NULL }
};

static const js_native_class g_event_class = {
    "Event",
    event_get,
    event_set,
    (const void *)event_methods,
};

/* The single C callback registered with dom_event for every JS listener.
 * `user` is the dom_evt_slot* holding the JS function to invoke. */
static void evt_trampoline(dom_event_t *ev, void *user)
{
    dom_evt_slot *slot = (dom_evt_slot *)user;
    if (!slot || !slot->used || !g_vm) return;
    if (slot->cb.type != JS_FUNCTION) return;
    /* Wrap the live event in a JS Event object exposing .type/.target/
     * .currentTarget/.defaultPrevented plus preventDefault()/
     * stopPropagation(). If the Event class is unavailable (should not
     * happen post-install) or wrapping fails, fall back to a bare target
     * wrapper so the handler still receives *something* usable. */
    js_value arg;
    if (g_event_class_id > 0) {
        arg = js_native_wrap(g_vm, g_event_class_id, ev);
        if (arg.type != JS_OBJECT) {
            arg = (ev && ev->target) ? wrap_node(g_vm, ev->target)
                                     : js_mk_null();
        }
    } else {
        arg = (ev && ev->target) ? wrap_node(g_vm, ev->target) : js_mk_null();
    }
    js_value ret;
    (void)js_call_function(g_vm, slot->cb, js_mk_null(), &arg, 1, &ret);
}

/* element.addEventListener(type, handler [, capture]) */
static js_value m_addEventListener(js_vm *vm, void *self, int argc,
                                   js_value *argv)
{
    (void)vm;
    dom_node *n = (dom_node *)self;
    if (!n || argc < 2) return js_mk_undef();
    if (argv[1].type != JS_FUNCTION) return js_mk_undef();
    const char *type = js_native_to_cstr(vm, argv[0]);
    if (!type) return js_mk_undef();
    int capture = (argc >= 3 && js_truthy(argv[2])) ? 1 : 0;

    /* Find a free slot. */
    int idx = -1;
    for (int i = 0; i < DOM_EVT_SLOTS; i++) {
        if (!g_evt_slots[i].used) { idx = i; break; }
    }
    if (idx < 0) return js_mk_undef();      /* table full: drop silently */

    g_evt_slots[idx].used = 1;
    g_evt_slots[idx].node = n;
    evt_copy_type(g_evt_slots[idx].type, type);
    g_evt_slots[idx].cb   = argv[1];

    if (dom_add_event_listener(n, type, evt_trampoline,
                               &g_evt_slots[idx], capture) != 0) {
        g_evt_slots[idx].used = 0;          /* registration failed: free */
    }
    return js_mk_undef();
}

/* element.dispatchEvent(typeOrEvent) -- accepts a string type or an
 * object with a `.type` string property. Returns true (not cancelled),
 * mirroring DOM dispatchEvent's boolean when nothing prevents default. */
static js_value m_dispatchEvent(js_vm *vm, void *self, int argc,
                                js_value *argv)
{
    dom_node *n = (dom_node *)self;
    if (!n || argc < 1) return js_mk_bool(0);

    const char *type = NULL;
    if (argv[0].type == JS_STRING) {
        type = js_native_to_cstr(vm, argv[0]);
    } else if (argv[0].type == JS_OBJECT && argv[0].u.o) {
        js_string *k = js_str_intern(vm, "type", 4);
        js_value tv;
        if (k && js_obj_get(vm, argv[0].u.o, k, &tv) && tv.type == JS_STRING)
            type = js_native_to_cstr(vm, tv);
    }
    if (!type) return js_mk_bool(0);

    dom_event_t ev;
    js_memset(&ev, 0, sizeof ev);
    evt_copy_type(ev.type, type);
    ev.target = n;
    dom_dispatch_event_full(n, &ev);
    return js_mk_bool(ev.default_prevented ? 0 : 1);
}

static const struct { const char *name; js_native_method fn; }
elem_methods[] = {
    { "getAttribute",          m_getAttribute          },
    { "setAttribute",          m_setAttribute          },
    { "hasAttribute",          m_hasAttribute          },
    { "removeAttribute",       m_removeAttribute       },
    { "getAttributeNames",     m_getAttributeNames     },
    { "appendChild",           m_appendChild           },
    { "removeChild",           m_removeChild           },
    { "insertBefore",          m_insertBefore          },
    { "remove",                m_remove                },
    { "querySelector",         m_querySelector         },
    { "querySelectorAll",      m_querySelectorAll      },
    { "getElementsByTagName",  m_getElementsByTagName  },
    { "getElementsByClassName",m_getElementsByClassName},
    { "matches",               m_matches               },
    { "closest",               m_closest               },
    { "cloneNode",             m_cloneNode             },
    { "addEventListener",      m_addEventListener      },
    { "dispatchEvent",         m_dispatchEvent         },
    { NULL, NULL }
};

static const js_native_class g_elem_class = {
    "Element",
    elem_get,
    elem_set,
    /* Cast: the public method table type is declared as an inline anonymous
     * struct -- our local definition has the same layout but a different
     * anonymous tag. The cast is safe. */
    (const void *)elem_methods,
};

/* ================================================================== */
/*  TextNode                                                          */
/* ================================================================== */
static js_value text_get(js_vm *vm, void *self_ptr, const char *prop)
{
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return js_native_make_undefined();
    if (str_eq(prop, "textContent") || str_eq(prop, "data") ||
        str_eq(prop, "nodeValue")) {
        return js_native_make_string(vm, n->text ? n->text : "");
    }
    if (str_eq(prop, "nodeType")) {
        return js_native_make_number(vm,
            n->type == DOM_NODE_COMMENT ? 8.0 : 3.0);
    }
    if (str_eq(prop, "nodeName")) {
        return js_native_make_string(vm,
            n->type == DOM_NODE_COMMENT ? "#comment" : "#text");
    }
    if (str_eq(prop, "parentNode")) {
        if (!n->parent || n->parent->type == DOM_NODE_DOCUMENT)
            return js_mk_null();
        return wrap_node(vm, n->parent);
    }
    if (str_eq(prop, "nextSibling"))
        return n->next_sibling ? wrap_node(vm, n->next_sibling) : js_mk_null();
    if (str_eq(prop, "previousSibling"))
        return n->prev_sibling ? wrap_node(vm, n->prev_sibling) : js_mk_null();
    return js_native_make_undefined();
}

static int text_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    dom_node *n = (dom_node *)self_ptr;
    if (!n) return 1;
    if (str_eq(prop, "textContent") || str_eq(prop, "data") ||
        str_eq(prop, "nodeValue")) {
        const char *s = js_native_to_cstr(vm, val);
        dom_set_text(n, s ? s : "");
        return 0;
    }
    return 1;
}

static const struct { const char *name; js_native_method fn; }
text_methods[] = {
    { NULL, NULL }
};

static const js_native_class g_text_class = {
    "Text",
    text_get,
    text_set,
    (const void *)text_methods,
};

/* ================================================================== */
/*  Document                                                          */
/* ================================================================== */
static dom_node *doc_html_root(dom_document *doc)
{
    if (!doc || !doc->root) return NULL;
    /* The root document holds one child conventionally: <html>. */
    return doc->root->first_child;
}

static js_value doc_get(js_vm *vm, void *self_ptr, const char *prop)
{
    dom_document *doc = (dom_document *)self_ptr;
    if (!doc) return js_native_make_undefined();
    if (str_eq(prop, "documentElement")) {
        dom_node *h = doc_html_root(doc);
        return h ? wrap_node(vm, h) : js_mk_null();
    }
    if (str_eq(prop, "body")) {
        dom_node *h = doc_html_root(doc);
        if (!h) return js_mk_null();
        dom_node *b = find_child_tag(h, "body");
        return b ? wrap_node(vm, b) : js_mk_null();
    }
    if (str_eq(prop, "head")) {
        dom_node *h = doc_html_root(doc);
        if (!h) return js_mk_null();
        dom_node *hd = find_child_tag(h, "head");
        return hd ? wrap_node(vm, hd) : js_mk_null();
    }
    if (str_eq(prop, "title")) {
        dom_node *h = doc_html_root(doc);
        if (!h) return js_native_make_string(vm, "");
        dom_node *t = find_child_tag(h, "title");
        if (!t) return js_native_make_string(vm, "");
        const char *txt = dom_get_text(t);
        js_value v = js_native_make_string(vm, txt ? txt : "");
        if (txt) free((void *)txt);
        return v;
    }
    if (str_eq(prop, "URL") || str_eq(prop, "documentURI")) {
        return js_native_make_string(vm, doc->url ? doc->url : "");
    }
    if (str_eq(prop, "nodeType"))
        return js_native_make_number(vm, 9.0);
    if (str_eq(prop, "nodeName"))
        return js_native_make_string(vm, "#document");
    return js_native_make_undefined();
}

static int doc_set(js_vm *vm, void *self_ptr, const char *prop, js_value val)
{
    dom_document *doc = (dom_document *)self_ptr;
    if (!doc) return 1;
    if (str_eq(prop, "title")) {
        dom_node *h = doc_html_root(doc);
        if (!h) return 0;
        dom_node *head = find_child_tag(h, "head");
        if (!head) {
            head = dom_create_element("head");
            if (head) {
                /* head should precede body */
                dom_insert_before(h, head, h->first_child);
            }
        }
        if (!head) return 0;
        dom_node *t = find_child_tag(head, "title");
        if (!t) {
            t = dom_create_element("title");
            if (t) dom_append_child(head, t);
        }
        if (t) {
            const char *s = js_native_to_cstr(vm, val);
            dom_set_text(t, s ? s : "");
        }
        return 0;
    }
    /* All other props are read-only at this level. */
    return 1;
}

static js_value m_getElementById(js_vm *vm, void *self, int argc, js_value *argv)
{
    dom_document *doc = (dom_document *)self;
    if (!doc || argc < 1) return js_mk_null();
    const char *id = js_native_to_cstr(vm, argv[0]);
    if (!id) return js_mk_null();
    dom_node *n = dom_get_element_by_id(doc, id);
    if (!n) return js_mk_null();
    return wrap_node(vm, n);
}

static js_value m_createElement(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)self;
    if (argc < 1) return js_mk_null();
    const char *tag = js_native_to_cstr(vm, argv[0]);
    if (!tag) return js_mk_null();
    dom_node *n = dom_create_element(tag);
    if (!n) return js_mk_null();
    return wrap_node(vm, n);
}

static js_value m_createTextNode(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)self;
    const char *t = (argc >= 1) ? js_native_to_cstr(vm, argv[0]) : "";
    dom_node *n = dom_create_text(t ? t : "");
    if (!n) return js_mk_null();
    return wrap_node(vm, n);
}

static js_value m_createComment(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)self;
    const char *t = (argc >= 1) ? js_native_to_cstr(vm, argv[0]) : "";
    dom_node *n = dom_create_comment(t ? t : "");
    if (!n) return js_mk_null();
    return wrap_node(vm, n);
}

/* document.addEventListener / dispatchEvent target the document root
 * node. `self` here is a dom_document*; rebind to its root and reuse the
 * element-level implementations. */
static js_value m_doc_addEventListener(js_vm *vm, void *self, int argc,
                                       js_value *argv)
{
    dom_document *doc = (dom_document *)self;
    if (!doc || !doc->root) return js_mk_undef();
    return m_addEventListener(vm, doc->root, argc, argv);
}

static js_value m_doc_dispatchEvent(js_vm *vm, void *self, int argc,
                                    js_value *argv)
{
    dom_document *doc = (dom_document *)self;
    if (!doc || !doc->root) return js_mk_bool(0);
    return m_dispatchEvent(vm, doc->root, argc, argv);
}

static const struct { const char *name; js_native_method fn; }
doc_methods[] = {
    { "getElementById",         m_getElementById         },
    { "createElement",          m_createElement          },
    { "createTextNode",         m_createTextNode         },
    { "createComment",          m_createComment          },
    { "querySelector",          m_querySelector          },
    { "querySelectorAll",       m_querySelectorAll       },
    { "getElementsByTagName",   m_getElementsByTagName   },
    { "getElementsByClassName", m_getElementsByClassName },
    { "addEventListener",       m_doc_addEventListener   },
    { "dispatchEvent",          m_doc_dispatchEvent      },
    { NULL, NULL }
};

static const js_native_class g_doc_class = {
    "Document",
    doc_get,
    doc_set,
    (const void *)doc_methods,
};

/* ================================================================== */
/*  Install                                                           */
/* ================================================================== */
void dom_bindings_install(js_vm *vm, dom_document *doc)
{
    if (!vm || !doc) return;
    g_doc = doc;
    g_vm  = vm;

    /* A new install marks a new arena epoch: any JS callbacks captured in
     * the event side-table from a prior epoch now dangle, so clear them. */
    for (int i = 0; i < DOM_EVT_SLOTS; i++) g_evt_slots[i].used = 0;

    g_doc_class_id   = js_native_register_class(vm, &g_doc_class);
    g_elem_class_id  = js_native_register_class(vm, &g_elem_class);
    g_text_class_id  = js_native_register_class(vm, &g_text_class);
    /* The Event class is wrapped internally per-dispatch (no JS-visible
     * global constructor yet); we only need its class id to wrap the live
     * dom_event_t when invoking handlers. */
    g_event_class_id = js_native_register_class(vm, &g_event_class);
    g_clist_class_id = js_native_register_class(vm, &g_clist_class);

    /* Bind the document as the JS `document` global. */
    js_value docv = js_native_wrap(vm, g_doc_class_id, doc);
    js_native_register_global_value(vm, "document", docv);
}

/* ================================================================== */
/*  Selftest                                                          */
/* ================================================================== */
/* Build a baseline empty document with <html><head></head><body></body>
 * so the selftest's `document.body.appendChild(...)` has somewhere to go. */
static dom_document *build_blank_doc(void)
{
    dom_document *doc = dom_document_new();
    if (!doc) return NULL;
    dom_node *html = dom_create_element("html");
    dom_node *head = dom_create_element("head");
    dom_node *body = dom_create_element("body");
    if (!html || !head || !body) {
        if (html) dom_node_free(html);
        if (head) dom_node_free(head);
        if (body) dom_node_free(body);
        dom_document_free(doc);
        return NULL;
    }
    dom_append_child(doc->root, html);
    dom_append_child(html, head);
    dom_append_child(html, body);
    return doc;
}

/* Forward-declare engine entry points the selftest uses directly. We
 * cannot call js_eval -- that would reset the arena and wipe our
 * bindings. */
extern js_node *js_parse_program(js_vm *vm, const char *src, js_usize len);
extern int     js_run_program(js_vm *vm, js_node *prog, js_value *completion);

int dom_bindings_selftest(js_vm *vm)
{
    if (!vm) return -1;
    dom_document *doc = build_blank_doc();
    if (!doc) return -2;

    dom_bindings_install(vm, doc);

    const char *src =
        "var a = document.createElement('div');"
        "a.id = 'x';"
        "a.textContent = 'hello';"
        "document.body.appendChild(a);"
        "document.getElementById('x').textContent;";

    js_node *prog = js_parse_program(vm, src, js_strlen(src));
    if (!prog) {
        dom_document_free(doc);
        return -3;
    }
    js_value completion;
    int rc = js_run_program(vm, prog, &completion);
    if (rc < 0) {
        dom_document_free(doc);
        return -4;
    }
    if (completion.type != JS_STRING || !completion.u.s) {
        dom_document_free(doc);
        return -5;
    }
    if (completion.u.s->len != 5 ||
        completion.u.s->data[0] != 'h' ||
        completion.u.s->data[1] != 'e' ||
        completion.u.s->data[2] != 'l' ||
        completion.u.s->data[3] != 'l' ||
        completion.u.s->data[4] != 'o') {
        dom_document_free(doc);
        return -6;
    }

    /* Also verify the node was inserted into the live DOM, not just the
     * JS-side cache: look it up via the C API. */
    dom_node *inserted = dom_get_element_by_id(doc, "x");
    if (!inserted) { dom_document_free(doc); return -7; }
    const char *txt = dom_get_text(inserted);
    if (!txt || !str_eq(txt, "hello")) {
        if (txt) free((void *)txt);
        dom_document_free(doc);
        return -8;
    }
    free((void *)txt);

    /* Test innerHTML round-trip for the plain-text case. */
    const char *src2 =
        "var b = document.createElement('span');"
        "b.innerHTML = 'world';"
        "document.body.appendChild(b);"
        "b.textContent;";
    js_node *p2 = js_parse_program(vm, src2, js_strlen(src2));
    if (p2) {
        js_value c2;
        if (js_run_program(vm, p2, &c2) == 0 &&
            c2.type == JS_STRING && c2.u.s && c2.u.s->len == 5 &&
            c2.u.s->data[0] == 'w') {
            /* ok */
        } else {
            dom_document_free(doc);
            return -9;
        }
    }

    /* querySelector('#x') must return the element we inserted above
     * (id=x, textContent "hello"); confirm via its textContent. */
    const char *src3 =
        "var q = document.querySelector('#x');"
        "q ? q.textContent : '';";
    js_node *p3 = js_parse_program(vm, src3, js_strlen(src3));
    if (!p3) { dom_document_free(doc); return -10; }
    {
        js_value c3;
        if (js_run_program(vm, p3, &c3) != 0 ||
            c3.type != JS_STRING || !c3.u.s ||
            c3.u.s->len != 5 || c3.u.s->data[0] != 'h') {
            dom_document_free(doc);
            return -11;
        }
    }

    /* innerHTML with real markup must build child ELEMENTS, not text.
     * After `c.innerHTML = '<p>hi</p><p>yo</p>'` we expect:
     *   c.childElementCount === 2
     *   c.firstChild.tagName === 'P'
     * Returned string "2|P" proves both. */
    const char *src4 =
        "var c = document.createElement('div');"
        "c.id = 'frag';"
        "document.body.appendChild(c);"
        "c.innerHTML = '<p>hi</p><p>yo</p>';"
        "'' + c.childElementCount + '|' + c.firstChild.tagName;";
    js_node *p4 = js_parse_program(vm, src4, js_strlen(src4));
    if (!p4) { dom_document_free(doc); return -12; }
    {
        js_value c4;
        if (js_run_program(vm, p4, &c4) != 0 ||
            c4.type != JS_STRING || !c4.u.s ||
            !str_eq(c4.u.s->data, "2|P")) {
            dom_document_free(doc);
            return -13;
        }
    }
    /* Cross-check the live DOM: the 'frag' div really holds two <p>
     * element children (not a single text node). */
    {
        dom_node *fr = dom_get_element_by_id(doc, "frag");
        if (!fr) { dom_document_free(doc); return -14; }
        int kids = 0;
        for (dom_node *k = fr->first_child; k; k = k->next_sibling) {
            if (k->type == DOM_NODE_ELEMENT && k->tag && str_eq(k->tag, "p"))
                kids++;
        }
        if (kids != 2) { dom_document_free(doc); return -15; }
    }

    /* Event round-trip: addEventListener stores a JS callback that flips a
     * global flag; dispatchEvent must invoke it. The handler also reads the
     * Event object it is handed -- e.type must be 'ping' and e.target must
     * be the element it was dispatched on (id 'x'). The completion string
     * "1|ping|x" proves the listener fired AND the Event API is live. */
    const char *src5 =
        "var fired = 0; var etype = ''; var eid = '';"
        "var t = document.getElementById('x');"
        "t.addEventListener('ping', function(e){"
        "  fired = 1; etype = e.type; eid = e.target.id;"
        "});"
        "t.dispatchEvent('ping');"
        "'' + fired + '|' + etype + '|' + eid;";
    js_node *p5 = js_parse_program(vm, src5, js_strlen(src5));
    if (!p5) { dom_document_free(doc); return -16; }
    {
        js_value c5;
        if (js_run_program(vm, p5, &c5) != 0) {
            dom_document_free(doc);
            return -17;
        }
        if (c5.type != JS_STRING || !c5.u.s ||
            !str_eq(c5.u.s->data, "1|ping|x")) {
            dom_document_free(doc);
            return -18;
        }
    }

    /* preventDefault(): a handler that calls e.preventDefault() must make
     * dispatchEvent() return false. We capture the boolean result into a
     * global and stringify it so the completion is deterministic. */
    const char *src6 =
        "var r1 = true; var r2 = true;"
        "var p = document.getElementById('x');"
        "p.addEventListener('cancelme', function(e){ e.preventDefault(); });"
        "r1 = p.dispatchEvent('cancelme');"      /* should be false        */
        "r2 = p.dispatchEvent('nolisteners');"   /* no handler -> true     */
        "'' + r1 + '|' + r2;";
    js_node *p6 = js_parse_program(vm, src6, js_strlen(src6));
    if (!p6) { dom_document_free(doc); return -19; }
    {
        js_value c6;
        if (js_run_program(vm, p6, &c6) != 0 ||
            c6.type != JS_STRING || !c6.u.s ||
            !str_eq(c6.u.s->data, "false|true")) {
            dom_document_free(doc);
            return -20;
        }
    }

    /* stopPropagation(): build a parent>child chain, add a bubble listener
     * on each, and have the CHILD's handler call e.stopPropagation(). The
     * child handler must run but the parent's bubble handler must NOT, so
     * the recorded order is just "child". (A control event with no stop
     * yields "child,parent", proving the chain bubbles by default.) */
    const char *src7 =
        "var log = '';"
        "var par = document.createElement('section');"
        "par.id = 'par';"
        "var ch  = document.createElement('button');"
        "ch.id = 'ch';"
        "par.appendChild(ch);"
        "document.body.appendChild(par);"
        "par.addEventListener('tap', function(e){"
        "  log = log + 'parent,';"
        "});"
        "ch.addEventListener('tap', function(e){"
        "  log = log + 'child,'; e.stopPropagation();"
        "});"
        "ch.dispatchEvent('tap');"               /* stopped at child       */
        "var stopped = log;"
        /* Control: a second type with no stopPropagation bubbles fully.   */
        "log = '';"
        "par.addEventListener('tap2', function(e){ log = log + 'parent,'; });"
        "ch.addEventListener('tap2', function(e){ log = log + 'child,'; });"
        "ch.dispatchEvent('tap2');"
        "stopped + '|' + log;";
    js_node *p7 = js_parse_program(vm, src7, js_strlen(src7));
    if (!p7) { dom_document_free(doc); return -21; }
    {
        js_value c7;
        if (js_run_program(vm, p7, &c7) != 0 ||
            c7.type != JS_STRING || !c7.u.s ||
            !str_eq(c7.u.s->data, "child,|child,parent,")) {
            dom_document_free(doc);
            return -22;
        }
    }

    /* ------------------------------------------------------------------
     * New API tests
     * ------------------------------------------------------------------ */

    /* nextElementSibling / previousElementSibling / firstElementChild /
     * lastElementChild: build a <ul> with three <li> children, then
     * verify navigation.
     * Expected result: "LI|LI|LI|LI|0".
     *   fe = firstElementChild.tagName == "LI"
     *   le = lastElementChild.tagName  == "LI"
     *   nxt = fe.nextElementSibling.tagName == "LI"
     *   prv = nxt.previousElementSibling.tagName == "LI" (== fe)
     *   (prv === null ? 1 : 0) == 0  (prv is not null) */
    const char *src8 =
        "var ul = document.createElement('ul');"
        "var li1 = document.createElement('li');"
        "var li2 = document.createElement('li');"
        "var li3 = document.createElement('li');"
        "ul.appendChild(li1); ul.appendChild(li2); ul.appendChild(li3);"
        "document.body.appendChild(ul);"
        "var fe = ul.firstElementChild;"
        "var le = ul.lastElementChild;"
        "var nxt = fe.nextElementSibling;"
        "var prv = nxt.previousElementSibling;"
        "'' + fe.tagName + '|' + le.tagName + '|' + nxt.tagName + '|'"
        "+ prv.tagName + '|' + (prv === null ? 1 : 0);";
    js_node *p8 = js_parse_program(vm, src8, js_strlen(src8));
    if (!p8) { dom_document_free(doc); return -23; }
    {
        js_value c8;
        if (js_run_program(vm, p8, &c8) != 0 ||
            c8.type != JS_STRING || !c8.u.s ||
            !str_eq(c8.u.s->data, "LI|LI|LI|LI|0")) {
            dom_document_free(doc);
            return -24;
        }
    }

    /* classList.add / contains / remove / toggle.
     * Expected: "1|0|1|0|foo bar|foo" */
    const char *src9 =
        "var d = document.createElement('div');"
        "d.className = 'foo';"
        "d.classList.add('bar');"
        "var has1 = d.classList.contains('bar') ? 1 : 0;"  /* 1 */
        "d.classList.remove('bar');"
        "var has2 = d.classList.contains('bar') ? 1 : 0;"  /* 0 */
        "var state = d.classList.toggle('baz');"            /* true->1 */
        "var has3 = d.classList.contains('baz') ? 1 : 0;"  /* 1 */
        "d.classList.toggle('baz', false);"                 /* force off */
        "var has4 = d.classList.contains('baz') ? 1 : 0;"  /* 0 */
        /* reset to 'foo bar' */
        "d.classList.add('bar');"
        "var val = d.classList.value;"                      /* 'foo bar' */
        /* remove 'bar', verify value */
        "d.classList.remove('bar');"
        "var val2 = d.classList.value;"                     /* 'foo' */
        "'' + has1 + '|' + has2 + '|' + (state ? 1:0) + '|' + has4 + '|' + val + '|' + val2;";
    js_node *p9 = js_parse_program(vm, src9, js_strlen(src9));
    if (!p9) { dom_document_free(doc); return -25; }
    {
        js_value c9;
        if (js_run_program(vm, p9, &c9) != 0 ||
            c9.type != JS_STRING || !c9.u.s ||
            !str_eq(c9.u.s->data, "1|0|1|0|foo bar|foo")) {
            dom_document_free(doc);
            return -26;
        }
    }

    /* getElementsByTagName / getElementsByClassName.
     * wrap has 2 span + 1 p; s1 and p2 each have class "hi".
     * Expected: "2|2" (2 spans by tag, 2 elements by class) */
    const char *src10 =
        "var wrap = document.createElement('div');"
        "var s1 = document.createElement('span');"
        "s1.className = 'hi';"
        "var s2 = document.createElement('span');"
        "var p2 = document.createElement('p');"
        "p2.className = 'hi';"
        "wrap.appendChild(s1); wrap.appendChild(s2); wrap.appendChild(p2);"
        "document.body.appendChild(wrap);"
        "var byTag = wrap.getElementsByTagName('span').length;"  /* 2 */
        "var byCls = wrap.getElementsByClassName('hi').length;"  /* 2 */
        /* also test document-level getElementsByTagName */
        "var docByP = document.getElementsByTagName('p').length;" /* >= 1 */
        "'' + byTag + '|' + byCls;";
    js_node *p10 = js_parse_program(vm, src10, js_strlen(src10));
    if (!p10) { dom_document_free(doc); return -27; }
    {
        js_value c10;
        if (js_run_program(vm, p10, &c10) != 0 ||
            c10.type != JS_STRING || !c10.u.s ||
            !str_eq(c10.u.s->data, "2|2")) {
            dom_document_free(doc);
            return -28;
        }
    }

    /* matches() / closest().
     * Expected: "1|0|SECTION" */
    const char *src11 =
        "var sec = document.createElement('section');"
        "sec.id = 'sec1';"
        "var btn = document.createElement('button');"
        "btn.className = 'ok';"
        "sec.appendChild(btn);"
        "document.body.appendChild(sec);"
        "var m1 = btn.matches('button.ok') ? 1 : 0;"    /* 1 */
        "var m2 = btn.matches('div') ? 1 : 0;"          /* 0 */
        "var cl = btn.closest('section');"               /* sec element */
        "'' + m1 + '|' + m2 + '|' + (cl ? cl.tagName : 'null');";
    js_node *p11 = js_parse_program(vm, src11, js_strlen(src11));
    if (!p11) { dom_document_free(doc); return -29; }
    {
        js_value c11;
        if (js_run_program(vm, p11, &c11) != 0 ||
            c11.type != JS_STRING || !c11.u.s ||
            !str_eq(c11.u.s->data, "1|0|SECTION")) {
            dom_document_free(doc);
            return -30;
        }
    }

    /* remove() -- detach element from parent.
     * Expected: "0" (childElementCount of wrapper after remove) */
    const char *src12 =
        "var box = document.createElement('div');"
        "var kid = document.createElement('span');"
        "box.appendChild(kid);"
        "document.body.appendChild(box);"
        "kid.remove();"
        "'' + box.childElementCount;";
    js_node *p12 = js_parse_program(vm, src12, js_strlen(src12));
    if (!p12) { dom_document_free(doc); return -31; }
    {
        js_value c12;
        if (js_run_program(vm, p12, &c12) != 0 ||
            c12.type != JS_STRING || !c12.u.s ||
            !str_eq(c12.u.s->data, "0")) {
            dom_document_free(doc);
            return -32;
        }
    }

    /* cloneNode(false) -- shallow: clone has same tag/attrs but no children.
     * cloneNode(true)  -- deep: clone includes children.
     * Expected: "SPAN|0|SPAN|1" */
    const char *src13 =
        "var orig = document.createElement('span');"
        "orig.id = 'orig';"
        "var child = document.createElement('em');"
        "orig.appendChild(child);"
        "var shallow = orig.cloneNode(false);"
        "var deep    = orig.cloneNode(true);"
        "'' + shallow.tagName + '|' + shallow.childElementCount"
        " + '|' + deep.tagName + '|' + deep.childElementCount;";
    js_node *p13 = js_parse_program(vm, src13, js_strlen(src13));
    if (!p13) { dom_document_free(doc); return -33; }
    {
        js_value c13;
        if (js_run_program(vm, p13, &c13) != 0 ||
            c13.type != JS_STRING || !c13.u.s ||
            !str_eq(c13.u.s->data, "SPAN|0|SPAN|1")) {
            dom_document_free(doc);
            return -34;
        }
    }

    /* getAttributeNames() -- returns array of attribute name strings.
     * Expected: "2" (two attributes set) */
    const char *src14 =
        "var atn = document.createElement('a');"
        "atn.setAttribute('href', '#');"
        "atn.setAttribute('title', 'hi');"
        "'' + atn.getAttributeNames().length;";
    js_node *p14 = js_parse_program(vm, src14, js_strlen(src14));
    if (!p14) { dom_document_free(doc); return -35; }
    {
        js_value c14;
        if (js_run_program(vm, p14, &c14) != 0 ||
            c14.type != JS_STRING || !c14.u.s ||
            !str_eq(c14.u.s->data, "2")) {
            dom_document_free(doc);
            return -36;
        }
    }

    dom_document_free(doc);
    return 0;
}

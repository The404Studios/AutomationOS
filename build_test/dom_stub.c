/* Minimal dom.c implementation for self-tests, uses hosted libc. */
#include "dom.h"
#include <stdlib.h>
#include <string.h>

static char *xdup(const char *s)
{
    if (!s) return 0;
    unsigned long n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (!o) return 0;
    for (unsigned long i = 0; i <= n; i++) o[i] = s[i];
    return o;
}

struct dom_document *dom_document_new(void)
{
    struct dom_document *d = (struct dom_document *)calloc(1, sizeof(*d));
    if (!d) return 0;
    d->root = (struct dom_node *)calloc(1, sizeof(struct dom_node));
    if (!d->root) { free(d); return 0; }
    d->root->type = DOM_NODE_DOCUMENT;
    return d;
}

struct dom_node *dom_create_element(const char *tag)
{
    struct dom_node *n = (struct dom_node *)calloc(1, sizeof(*n));
    if (!n) return 0;
    n->type = DOM_NODE_ELEMENT;
    n->tag = xdup(tag ? tag : "");
    return n;
}

struct dom_node *dom_create_text(const char *text)
{
    struct dom_node *n = (struct dom_node *)calloc(1, sizeof(*n));
    if (!n) return 0;
    n->type = DOM_NODE_TEXT;
    n->text = xdup(text ? text : "");
    return n;
}

struct dom_node *dom_create_comment(const char *text)
{
    struct dom_node *n = (struct dom_node *)calloc(1, sizeof(*n));
    if (!n) return 0;
    n->type = DOM_NODE_COMMENT;
    n->text = xdup(text ? text : "");
    return n;
}

void dom_append_child(struct dom_node *parent, struct dom_node *child)
{
    if (!parent || !child) return;
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = 0;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else parent->first_child = child;
    parent->last_child = child;
}

void dom_set_attribute(struct dom_node *el, const char *name, const char *value)
{
    if (!el || !name) return;
    for (struct dom_attr *a = el->attrs; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0) {
            if (a->value) free(a->value);
            a->value = xdup(value ? value : "");
            return;
        }
    }
    struct dom_attr *a = (struct dom_attr *)calloc(1, sizeof(*a));
    if (!a) return;
    a->name = xdup(name);
    a->value = xdup(value ? value : "");
    a->next = el->attrs;
    el->attrs = a;
}

void dom_node_free(struct dom_node *n)
{
    if (!n) return;
    struct dom_node *c = n->first_child;
    while (c) {
        struct dom_node *nx = c->next_sibling;
        dom_node_free(c);
        c = nx;
    }
    struct dom_attr *a = n->attrs;
    while (a) {
        struct dom_attr *na = a->next;
        if (a->name) free(a->name);
        if (a->value) free(a->value);
        free(a);
        a = na;
    }
    if (n->tag) free(n->tag);
    if (n->text) free(n->text);
    free(n);
}

void dom_document_free(struct dom_document *d)
{
    if (!d) return;
    if (d->root) dom_node_free(d->root);
    if (d->url) free(d->url);
    free(d);
}

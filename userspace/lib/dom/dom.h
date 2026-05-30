/*
 * dom.h -- AutomationOS browser DOM data model (public API).
 * ===========================================================
 *
 * Freestanding ring-3 userspace. NO libc/stdio. Uses ONLY:
 *
 *   - userspace/libc/malloc.h  (malloc/free/calloc/realloc)
 *   - userspace/libc/string.h  (strlen/strcmp/memcpy/memset/strchr/...)
 *
 * Lifetimes: DOM nodes outlive any single js_eval(), so node memory comes
 * from malloc() (NOT the JS engine's per-eval arena). Strings (tag names,
 * attribute names/values, text content) are also malloc'd and owned by
 * their dom_node. Freeing a node frees its strings and recursively frees
 * its children.
 *
 * Build (objdump must show NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2 -c dom.c -o dom.o
 *
 * Threading: single-threaded. No locks.
 */
#ifndef DOM_H
#define DOM_H

/* Bring in size_t / NULL via the userspace libc headers. */
#include "../../libc/malloc.h"
#include "../../libc/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Node kinds                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    DOM_NODE_DOCUMENT = 0,
    DOM_NODE_ELEMENT,
    DOM_NODE_TEXT,
    DOM_NODE_COMMENT
} dom_node_type;

/* ------------------------------------------------------------------ */
/*  Attributes (singly-linked, insertion order preserved)             */
/* ------------------------------------------------------------------ */
typedef struct dom_attr {
    char            *name;   /* malloc'd, lowercased ASCII             */
    char            *value;  /* malloc'd, raw (already entity-decoded) */
    struct dom_attr *next;
} dom_attr;

/* ------------------------------------------------------------------ */
/*  Node                                                               */
/* ------------------------------------------------------------------ */
typedef struct dom_node {
    dom_node_type    type;

    /* Tree links */
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *last_child;
    struct dom_node *prev_sibling;
    struct dom_node *next_sibling;

    /* Element-only */
    char            *tag;    /* lowercased, malloc'd, NUL-terminated   */
    dom_attr        *attrs;

    /* Text/Comment-only */
    char            *text;   /* malloc'd, NUL-terminated               */

    /* Scratch for layout / render: layout box, computed style, etc.   */
    void            *user;
} dom_node;

/* ------------------------------------------------------------------ */
/*  Document                                                           */
/* ------------------------------------------------------------------ */
typedef struct dom_document {
    dom_node *root;          /* DOM_NODE_DOCUMENT, conventionally holds
                                <html> as its first child              */
    char     *url;           /* current page URL, malloc'd (may be NULL)*/
} dom_document;

/* ------------------------------------------------------------------ */
/*  Document lifecycle                                                 */
/* ------------------------------------------------------------------ */
dom_document *dom_document_new(void);
void          dom_document_free(dom_document *doc);

/* ------------------------------------------------------------------ */
/*  Node creation / destruction                                        */
/* ------------------------------------------------------------------ */
dom_node *dom_create_element(const char *tag);
dom_node *dom_create_text(const char *text);
dom_node *dom_create_comment(const char *text);
void      dom_node_free(dom_node *node);    /* recursive */

/* ------------------------------------------------------------------ */
/*  Tree mutation                                                      */
/* ------------------------------------------------------------------ */
void      dom_append_child(dom_node *parent, dom_node *child);
void      dom_remove_child(dom_node *parent, dom_node *child);
void      dom_insert_before(dom_node *parent, dom_node *child, dom_node *ref);

/* ------------------------------------------------------------------ */
/*  Attribute manipulation                                             */
/* ------------------------------------------------------------------ */
void        dom_set_attribute(dom_node *el, const char *name, const char *value);
const char *dom_get_attribute(const dom_node *el, const char *name);
int         dom_has_attribute(const dom_node *el, const char *name);
void        dom_remove_attribute(dom_node *el, const char *name);

/* ------------------------------------------------------------------ */
/*  Text content                                                       */
/* ------------------------------------------------------------------ */
/* On TEXT/COMMENT: replaces .text. On ELEMENT: drops all children and
 * attaches a single TEXT child with the given content. */
void        dom_set_text(dom_node *node, const char *text);
/* On TEXT/COMMENT: returns .text. On ELEMENT: returns a malloc'd
 * concatenation of all descendant TEXT nodes (caller must free).
 * For all other cases returns NULL. NULL is also returned on OOM.    */
const char *dom_get_text(const dom_node *node);

/* ------------------------------------------------------------------ */
/*  Queries                                                            */
/* ------------------------------------------------------------------ */
dom_node *dom_get_element_by_id(dom_document *doc, const char *id);

/* Walk (pre-order). Visitor may NOT mutate the tree during the walk. */
void      dom_walk(dom_node *root,
                   void (*visit)(dom_node *, void *), void *ctx);

/* ------------------------------------------------------------------ */
/*  Selftest                                                           */
/* ------------------------------------------------------------------ */
/* Builds a small sample tree, exercises queries + mutation + text. */
int       dom_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_H */

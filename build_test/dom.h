/* TEST-ONLY stub dom.h matching the contract documented in
 * userspace/lib/html/html_parse.c (see the comment at the top there).
 * This file is NOT part of the shipping codebase -- the real userspace/lib/dom/dom.h
 * is owned by a different agent.
 */
#ifndef DOM_H_TEST_STUB
#define DOM_H_TEST_STUB

enum {
    DOM_NODE_DOCUMENT = 0,
    DOM_NODE_ELEMENT  = 1,
    DOM_NODE_TEXT     = 2,
    DOM_NODE_COMMENT  = 3
};

struct dom_attr {
    char *name;
    char *value;
    struct dom_attr *next;
};

struct dom_node {
    int type;
    struct dom_node *parent, *first_child, *last_child, *prev_sibling, *next_sibling;
    char *tag;
    struct dom_attr *attrs;
    char *text;
    void *user;
};

struct dom_document {
    struct dom_node *root;
    char *url;
};

struct dom_document *dom_document_new(void);
struct dom_node *dom_create_element(const char *tag);
struct dom_node *dom_create_text(const char *text);
struct dom_node *dom_create_comment(const char *text);
void dom_append_child(struct dom_node *parent, struct dom_node *child);
void dom_set_attribute(struct dom_node *el, const char *name, const char *value);

/* For tests: a recursive free. */
void dom_node_free(struct dom_node *n);
void dom_document_free(struct dom_document *d);

#endif

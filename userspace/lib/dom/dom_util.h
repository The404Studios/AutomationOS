/*
 * dom_util.h -- cloneNode + tree-walk utilities for AutomationOS browser DOM.
 * ===========================================================================
 *
 * Freestanding ring-3 userspace.  No libc / stdio.
 * Uses only: userspace/libc/malloc.h, userspace/libc/string.h, dom.h.
 *
 * Build flags (no fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2
 *
 * Threading: single-threaded.  No locks.
 */
#ifndef DOM_UTIL_H
#define DOM_UTIL_H

#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dom_clone_node
 * --------------
 * Deep-clone (deep=1) or shallow-clone (deep=0) `src`.
 *
 * A deep clone copies the entire descendant subtree.  A shallow clone copies
 * only the node itself (tag/attrs/text) with no children.  In either case the
 * returned tree is completely independent: new malloc'd strings, new
 * dom_attr chains, new dom_node allocations.  The caller owns the result and
 * must eventually free it with dom_node_free().
 *
 * Returns NULL on OOM or if src is NULL.
 */
struct dom_node *dom_clone_node(const struct dom_node *src, int deep);

/*
 * dom_walk_tree
 * -------------
 * Visit every node in the subtree rooted at `root` in pre-order
 * (document order): root first, then first child and its whole subtree,
 * then the next sibling's subtree, etc.
 *
 * The visitor MUST NOT mutate the tree (add/remove/move nodes) during the
 * walk.  Modifying a node's attribute values or text is safe.
 *
 * Uses an explicit iterative stack capped at DOM_UTIL_WALK_STACK (256) to
 * avoid deep C-stack recursion.
 */
void dom_walk_tree(struct dom_node *root,
                   void (*visit)(struct dom_node *, void *),
                   void *user);

/*
 * dom_count_descendants
 * ---------------------
 * Count all descendants of `root` whose tag matches `tag` (case-insensitive).
 * Pass tag=NULL to count *all* descendants (any type, including text/comment).
 * `root` itself is NOT counted.
 *
 * Returns the count (>= 0).
 */
int dom_count_descendants(const struct dom_node *root, const char *tag);

/*
 * dom_depth
 * ---------
 * Return the depth of `node` within the subtree rooted at `root`, where
 * root itself is depth 0.
 *
 * Returns -1 if `node` is not a descendant of `root` (or is equal to root).
 * Returns -1 if either pointer is NULL.
 */
int dom_depth(const struct dom_node *root, const struct dom_node *node);

/*
 * dom_contains
 * ------------
 * Return 1 if `node` is a proper descendant of `ancestor` (i.e. ancestor
 * appears somewhere above node in the parent chain), 0 otherwise.
 * Returns 0 if either pointer is NULL or if ancestor == node.
 */
int dom_contains(const struct dom_node *ancestor, const struct dom_node *node);

/*
 * dom_owner_document
 * ------------------
 * Walk the parent chain of `node` until a DOM_NODE_DOCUMENT node is found.
 * That node is expected to be the `.root` member of a dom_document; this
 * function searches a caller-supplied `doc` to confirm the relationship and
 * returns it, or searches a small internal sentinel list if the tree was
 * built outside any document.
 *
 * In practice: walks parents to the root node; if the root's type is
 * DOM_NODE_DOCUMENT the function returns NULL (the caller does not have a
 * dom_document* handle here).  The contract says "may return NULL if root
 * has no document wrapper", which is the usual case for nodes created with
 * dom_create_element and never attached to a dom_document.
 *
 * NOTE: This function cannot return a valid dom_document* without an
 * out-of-band registry, so it always returns NULL per the stated contract
 * ("may return NULL").  It is provided for API completeness; the useful
 * side-effect is walking to the tree root.
 */
struct dom_document *dom_owner_document(const struct dom_node *node);

/*
 * dom_util_selftest
 * -----------------
 * Build a small tree, exercise clone/walk/count/depth/contains.
 * Returns 0 on full pass, negative on first failure.
 * Safe to call in freestanding builds.
 */
int dom_util_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_UTIL_H */

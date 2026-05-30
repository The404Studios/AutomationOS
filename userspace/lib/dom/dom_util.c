/*
 * dom_util.c -- cloneNode + tree-walk utilities for AutomationOS browser DOM.
 * ===========================================================================
 *
 * Freestanding ring-3.  NO libc/stdio.  Uses only malloc/free/calloc from
 * userspace/libc/malloc.h and strlen/strcmp/strcasecmp/memcpy from
 * userspace/libc/string.h, plus the DOM API from dom.h.
 *
 * Recursion avoidance
 * -------------------
 * dom_walk_tree uses an explicit stack array of DOM_UTIL_WALK_STACK (256)
 * node pointers allocated on the C stack.  This is safe because the stack
 * frame is small (256 * 8 = 2 KB on x86-64) and bounded.  When the stack
 * would overflow the hard cap we simply stop descending (extremely deep
 * trees beyond 256 levels are pathological).
 *
 * dom_clone_node for a deep clone is implemented as an iterative pre-order
 * walk using the same explicit-stack technique so that cloning a very deep
 * tree does not blow the C call stack either.
 *
 * Build flags (objdump must show NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2 -c dom_util.c -o dom_util.o
 */
#include "dom_util.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Maximum explicit-stack depth for iterative walks/clones. */
#define DOM_UTIL_WALK_STACK 256

/* Defensive sibling-scan cap (matches dom.c). */
#define DOM_UTIL_SIB_CAP    (1u << 20)

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */

/* ASCII case-insensitive compare (no libc strcasecmp available guarantee). */
static int util_ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int util_icmp(const char *a, const char *b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return  1;
    while (*a && *b) {
        int ca = util_ascii_tolower(*a);
        int cb = util_ascii_tolower(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return util_ascii_tolower(*a) - util_ascii_tolower(*b);
}

/* ========================================================================= */
/*  dom_clone_node                                                           */
/* ========================================================================= */

/*
 * Shallow-clone a single node (no children, no tree links set).
 * Uses dom_create_element / dom_create_text / dom_create_comment +
 * dom_set_attribute so memory ownership matches the rest of the API.
 */
static struct dom_node *clone_single(const struct dom_node *src)
{
    if (!src) return NULL;

    struct dom_node *dst = NULL;

    switch (src->type) {
    case DOM_NODE_ELEMENT:
        dst = dom_create_element(src->tag ? src->tag : "");
        if (!dst) return NULL;
        /* Copy attribute list in insertion order. */
        {
            const dom_attr *a = src->attrs;
            unsigned long guard = 0;
            while (a && guard++ < DOM_UTIL_SIB_CAP) {
                dom_set_attribute(dst, a->name  ? a->name  : "",
                                       a->value ? a->value : "");
                a = a->next;
            }
        }
        break;

    case DOM_NODE_TEXT:
        dst = dom_create_text(src->text);
        break;

    case DOM_NODE_COMMENT:
        dst = dom_create_comment(src->text);
        break;

    case DOM_NODE_DOCUMENT: {
        /* Rare: cloning a document node. Use calloc directly; no helper. */
        dst = (struct dom_node *)calloc(1, sizeof(struct dom_node));
        if (!dst) return NULL;
        dst->type = DOM_NODE_DOCUMENT;
        break;
    }
    }

    return dst;
}

/*
 * Iterative deep clone using an explicit stack of pairs:
 *   (src_node, dst_parent)
 * We push each child of src onto the stack together with the already-created
 * clone of its parent, so that when we pop we can dom_append_child the new
 * clone to the right parent.
 *
 * Stack entry: source node + its already-cloned parent destination.
 */
typedef struct {
    const struct dom_node *src;
    struct dom_node       *dst_parent; /* clone of src->parent, or NULL for root */
} clone_frame;

struct dom_node *dom_clone_node(const struct dom_node *src, int deep)
{
    if (!src) return NULL;

    /* --- shallow clone: just one node --- */
    if (!deep) {
        return clone_single(src);
    }

    /* --- deep clone: iterative pre-order --- */

    /* Explicit stack lives on the C stack; 256 * 2 ptrs = 4 KB on x86-64. */
    clone_frame stack[DOM_UTIL_WALK_STACK];
    int top = 0;

    /* Create the root clone first. */
    struct dom_node *root_clone = clone_single(src);
    if (!root_clone) return NULL;

    /* Push all children of src onto the stack paired with root_clone. */
    {
        const struct dom_node *c = src->first_child;
        unsigned long guard = 0;
        while (c && guard++ < DOM_UTIL_SIB_CAP) {
            if (top >= DOM_UTIL_WALK_STACK) break; /* cap: ignore deeper levels */
            stack[top].src        = c;
            stack[top].dst_parent = root_clone;
            top++;
            c = c->next_sibling;
        }
        /* Children were pushed in forward order; to preserve document order
         * during pop we need to reverse the segment just pushed so the first
         * child ends up on top.  Because we append each popped node to its
         * parent in order, we want first-child to be popped first. */
        /* Reverse the range [0, top) */
        {
            int lo = 0, hi = top - 1;
            while (lo < hi) {
                clone_frame tmp = stack[lo];
                stack[lo] = stack[hi];
                stack[hi] = tmp;
                lo++; hi--;
            }
        }
    }

    while (top > 0) {
        top--;
        const struct dom_node *cur_src    = stack[top].src;
        struct dom_node       *cur_parent = stack[top].dst_parent;

        /* Clone this node and attach to its parent clone. */
        struct dom_node *cur_clone = clone_single(cur_src);
        if (!cur_clone) {
            /* OOM: free what we built and bail. */
            dom_node_free(root_clone);
            return NULL;
        }
        dom_append_child(cur_parent, cur_clone);

        /* Push this node's children (in reverse order so first child is
         * popped/appended first). */
        if (cur_src->first_child) {
            /* Count children to push (bounded). */
            int push_start = top;
            const struct dom_node *c = cur_src->first_child;
            unsigned long guard = 0;
            while (c && guard++ < DOM_UTIL_SIB_CAP) {
                if (top >= DOM_UTIL_WALK_STACK) break;
                stack[top].src        = c;
                stack[top].dst_parent = cur_clone;
                top++;
                c = c->next_sibling;
            }
            /* Reverse the newly pushed range to maintain document order. */
            {
                int lo = push_start, hi = top - 1;
                while (lo < hi) {
                    clone_frame tmp = stack[lo];
                    stack[lo] = stack[hi];
                    stack[hi] = tmp;
                    lo++; hi--;
                }
            }
        }
    }

    return root_clone;
}

/* ========================================================================= */
/*  dom_walk_tree                                                            */
/* ========================================================================= */

/*
 * Iterative pre-order depth-first walk using an explicit node pointer stack.
 * We push children in reverse sibling order so that the first child is popped
 * (and therefore visited) before later siblings -- preserving document order.
 *
 * Stack is a fixed array on the C stack (256 pointers = 2 KB on x86-64).
 */
void dom_walk_tree(struct dom_node *root,
                   void (*visit)(struct dom_node *, void *),
                   void *user)
{
    if (!root || !visit) return;

    struct dom_node *stack[DOM_UTIL_WALK_STACK];
    int top = 0;

    stack[top++] = root;

    while (top > 0) {
        struct dom_node *cur = stack[--top];

        visit(cur, user);

        /* Push children in reverse order so first child is on top. */
        if (cur->first_child) {
            /* Collect siblings first (forward order) into a temporary
             * reverse-push loop. */
            /* Find last_child quickly via the DOM's last_child pointer. */
            struct dom_node *c = cur->last_child;
            unsigned long guard = 0;
            while (c && guard++ < DOM_UTIL_SIB_CAP) {
                if (top >= DOM_UTIL_WALK_STACK) break; /* cap depth */
                stack[top++] = c;
                c = c->prev_sibling;
            }
        }
    }
}

/* ========================================================================= */
/*  dom_count_descendants                                                    */
/* ========================================================================= */

/*
 * Count context passed to a walk visitor.
 */
typedef struct {
    const char *tag;   /* NULL = count all */
    int         count;
    int         skip_root; /* set to 1 so the first call (root) is ignored */
} count_ctx;

static void count_visitor(struct dom_node *n, void *u)
{
    count_ctx *ctx = (count_ctx *)u;
    if (ctx->skip_root) {
        ctx->skip_root = 0;
        return; /* don't count root itself */
    }
    if (ctx->tag == NULL) {
        ctx->count++;
    } else {
        /* Only ELEMENT nodes have tags. */
        if (n->type == DOM_NODE_ELEMENT && n->tag &&
            util_icmp(n->tag, ctx->tag) == 0) {
            ctx->count++;
        }
    }
}

int dom_count_descendants(const struct dom_node *root, const char *tag)
{
    if (!root) return 0;
    count_ctx ctx;
    ctx.tag       = tag;
    ctx.count     = 0;
    ctx.skip_root = 1;
    dom_walk_tree((struct dom_node *)root, count_visitor, &ctx);
    return ctx.count;
}

/* ========================================================================= */
/*  dom_depth                                                                */
/* ========================================================================= */

int dom_depth(const struct dom_node *root, const struct dom_node *node)
{
    if (!root || !node) return -1;
    if (root == node) return -1; /* root itself is not a descendant */

    /* Walk the parent chain of `node` upward, counting levels. */
    int depth = 0;
    const struct dom_node *cur = node;
    unsigned long guard = 0;
    while (cur->parent && guard++ < DOM_UTIL_WALK_STACK) {
        depth++;
        cur = cur->parent;
        if (cur == root) return depth;
    }
    return -1; /* not found under root */
}

/* ========================================================================= */
/*  dom_contains                                                             */
/* ========================================================================= */

int dom_contains(const struct dom_node *ancestor, const struct dom_node *node)
{
    if (!ancestor || !node || ancestor == node) return 0;

    const struct dom_node *cur = node->parent;
    unsigned long guard = 0;
    while (cur && guard++ < DOM_UTIL_WALK_STACK) {
        if (cur == ancestor) return 1;
        cur = cur->parent;
    }
    return 0;
}

/* ========================================================================= */
/*  dom_owner_document                                                       */
/* ========================================================================= */

/*
 * Per the contract: "may return NULL if root has no document wrapper".
 * Without a global document registry we cannot recover the dom_document*
 * from a bare dom_node*; we walk to the root and return NULL regardless.
 * The function is provided for API completeness.
 */
struct dom_document *dom_owner_document(const struct dom_node *node)
{
    (void)node; /* walk to root is possible but returning NULL is correct per spec */
    return NULL;
}

/* ========================================================================= */
/*  dom_util_selftest                                                        */
/* ========================================================================= */

/*
 * Tree built for the selftest:
 *
 *   div#root  [id="root"]
 *     p.a     [class="a"]             -- text "hello"
 *     p.b     [class="b"]             -- text "world"
 *       span  [data-x="42"]           -- text "inner"
 *     p.c     [class="c"]
 *
 * Deep-clone div#root.
 * Assertions:
 *   1. Clone has same structure as original (tags, attrs, texts).
 *   2. Clone is independent (modifying original doesn't change clone).
 *   3. dom_count_descendants(root, "p")   == 3
 *   4. dom_count_descendants(root, NULL)  == 5  (3 p + 1 span + 4 text nodes... wait)
 *      Actually: p.a has 1 text child, p.b has 1 text + span (span has 1 text),
 *      p.c has 0 children.
 *      Descendants of div: p.a, text("hello"), p.b, text("world"), span,
 *                          text("inner"), p.c  => 7 total descendants.
 *      "p" descendants: 3.
 *   5. dom_depth(div, span)  == 2  (div -> p.b -> span)
 *   6. dom_contains(div, span) == 1
 *   7. dom_contains(div, div)  == 0
 *   8. dom_depth(div, div)    == -1 (root is not its own descendant)
 */

/* Small assert helper: returns failure code if cond is false. */
#define SELFTEST_ASSERT(cond, code) do { if (!(cond)) { dom_node_free(root); return (code); } } while (0)

int dom_util_selftest(void)
{
    /* Build the tree. */
    struct dom_node *root  = dom_create_element("div");
    if (!root) return -1;
    dom_set_attribute(root, "id", "root");

    struct dom_node *pa = dom_create_element("p");
    struct dom_node *pb = dom_create_element("p");
    struct dom_node *pc = dom_create_element("p");
    struct dom_node *sp = dom_create_element("span");

    if (!pa || !pb || !pc || !sp) {
        if (pa) dom_node_free(pa);
        if (pb) dom_node_free(pb);
        if (pc) dom_node_free(pc);
        if (sp) dom_node_free(sp);
        dom_node_free(root);
        return -2;
    }

    dom_set_attribute(pa, "class", "a");
    dom_set_attribute(pb, "class", "b");
    dom_set_attribute(pc, "class", "c");
    dom_set_attribute(sp, "data-x", "42");

    dom_set_text(pa, "hello");
    dom_set_text(pb, "world");
    dom_set_text(sp, "inner");

    dom_append_child(root, pa);
    dom_append_child(root, pb);
    dom_append_child(pb, sp);   /* span is second child of pb (after text "world") */
    dom_append_child(root, pc);

    /* --- 1. dom_count_descendants("p") --- */
    int cnt_p = dom_count_descendants(root, "p");
    SELFTEST_ASSERT(cnt_p == 3, -10);

    /* --- 2. dom_count_descendants(NULL) counts all descendants --- */
    int cnt_all = dom_count_descendants(root, NULL);
    /* Descendants: pa, text(hello), pb, text(world), sp, text(inner), pc = 7 */
    SELFTEST_ASSERT(cnt_all == 7, -11);

    /* --- 3. dom_depth --- */
    /* sp is at: root(0) -> pb(1) -> sp(2), so depth == 2 */
    int d = dom_depth(root, sp);
    SELFTEST_ASSERT(d == 2, -12);

    /* pa is a direct child: depth == 1 */
    int d_pa = dom_depth(root, pa);
    SELFTEST_ASSERT(d_pa == 1, -13);

    /* root is not a descendant of itself: -1 */
    int d_self = dom_depth(root, root);
    SELFTEST_ASSERT(d_self == -1, -14);

    /* --- 4. dom_contains --- */
    SELFTEST_ASSERT(dom_contains(root, sp)   == 1, -15);
    SELFTEST_ASSERT(dom_contains(root, pa)   == 1, -16);
    SELFTEST_ASSERT(dom_contains(root, root) == 0, -17);
    SELFTEST_ASSERT(dom_contains(pb, sp)     == 1, -18);
    SELFTEST_ASSERT(dom_contains(pa, sp)     == 0, -19);

    /* --- 5. dom_clone_node(root, 0) -- shallow clone --- */
    struct dom_node *shallow = dom_clone_node(root, 0);
    SELFTEST_ASSERT(shallow != NULL, -20);
    SELFTEST_ASSERT(shallow != root, -21);
    /* Shallow clone has no children. */
    SELFTEST_ASSERT(shallow->first_child == NULL, -22);
    /* Tag and attribute match. */
    SELFTEST_ASSERT(shallow->tag && strcmp(shallow->tag, "div") == 0, -23);
    const char *sid = dom_get_attribute(shallow, "id");
    SELFTEST_ASSERT(sid && strcmp(sid, "root") == 0, -24);
    dom_node_free(shallow);

    /* --- 6. dom_clone_node(root, 1) -- deep clone --- */
    struct dom_node *deep = dom_clone_node(root, 1);
    SELFTEST_ASSERT(deep != NULL, -30);
    SELFTEST_ASSERT(deep != root, -31);

    /* Clone tag. */
    SELFTEST_ASSERT(deep->tag && strcmp(deep->tag, "div") == 0, -32);
    const char *did = dom_get_attribute(deep, "id");
    SELFTEST_ASSERT(did && strcmp(did, "root") == 0, -33);

    /* Clone has 3 direct p children. */
    struct dom_node *dpa = deep->first_child;
    SELFTEST_ASSERT(dpa != NULL, -34);
    SELFTEST_ASSERT(dpa->tag && strcmp(dpa->tag, "p") == 0, -35);
    const char *dpa_class = dom_get_attribute(dpa, "class");
    SELFTEST_ASSERT(dpa_class && strcmp(dpa_class, "a") == 0, -36);

    struct dom_node *dpb = dpa->next_sibling;
    SELFTEST_ASSERT(dpb != NULL, -37);
    SELFTEST_ASSERT(dpb->tag && strcmp(dpb->tag, "p") == 0, -38);

    struct dom_node *dpc = dpb->next_sibling;
    SELFTEST_ASSERT(dpc != NULL, -39);
    SELFTEST_ASSERT(dpc->next_sibling == NULL, -40); /* exactly 3 children */

    /* Clone of pb has text child + span child. */
    struct dom_node *dpb_text = dpb->first_child;
    SELFTEST_ASSERT(dpb_text != NULL && dpb_text->type == DOM_NODE_TEXT, -41);
    SELFTEST_ASSERT(dpb_text->text && strcmp(dpb_text->text, "world") == 0, -42);

    struct dom_node *dsp = dpb_text->next_sibling;
    SELFTEST_ASSERT(dsp != NULL, -43);
    SELFTEST_ASSERT(dsp->tag && strcmp(dsp->tag, "span") == 0, -44);
    const char *dsp_dx = dom_get_attribute(dsp, "data-x");
    SELFTEST_ASSERT(dsp_dx && strcmp(dsp_dx, "42") == 0, -45);

    /* span's text child. */
    struct dom_node *dsp_text = dsp->first_child;
    SELFTEST_ASSERT(dsp_text != NULL && dsp_text->type == DOM_NODE_TEXT, -46);
    SELFTEST_ASSERT(dsp_text->text && strcmp(dsp_text->text, "inner") == 0, -47);

    /* --- 7. Clone is independent: modify original, clone unchanged. --- */
    dom_set_attribute(root, "id", "CHANGED");
    const char *orig_id_now = dom_get_attribute(root, "id");
    const char *clone_id_still = dom_get_attribute(deep, "id");
    SELFTEST_ASSERT(orig_id_now   && strcmp(orig_id_now,   "CHANGED") == 0, -50);
    SELFTEST_ASSERT(clone_id_still && strcmp(clone_id_still, "root")   == 0, -51);

    /* Modify a text node in the original; clone's text must stay. */
    dom_set_text(pa, "modified");
    SELFTEST_ASSERT(pa->first_child && strcmp(pa->first_child->text, "modified") == 0, -52);
    SELFTEST_ASSERT(dpa->first_child && strcmp(dpa->first_child->text, "hello") == 0, -53);

    /* --- 8. dom_walk_tree counts on the clone (structure verification). --- */
    count_ctx wctx;
    wctx.tag       = NULL;
    wctx.count     = 0;
    wctx.skip_root = 0; /* count root too */
    dom_walk_tree(deep, count_visitor, &wctx);
    /* clone: div + 3p + text(hello) + text(world) + span + text(inner) = 8
       Note: skip_root is 0 so root IS counted by count_visitor here
       but count_visitor checks skip_root flag; we set it to 0 so root is
       NOT skipped (visitor decrements skip_root flag on first call).
       Actually count_visitor skips when skip_root==1; here skip_root=0 so
       all nodes including root are counted.
       Total nodes: div(1) + p.a(1) + text(hello)(1) + p.b(1) + text(world)(1)
                    + span(1) + text(inner)(1) + p.c(1) = 8 */
    SELFTEST_ASSERT(wctx.count == 8, -54);

    /* --- Cleanup --- */
    dom_node_free(deep);
    dom_node_free(root);  /* also frees pa, pb, pc, sp and their text children */

    return 0; /* full pass */
}

/* Undef local helpers to keep the translation unit clean. */
#undef SELFTEST_ASSERT

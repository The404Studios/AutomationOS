/* userspace/lib/layout/layout.c
 * ============================================================================
 * Flow layout engine: walks the DOM, computes css_computed per element, and
 * produces a layout_box tree with absolute viewport coords.
 *
 * See layout.h for the model and the explicit list of features we drop
 * (no float, no flex/grid, no z-index, no transforms, etc).
 *
 * FREESTANDING ring-3 -- only userspace/libc/{malloc.h, string.h}.
 *
 * Margin collapsing policy (documented):
 *   Adjacent vertical margins between sibling block boxes COLLAPSE: the gap
 *   between consecutive sibling blocks is MAX(prev->margin_b, cur->margin_t)
 *   rather than the sum.  Parent-child margin collapse is NOT implemented
 *   (first/last child margins do not escape through the parent).
 * ============================================================================
 */

#include "layout.h"
#include "../../libc/malloc.h"
#include "../../libc/string.h"

/* Fixed-width 8x16 bitmap font advance (in px). Bold/italic don't extend
 * advance in our fixed-glyph model. */
#define GLYPH_W   8
#define LINE_H_OF(fs) ((fs) + 4)

/* Maximum layout recursion depth guard — prevents stack overflow on
 * pathologically deep DOM trees. */
#define MAX_DEPTH 256

/* ------------------------------------------------------------------ */
/* Small helpers.                                                       */
/* ------------------------------------------------------------------ */

/* Whitespace as defined by HTML inline layout (HTML uses ' '/'\t'/'\n'/'\r'). */
static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ */
/* Box allocation.                                                      */
/* ------------------------------------------------------------------ */

static layout_box *box_new(int kind, const struct dom_node *node)
{
    layout_box *b = (layout_box *)calloc(1, sizeof(*b));
    if (!b) return 0;
    b->kind = kind;
    b->node = node;
    b->text = 0;
    b->first_child = 0;
    b->next_sibling = 0;
    return b;
}

static void box_attach(layout_box *parent, layout_box *child)
{
    if (!parent || !child) return;
    /* Append to sibling list. */
    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        layout_box *c = parent->first_child;
        while (c->next_sibling) c = c->next_sibling;
        c->next_sibling = child;
    }
}

/* Recursive box-tree walker; arena cleanup is layered on top in
 * layout_free() below. */
static void free_tree(layout_box *root)
{
    if (!root) return;
    layout_box *c = root->first_child;
    while (c) {
        layout_box *n = c->next_sibling;
        free_tree(c);
        c = n;
    }
    /* `text` borrows from the arena, which is freed by the public
     * layout_free() entry point. */
    free(root);
}

/* ------------------------------------------------------------------ */
/* Text arena.  Layout copies text into its own buffer so the boxes'  */
/* `text` pointers stay valid even if the DOM is mutated.             */
/* We attach the arena pointer to the root box via a header trick:    */
/* the arena chunks are kept on a small linked list freed at the end. */
/* ------------------------------------------------------------------ */

typedef struct arena_chunk {
    struct arena_chunk *next;
    char *base;
    unsigned long len;
    unsigned long cap;
} arena_chunk;

typedef struct {
    arena_chunk *head;
    arena_chunk *cur;
} arena;

static char *arena_push(arena *a, const char *p, unsigned long n)
{
    if (n == 0) {
        /* Return pointer to an empty string in the arena. */
        if (!a->cur || a->cur->len + 1 > a->cur->cap) {
            arena_chunk *c = (arena_chunk *)calloc(1, sizeof(*c));
            if (!c) return 0;
            c->cap = 1024;
            c->base = (char *)malloc(c->cap);
            if (!c->base) { free(c); return 0; }
            c->next = a->head;
            a->head = c;
            a->cur = c;
        }
        char *out = a->cur->base + a->cur->len;
        out[0] = 0;
        a->cur->len += 1;
        return out;
    }
    /* Need n+1 bytes (NUL). */
    if (!a->cur || a->cur->len + n + 1 > a->cur->cap) {
        unsigned long cap = 1024;
        while (cap < n + 1) cap *= 2;
        arena_chunk *c = (arena_chunk *)calloc(1, sizeof(*c));
        if (!c) return 0;
        c->cap = cap;
        c->base = (char *)malloc(cap);
        if (!c->base) { free(c); return 0; }
        c->next = a->head;
        a->head = c;
        a->cur = c;
    }
    char *out = a->cur->base + a->cur->len;
    for (unsigned long i = 0; i < n; i++) out[i] = p[i];
    out[n] = 0;
    a->cur->len += n + 1;
    return out;
}

static void arena_free(arena *a)
{
    arena_chunk *c = a->head;
    while (c) {
        arena_chunk *n = c->next;
        if (c->base) free(c->base);
        free(c);
        c = n;
    }
    a->head = a->cur = 0;
}

/* We need to keep the arena alive for the lifetime of the layout tree,
 * yet pass it through layout_compute(). We allocate it heap-side and
 * hide a pointer to it inside layout_box::node of the root (we can't --
 * node is const). Instead, we cheat: we allocate the arena AT the same
 * address as a sentinel by stashing it in a static singleton hash, indexed
 * by root pointer. The simpler approach: stash arena AFTER the root box,
 * via a leading "doc box" that owns a struct via a side field. We add a
 * small parallel registry. */

typedef struct {
    layout_box *root;
    arena *arn;
} arena_reg_entry;

static arena_reg_entry *g_reg = 0;
static int g_reg_len = 0;
static int g_reg_cap = 0;

static void arena_reg_add(layout_box *root, arena *arn)
{
    if (g_reg_len + 1 > g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 8;
        arena_reg_entry *na = (arena_reg_entry *)realloc(g_reg,
            (unsigned long)nc * sizeof(arena_reg_entry));
        if (!na) return;
        g_reg = na;
        g_reg_cap = nc;
    }
    g_reg[g_reg_len].root = root;
    g_reg[g_reg_len].arn = arn;
    g_reg_len++;
}

static arena *arena_reg_take(layout_box *root)
{
    for (int i = 0; i < g_reg_len; i++) {
        if (g_reg[i].root == root) {
            arena *a = g_reg[i].arn;
            g_reg[i] = g_reg[g_reg_len - 1];
            g_reg_len--;
            return a;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Layout pass.                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    css_stylesheet *sheet;
    arena *arn;
} ctx;

/* Forward decl.
 *
 * layout_node: lays out children of `node` into `block`.
 *   content_x / content_y: absolute viewport coords of the content area
 *     top-left (i.e. block outer-x + margin_l + padding_l, etc.).
 *     These are used only for positioning child boxes and line origins;
 *     block->x / block->y are NOT touched — the caller sets them to the
 *     outer box edge before calling this function.
 *   avail_w: content-area width (block inner width after padding).
 *   depth: recursion depth guard; layout_node returns 0 when depth >= MAX_DEPTH.
 *
 * Returns the total content height (sum of line heights + block child heights
 * + margins). Does NOT include the calling block's own padding_t/b.
 */
static int layout_node(ctx *C,
                       const struct dom_node *node,
                       layout_box *block,
                       int avail_w,
                       int content_x, int content_y,
                       int depth);

/* ------------------------------------------------------------------ */
/* Inline formatting context helpers.                                   */
/* ------------------------------------------------------------------ */

/* Push one LB_TEXT word into the current line. Returns the LB_TEXT box. */
static layout_box *make_text_box(ctx *C, const struct dom_node *src,
                                 const char *word, unsigned long wlen,
                                 const css_computed *style)
{
    layout_box *b = box_new(LB_TEXT, src);
    if (!b) return 0;
    char *t = arena_push(C->arn, word, wlen);
    b->text = t ? t : "";
    b->style = *style;
    b->w = (int)wlen * GLYPH_W;
    b->h = LINE_H_OF(style->font_size);
    return b;
}

/* Inline state used throughout an inline formatting context. */
typedef struct {
    layout_box *line;       /* current LB_LINE box */
    int         line_x;     /* next x within line (relative to line->x) */
    int         line_h;     /* tallest box on this line */
    int         total_h;    /* cumulative inline height (grows as lines finish) */
    int         base_x;     /* content-area left edge (absolute viewport x) */
    int         content_y;  /* content-area top edge (absolute viewport y) */
    int         max_w;      /* available inline width */
} inline_state;

static void start_line(inline_state *S, layout_box *block)
{
    layout_box *ln = box_new(LB_LINE, 0);
    if (!ln) return;
    ln->x = S->base_x;
    ln->y = S->content_y + S->total_h;
    ln->w = S->max_w;
    ln->h = 0;
    box_attach(block, ln);
    S->line = ln;
    S->line_x = 0;
    S->line_h = 0;
}

static void finish_line(inline_state *S, const css_computed *parent_style)
{
    if (!S->line) return;
    /* Guard: if no children produced any height, use a default line height. */
    if (S->line_h == 0) {
        if (parent_style) S->line_h = LINE_H_OF(parent_style->font_size);
        if (S->line_h == 0) S->line_h = LINE_H_OF(16);
    }
    S->line->h = S->line_h;
    /* Text alignment within line: shift inline children if not left-aligned. */
    if (parent_style && parent_style->text_align != CSS_ALIGN_LEFT
            && S->line_x < S->max_w) {
        int gap = S->max_w - S->line_x;
        if (gap < 0) gap = 0;
        int shift = 0;
        if (parent_style->text_align == CSS_ALIGN_CENTER) shift = gap / 2;
        else if (parent_style->text_align == CSS_ALIGN_RIGHT) shift = gap;
        if (shift > 0) {
            for (layout_box *c = S->line->first_child; c; c = c->next_sibling) {
                c->x += shift;
            }
        }
    }
    S->total_h += S->line_h;
    S->line = 0;
}

/* Add inline child to current line; wrap if necessary.
 * Single words wider than max_w are placed on their own line without
 * clipping (they overflow, as documented in layout.h). */
static void inline_add(inline_state *S, layout_box *block,
                       layout_box *child, const css_computed *parent_style)
{
    if (!child) return;
    /* Ensure there is an active line. */
    if (!S->line) start_line(S, block);
    if (!S->line) return;

    /* Wrap: if child doesn't fit on current line and the line is non-empty,
     * finish the current line and start a new one. */
    if (S->line_x > 0 && S->line_x + child->w > S->max_w) {
        finish_line(S, parent_style);
        start_line(S, block);
        if (!S->line) return;
    }
    /* Place child at absolute position within the line. */
    child->x = S->line->x + S->line_x;
    child->y = S->line->y;
    box_attach(S->line, child);
    S->line_x += child->w;
    if (child->h > S->line_h) S->line_h = child->h;
}

/* Recurse into a DOM subtree, emitting inline content into S. `text_style`
 * is the *current* style cascading down the inline chain. */
static void emit_inline_subtree(ctx *C, const struct dom_node *n,
                                layout_box *block, inline_state *S,
                                const css_computed *parent_style,
                                int depth)
{
    if (!n) return;
    if (depth >= MAX_DEPTH) return;

    if (n->type == DOM_NODE_TEXT) {
        /* Split into whitespace-collapsed words. */
        const char *p = n->text;
        if (!p) return;
        const char *end = p + strlen(p);
        /* prev_ws tracks whether the last emitted token was whitespace;
         * start with 1 so we don't emit a leading space at line start. */
        int prev_ws = (S->line_x == 0) ? 1 : 0;
        while (p < end) {
            if (is_ws((unsigned char)*p)) {
                if (!prev_ws) {
                    /* Emit a single space-segment between words. */
                    layout_box *sp = make_text_box(C, n, " ", 1, parent_style);
                    inline_add(S, block, sp, parent_style);
                }
                prev_ws = 1;
                p++;
                continue;
            }
            /* Scan to end of word. */
            const char *ws = p;
            while (ws < end && !is_ws((unsigned char)*ws)) ws++;
            unsigned long wlen = (unsigned long)(ws - p);
            layout_box *tb = make_text_box(C, n, p, wlen, parent_style);
            inline_add(S, block, tb, parent_style);
            p = ws;
            prev_ws = 0;
        }
        return;
    }
    if (n->type != DOM_NODE_ELEMENT) return;

    css_computed cs;
    css_compute(C->sheet, n, &cs);

    if (cs.display == CSS_DISP_NONE) return;

    if (cs.display == CSS_DISP_BLOCK) {
        /* A block inside an inline context terminates the current line and
         * lays out the block independently. */
        finish_line(S, parent_style);
        layout_box *blk = box_new(LB_BLOCK, n);
        if (!blk) return;
        blk->style = cs;
        blk->x = S->base_x + cs.margin_l;
        blk->y = S->content_y + S->total_h + cs.margin_t;
        int inner_w = S->max_w - cs.margin_l - cs.margin_r;
        if (cs.width > 0) inner_w = cs.width;
        if (inner_w < 0) inner_w = 0;
        blk->w = inner_w;
        box_attach(block, blk);
        int content_w = inner_w - cs.padding_l - cs.padding_r;
        if (content_w < 0) content_w = 0;
        int used = layout_node(C, n, blk,
                               content_w,
                               blk->x + cs.padding_l,
                               blk->y + cs.padding_t,
                               depth + 1);
        blk->h = (cs.height > 0)
               ? cs.height
               : used + cs.padding_t + cs.padding_b;
        S->total_h += cs.margin_t + blk->h + cs.margin_b;
        return;
    }

    /* Special-case <br>: forced line break. */
    if (n->tag && strcmp(n->tag, "br") == 0) {
        if (!S->line) start_line(S, block);
        if (S->line_h == 0) S->line_h = LINE_H_OF(cs.font_size);
        finish_line(S, parent_style);
        return;
    }

    if (cs.display == CSS_DISP_INLINE_BLOCK) {
        /* Treat as one atomic inline box: lay out its children as a sub-block. */
        layout_box *ib = box_new(LB_INLINE, n);
        if (!ib) return;
        ib->style = cs;
        int inner_w = cs.width > 0 ? cs.width : (S->max_w / 2);
        if (inner_w < 0) inner_w = 0;
        ib->w = inner_w + cs.padding_l + cs.padding_r;
        /* Temporarily position at origin; inline_add will place it. */
        ib->x = 0;
        ib->y = 0;
        int used = layout_node(C, n, ib,
                               inner_w,
                               cs.padding_l,
                               cs.padding_t,
                               depth + 1);
        ib->h = cs.height > 0 ? cs.height : used + cs.padding_t + cs.padding_b;
        inline_add(S, block, ib, parent_style);
        /* Adjust children to the placed position. */
        for (layout_box *c = ib->first_child; c; c = c->next_sibling) {
            c->x += ib->x;
            c->y += ib->y;
        }
        return;
    }

    /* display: inline -- recurse into children using cs as the cascading style. */
    for (struct dom_node *c = n->first_child; c; c = c->next_sibling) {
        emit_inline_subtree(C, c, block, S, &cs, depth + 1);
    }
}

/* ------------------------------------------------------------------ */
/* Block layout node.                                                   */
/* ------------------------------------------------------------------ */

/* Lay out all children of `node` into `block`.
 *   content_x / content_y: absolute viewport coords of the content-area
 *     top-left.  block->x and block->y are the OUTER box edges and are
 *     NOT modified here — they must be set by the caller.
 *   avail_w: content-area width (already subtracted padding_l + padding_r).
 *   depth: recursion guard.
 *
 * Margin collapse between adjacent sibling blocks:
 *   We track `prev_margin_b` (the bottom margin of the most-recently placed
 *   block sibling).  When placing the next block, the gap between them is
 *   MAX(prev_margin_b, cur_margin_t) — the classic CSS vertical margin
 *   collapse.  The margin added to `total_h` is that collapsed value plus
 *   the block's own height plus its bottom margin.
 *
 * Returns total content height (all lines + all block children + margins),
 * NOT including the calling block's own padding_t/b. */
static int layout_node(ctx *C, const struct dom_node *node, layout_box *block,
                       int avail_w, int content_x, int content_y, int depth)
{
    if (!node) return 0;
    if (depth >= MAX_DEPTH) return 0;

    inline_state S;
    S.line     = 0;
    S.line_x   = 0;
    S.line_h   = 0;
    S.total_h  = 0;
    S.base_x   = content_x;
    S.content_y = content_y;
    S.max_w    = avail_w > 0 ? avail_w : 0;

    /* block->x and block->y are already set by the caller.
     * Only update w here to reflect the content width we were given. */
    block->w = avail_w > 0 ? avail_w : 0;

    css_computed parent_cs;
    css_compute(C->sheet, node, &parent_cs);

    /* prev_margin_b: bottom margin of the last block-level sibling placed.
     * Used for vertical margin collapse (MAX strategy). */
    int prev_margin_b = 0;

    /* Iterate DOM children; for each, decide block vs inline at top level. */
    for (struct dom_node *c = node->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_NODE_COMMENT) continue;

        if (c->type == DOM_NODE_TEXT) {
            /* Skip whitespace-only text between blocks. */
            const char *p = c->text;
            int has_real = 0;
            while (p && *p) {
                if (!is_ws((unsigned char)*p)) { has_real = 1; break; }
                p++;
            }
            if (!has_real) continue;
            /* Non-trivial text: enter inline context.
             * If we just exited a block, carry forward any pending bottom
             * margin as a spacer (simplified: add it to total_h). */
            if (prev_margin_b > 0) {
                S.total_h += prev_margin_b;
                prev_margin_b = 0;
            }
            emit_inline_subtree(C, c, block, &S, &parent_cs, depth + 1);
            continue;
        }
        if (c->type != DOM_NODE_ELEMENT) continue;

        css_computed cs;
        css_compute(C->sheet, c, &cs);
        if (cs.display == CSS_DISP_NONE) continue;

        if (cs.display == CSS_DISP_BLOCK) {
            /* Close any open inline run before placing a block. */
            finish_line(&S, &parent_cs);

            /* Vertical margin collapse between adjacent sibling blocks:
             * gap = MAX(prev_margin_b, cs.margin_t). */
            int collapsed_gap = imax(prev_margin_b, cs.margin_t);

            layout_box *blk = box_new(LB_BLOCK, c);
            if (!blk) { prev_margin_b = cs.margin_b; continue; }
            blk->style = cs;
            /* Outer box position: content_x + margin_l, content_y + offset + gap */
            blk->x = content_x + cs.margin_l;
            blk->y = content_y + S.total_h + collapsed_gap;

            int inner_w = S.max_w - cs.margin_l - cs.margin_r;
            if (cs.width > 0) inner_w = cs.width;
            if (inner_w < 0) inner_w = 0;
            blk->w = inner_w;

            int content_w = inner_w - cs.padding_l - cs.padding_r;
            if (content_w < 0) content_w = 0;
            int used = layout_node(C, c, blk,
                                   content_w,
                                   blk->x + cs.padding_l,
                                   blk->y + cs.padding_t,
                                   depth + 1);

            blk->h = (cs.height > 0)
                   ? cs.height
                   : used + cs.padding_t + cs.padding_b;
            box_attach(block, blk);

            /* Advance total_h by the collapsed gap + block height.
             * We do NOT add cs.margin_b to total_h yet; we save it in
             * prev_margin_b so it can be collapsed with the next sibling. */
            S.total_h += collapsed_gap + blk->h;
            prev_margin_b = cs.margin_b;

        } else {
            /* inline / inline-block: stay in the inline run */
            if (prev_margin_b > 0) {
                /* Transitioning from block context back to inline: flush
                 * any pending bottom margin (can't collapse with inline). */
                S.total_h += prev_margin_b;
                prev_margin_b = 0;
            }
            emit_inline_subtree(C, c, block, &S, &parent_cs, depth + 1);
        }
    }
    finish_line(&S, &parent_cs);

    /* Flush the last block's pending bottom margin. */
    if (prev_margin_b > 0) S.total_h += prev_margin_b;

    return S.total_h;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                          */
/* ------------------------------------------------------------------ */

layout_box *layout_compute(struct dom_document *doc,
                           css_stylesheet     *sheet,
                           int                 viewport_w)
{
    if (!doc || !doc->root) return 0;

    arena *arn = (arena *)calloc(1, sizeof(*arn));
    if (!arn) return 0;

    /* Find <html>/<body>. */
    struct dom_node *html = 0, *body = 0;
    for (struct dom_node *n = doc->root->first_child; n; n = n->next_sibling) {
        if (n->type == DOM_NODE_ELEMENT && n->tag
                && strcmp(n->tag, "html") == 0) {
            html = n; break;
        }
    }
    if (html) {
        for (struct dom_node *n = html->first_child; n; n = n->next_sibling) {
            if (n->type == DOM_NODE_ELEMENT && n->tag
                    && strcmp(n->tag, "body") == 0) {
                body = n; break;
            }
        }
    }

    layout_box *root = box_new(LB_BLOCK, doc->root);
    if (!root) { free(arn); return 0; }
    root->x = 0;
    root->y = 0;
    root->w = viewport_w;
    root->h = 0;
    /* Apply UA defaults so root has sane style. */
    css_compute(sheet, html ? html : doc->root, &root->style);

    ctx C;
    C.sheet = sheet;
    C.arn   = arn;

    int used = 0;
    if (body) {
        css_computed bcs;
        css_compute(sheet, body, &bcs);
        layout_box *bb = box_new(LB_BLOCK, body);
        if (bb) {
            bb->style = bcs;
            /* Outer box: viewport origin + body margins. */
            bb->x = bcs.margin_l;
            bb->y = bcs.margin_t;
            int inner_w = viewport_w - bcs.margin_l - bcs.margin_r;
            if (inner_w < 0) inner_w = 0;
            bb->w = inner_w;
            /* Content area starts at padding offset from outer edge. */
            int body_cw = inner_w - bcs.padding_l - bcs.padding_r;
            if (body_cw < 0) body_cw = 0;
            int u = layout_node(&C, body, bb,
                                body_cw,
                                bb->x + bcs.padding_l,
                                bb->y + bcs.padding_t,
                                0);
            bb->h = u + bcs.padding_t + bcs.padding_b;
            box_attach(root, bb);
            used = bcs.margin_t + bb->h + bcs.margin_b;
        }
    } else {
        /* No <body>: lay out doc->root directly.
         * root->x/y are already 0; pass content area = (0,0). */
        used = layout_node(&C, doc->root, root, viewport_w, 0, 0, 0);
    }
    root->h = used;

    arena_reg_add(root, arn);
    return root;
}

/* Public free: also drops the arena. Only valid on a root returned by
 * layout_compute(). Sub-trees should not be passed here (no arena to take). */
void layout_free(layout_box *root)
{
    if (!root) return;
    arena *a = arena_reg_take(root);
    free_tree(root);
    if (a) {
        arena_free(a);
        free(a);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test.                                                           */
/* ------------------------------------------------------------------ */

#include "../html/html_parse.h"

int layout_selftest(void)
{
    /* ---------------------------------------------------------------- */
    /* Test 1: basic block stacking, h1 above p, text words present.    */
    /* ---------------------------------------------------------------- */
    {
        static const char src[] =
            "<body><h1>Hi</h1><p>Hello world.</p></body>";
        struct dom_document *doc = html_parse(src, sizeof(src) - 1);
        if (!doc) return -1;

        css_stylesheet *sh = css_parse("", 0);   /* UA-only */

        layout_box *root = layout_compute(doc, sh, 800);
        if (!root) return -2;

        layout_box *body = root->first_child;
        if (!body) return -3;

        layout_box *h1 = 0, *p = 0;
        for (layout_box *b = body->first_child; b; b = b->next_sibling) {
            if (b->node && b->node->tag) {
                if (strcmp(b->node->tag, "h1") == 0) h1 = b;
                else if (strcmp(b->node->tag, "p") == 0) p = b;
            }
        }
        if (!h1) return -4;
        if (!p)  return -5;
        if (!(h1->w > 0)) return -6;
        if (!(p->w > 0))  return -7;
        /* p must be strictly below h1 (p.y >= h1.y + h1.h). */
        if (!(p->y >= h1->y + h1->h)) return -8;

        /* h1 should have at least one LB_LINE with an LB_TEXT "Hi". */
        layout_box *line = h1->first_child;
        if (!line || line->kind != LB_LINE) return -9;
        layout_box *t = line->first_child;
        if (!t || t->kind != LB_TEXT || !t->text) return -10;
        if (strcmp(t->text, "Hi") != 0) return -11;

        /* p has at least two LB_TEXT words ("Hello", "world."). */
        layout_box *pline = p->first_child;
        if (!pline) return -12;
        int word_count = 0;
        for (layout_box *c = pline->first_child; c; c = c->next_sibling) {
            if (c->kind == LB_TEXT && c->text
                    && c->text[0] && c->text[0] != ' ')
                word_count++;
        }
        if (word_count < 2) return -13;

        layout_free(root);
        css_free(sh);
        (void)doc; /* intentional leak of small selftest DOM */
    }

    /* ---------------------------------------------------------------- */
    /* Test 2: long paragraph wraps to > 1 line at narrow viewport.     */
    /* "height grows" = more than one LB_LINE child inside the <p> box. */
    /* ---------------------------------------------------------------- */
    {
        /* 20 words; at 8px/char, "pneumonoultramicroscopicsilicovolcanoconiosis"
         * is 45 chars = 360px, wider than our 200px viewport — forces wrap.
         * We use shorter but numerous words to guarantee multiple lines. */
        static const char src2[] =
            "<body><p>"
            "one two three four five six seven eight nine ten "
            "eleven twelve thirteen fourteen fifteen sixteen seventeen "
            "eighteen nineteen twenty"
            "</p></body>";
        struct dom_document *doc2 = html_parse(src2, sizeof(src2) - 1);
        if (!doc2) return -20;

        css_stylesheet *sh2 = css_parse("", 0);
        /* Viewport 200px: at 8px/char a word like "seventeen" is 72px,
         * so many words won't fit on a single 200px line. */
        layout_box *root2 = layout_compute(doc2, sh2, 200);
        if (!root2) return -21;

        layout_box *body2 = root2->first_child;
        if (!body2) return -22;

        layout_box *p2 = 0;
        for (layout_box *b = body2->first_child; b; b = b->next_sibling) {
            if (b->node && b->node->tag
                    && strcmp(b->node->tag, "p") == 0) {
                p2 = b; break;
            }
        }
        if (!p2) return -23;

        /* Count LB_LINE children of p2; must be > 1 (paragraph wrapped). */
        int line_count = 0;
        for (layout_box *b = p2->first_child; b; b = b->next_sibling) {
            if (b->kind == LB_LINE) line_count++;
        }
        if (line_count < 2) return -24;   /* wrapping test failed */

        /* p2->h must be greater than a single line height (20px for 16px font). */
        int single_line_h = LINE_H_OF(16);   /* 20 */
        if (p2->h <= single_line_h) return -25;

        layout_free(root2);
        css_free(sh2);
        (void)doc2;
    }

    /* ---------------------------------------------------------------- */
    /* Test 3: tall document — content height exceeds viewport height.  */
    /* Many blocks stacked vertically should make root->h > viewport_h. */
    /* ---------------------------------------------------------------- */
    {
        /* 30 paragraphs of text; at ~20px each that is ~600px content,
         * well beyond a 400px viewport. */
        static const char src3[] =
            "<body>"
            "<p>Line 1.</p><p>Line 2.</p><p>Line 3.</p><p>Line 4.</p>"
            "<p>Line 5.</p><p>Line 6.</p><p>Line 7.</p><p>Line 8.</p>"
            "<p>Line 9.</p><p>Line 10.</p><p>Line 11.</p><p>Line 12.</p>"
            "<p>Line 13.</p><p>Line 14.</p><p>Line 15.</p><p>Line 16.</p>"
            "<p>Line 17.</p><p>Line 18.</p><p>Line 19.</p><p>Line 20.</p>"
            "<p>Line 21.</p><p>Line 22.</p><p>Line 23.</p><p>Line 24.</p>"
            "<p>Line 25.</p><p>Line 26.</p><p>Line 27.</p><p>Line 28.</p>"
            "<p>Line 29.</p><p>Line 30.</p>"
            "</body>";
        struct dom_document *doc3 = html_parse(src3, sizeof(src3) - 1);
        if (!doc3) return -30;

        css_stylesheet *sh3 = css_parse("", 0);
        int viewport_h = 400;
        layout_box *root3 = layout_compute(doc3, sh3, 800);
        if (!root3) return -31;

        /* root3->h must exceed the viewport height. */
        if (root3->h <= viewport_h) return -32;

        layout_free(root3);
        css_free(sh3);
        (void)doc3;
    }

    /* ---------------------------------------------------------------- */
    /* Test 4: display:none removes box — no child box for hidden elem. */
    /* ---------------------------------------------------------------- */
    {
        static const char src4[] =
            "<body>"
            "<p>visible</p>"
            "<p style=\"display:none\">hidden</p>"
            "<p>also visible</p>"
            "</body>";
        struct dom_document *doc4 = html_parse(src4, sizeof(src4) - 1);
        if (!doc4) return -40;

        css_stylesheet *sh4 = css_parse("", 0);
        layout_box *root4 = layout_compute(doc4, sh4, 800);
        if (!root4) return -41;

        layout_box *body4 = root4->first_child;
        if (!body4) return -42;

        /* Count p-level block children; should be 2 (hidden one absent). */
        int p_count = 0;
        for (layout_box *b = body4->first_child; b; b = b->next_sibling) {
            if (b->node && b->node->tag
                    && strcmp(b->node->tag, "p") == 0)
                p_count++;
        }
        if (p_count != 2) return -43;   /* display:none box must not appear */

        layout_free(root4);
        css_free(sh4);
        (void)doc4;
    }

    /* ---------------------------------------------------------------- */
    /* Test 5: NULL sheet (UA defaults only) must not crash.            */
    /* ---------------------------------------------------------------- */
    {
        static const char src5[] = "<body><p>null sheet test</p></body>";
        struct dom_document *doc5 = html_parse(src5, sizeof(src5) - 1);
        if (!doc5) return -50;

        layout_box *root5 = layout_compute(doc5, 0 /* NULL sheet */, 800);
        if (!root5) return -51;
        if (root5->h <= 0) return -52;

        layout_free(root5);
        (void)doc5;
    }

    return 0;
}

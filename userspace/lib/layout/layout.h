/* userspace/lib/layout/layout.h
 * ============================================================================
 * Block + inline FLOW layout engine for AutomationOS ring-3 browser pipeline.
 *
 * Walks a DOM tree (../dom/dom.h), computes css_computed for each element
 * (../css/css.h), and produces a tree of layout_box rectangles with absolute
 * viewport coordinates. The paint/render layer consumes the layout tree to
 * produce a paint list.
 *
 * FREESTANDING: malloc/free + string.h only.
 *
 * ----------------------------------------------------------------------------
 * Layout model:
 *   - Block formatting context (BFC) per block container. Blocks stack
 *     vertically, each consuming the available width (parent content width).
 *   - Inline formatting context inside a block: inline boxes and anonymous
 *     LB_TEXT segments flow horizontally, wrapped to the line width.
 *   - Wrapping is whitespace-driven (greedy word wrap on space/tab/newline).
 *   - Lines are LB_LINE pseudo-boxes that own the inline children for one
 *     line. Their y is the line's baseline-ish top within the parent block.
 *   - Text in a block container becomes anonymous inline child boxes split
 *     into LB_TEXT segments, one per word + intervening single-space.
 *
 * Box dimensions:
 *   - For blocks, width = parent_content_width - margin_l - margin_r unless
 *     an explicit `width` is given (then `width` is used; margins still
 *     subtracted from the slot). Height grows to enclose content.
 *   - Inline-block: width/height honoured if explicit; else fit content.
 *   - For text segments, w = ceil(char_count * avg_glyph_w); h = font_size.
 *     We use a fixed 8px advance per char (matches the 8x16 bitmap font);
 *     bold adds +0 (no extended glyph), italic adds +0. Documented below.
 *
 * Margin collapsing (simplified):
 *   - Adjacent vertical margins between sibling blocks collapse to the
 *     MAX of the two (not the spec-correct "max of greater" for nested
 *     containers; we don't recursively bubble through transparent ancestors).
 *
 * Padding shrinks the content area inside a block (left/right reduce the
 * inline line width; top/bottom add space before/after content).
 *
 * display: none skips the element and its entire subtree.
 *
 * KNOWN LIMITATIONS:
 *   - No float, no clear.
 *   - No position other than static (no relative/absolute/fixed/sticky).
 *   - No flex, no grid.
 *   - No transforms, no z-index (paint order = document order).
 *   - No vertical-align (text baseline math is approximate: line height
 *     = max(font_size of children) and text segments are top-aligned within
 *     the line).
 *   - No bidi / RTL.
 *   - Tables render as a stack of block rows; no column sizing.
 *   - No text justification (text-align: justify falls back to left).
 *   - text-align acts on the parent block when its inline children form a
 *     single line; multi-line alignment uses the same horizontal offset
 *     per line (no per-line shrink-to-fit).
 *   - No font-family selection; advance width is the same for every glyph.
 *   - No word-break / hyphenation. A single word longer than the line is
 *     placed on its own line and may overflow.
 *
 * Lifetime:
 *   - The returned tree is heap-owned (one allocation per box; text
 *     segments point into a malloc'd, parser-owned buffer kept alive by
 *     the layout root). layout_free() walks the tree and frees everything.
 *   - The DOM and the stylesheet passed in must outlive the layout tree:
 *     layout_box::node and layout_box::text borrow into them.
 * ============================================================================
 */

#ifndef USERSPACE_LIB_LAYOUT_LAYOUT_H
#define USERSPACE_LIB_LAYOUT_LAYOUT_H

#include "../dom/dom.h"
#include "../css/css.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Layout box kind. */
enum layout_box_kind {
    LB_BLOCK = 0,    /* block container                            */
    LB_INLINE,       /* inline element (span, a, b, ...)           */
    LB_TEXT,         /* anonymous text segment (one word + space)  */
    LB_LINE          /* anonymous line box inside a block          */
};

typedef struct layout_box {
    int x, y, w, h;                          /* px in the viewport         */
    int kind;                                /* enum layout_box_kind        */
    const struct dom_node *node;             /* originating node, may be 0  */
    const char *text;                         /* LB_TEXT: borrows; not own   */
    css_computed style;                       /* computed style for this box */
    struct layout_box *first_child, *next_sibling;
} layout_box;

/* Compute a layout tree for the document. Viewport width is in px and is
 * used as the initial content width for <html>/<body>. Height grows from
 * content; the root box's h is the total document height.
 *
 * Both `doc` and `sheet` MUST stay alive for as long as the returned tree
 * is used (layout_box::node and layout_box::text borrow into them).
 * Returns NULL on allocation failure or if `doc` is NULL. */
layout_box *layout_compute(struct dom_document *doc,
                           css_stylesheet     *sheet,
                           int                 viewport_w);

/* Free the layout tree (every box) plus any internal text-segment buffer
 * the layout engine allocated. Does NOT free the DOM or the stylesheet. */
void layout_free(layout_box *root);

/* Built-in self-test. Returns 0 on pass, negative on first failure. */
int layout_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIB_LAYOUT_LAYOUT_H */

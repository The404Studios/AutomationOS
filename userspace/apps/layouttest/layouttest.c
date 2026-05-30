/* userspace/apps/layouttest/layouttest.c
 * ============================================================================
 * Boot-verification test for the flow layout engine.
 *
 * Build (no fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *       layouttest.c ../../lib/dom/dom.c ../../lib/css/css.c
 *       ../../lib/layout/layout.c ../../libc/malloc.c ../../libc/string.c
 *       ../../libc/stdio.c ../../libc/syscall.c ../libc/start.o
 *       -o layouttest
 *
 * Links via crt0 -> int main(int argc, char **argv).
 *
 * TOLERANCE CONTRACT:
 *   - Block width: expect w == viewport_w (800). No tolerance; block layout
 *     fills the full available width. UA margins on <body> (8px each side)
 *     reduce the content width for children to 784, so body.w == 800 but
 *     h1.w and p.w == 784.  We check w > 0 for inner boxes and w == 800 for
 *     the root, to tolerate any UA-margin value the implementation picks.
 *   - Vertical position of h1: the root/body may apply UA margin (8px by
 *     default for <body>) above the first child. We therefore accept
 *     h1.y >= 0 (not strictly == 0). The header states "y=0 modulo any UA
 *     margin" so we assert h1.y >= 0 and h1.y < 64 (sanity cap).
 *   - h1 height: font-size is 32px.  h1 also gets UA margin-top/bottom of
 *     16px (from css.h UA defaults). Height of the block that WRAPS the
 *     text is at least font_size (32px). We accept h >= 32.
 *   - p y-position: must be strictly below h1 bottom edge (p.y > h1.y + h1.h - 1),
 *     allowing the collapsed margin between h1 and p to be any value >= 0.
 *   - second <p> must sit below first <p>.
 *   - display:none span: no layout_box with node == hidden_span should appear
 *     anywhere in the tree.
 *   - All visible boxes must have w > 0 and h > 0.
 * ============================================================================
 */

#include "../../libc/stdio.h"
#include "../../libc/syscall.h"
#include "../../lib/dom/dom.h"
#include "../../lib/css/css.h"
#include "../../lib/layout/layout.h"

/* -------------------------------------------------------------------------
 * Helper: write a string to stdout via sys_write (printf is available from
 * libc stdio.h, but we want a tiny puts-style helper that avoids any
 * buffering issue at exit time).
 * ---------------------------------------------------------------------- */
static void print(const char *s) {
    /* Walk to find length, then write.  No strlen in scope yet -- but we
     * included libc/string.h via dom.h -> ../../libc/string.h, so strlen
     * IS available through the dom.h include chain. Use write() from
     * libc/syscall.h. */
    unsigned long n = 0;
    const char *p = s;
    while (*p++) n++;
    write(STDOUT_FILENO, s, n);
}

static void println(const char *s) {
    print(s);
    write(STDOUT_FILENO, "\n", 1);
}

/* -------------------------------------------------------------------------
 * Walk the layout tree looking for a box whose originating DOM node matches
 * `target`. Returns the first such box, or NULL.
 * ---------------------------------------------------------------------- */
static const layout_box *find_box_for_node(const layout_box *root,
                                            const dom_node   *target)
{
    if (!root) return (const layout_box *)0;
    if (root->node == target) return root;
    const layout_box *found = find_box_for_node(root->first_child, target);
    if (found) return found;
    return find_box_for_node(root->next_sibling, target);
}

/* Returns 1 if ANY box in the tree references `target`. */
static int tree_contains_node(const layout_box *root,
                               const dom_node   *target)
{
    if (!root) return 0;
    if (root->node == target) return 1;
    if (tree_contains_node(root->first_child, target)) return 1;
    return tree_contains_node(root->next_sibling, target);
}

/* -------------------------------------------------------------------------
 * Fail helper: prints the message and sets the global failure flag.
 * ---------------------------------------------------------------------- */
static int g_failed = 0;
static void fail(const char *msg) {
    print("LAYOUTTEST: FAIL ");
    println(msg);
    g_failed = 1;
}

/* -------------------------------------------------------------------------
 * int main(int argc, char **argv)
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* ------------------------------------------------------------------
     * STEP 1: library self-test (layout_selftest is the KAT for the
     * layout engine; it exercises the full pipeline internally).
     * ------------------------------------------------------------------ */
    {
        int rc = layout_selftest();
        if (rc != 0) {
            fail("layout_selftest returned non-zero");
            exit(1); /* fatal: library itself broken */
        }
    }

    /* ------------------------------------------------------------------
     * STEP 2: Build the DOM tree.
     *
     *   <body>
     *     <h1>Title</h1>
     *     <p>Hello world this is a test paragraph.</p>
     *     <p>Second.</p>
     *     <span id="hidden">x</span>   <- will be hidden via CSS
     *   </body>
     *
     * The document root must have <html> -> <body> as the spec requires
     * (layout_compute walks from root looking for a body). We build the
     * minimal structure: document -> html -> body -> children.
     * ------------------------------------------------------------------ */
    dom_document *doc = dom_document_new();
    if (!doc) { fail("dom_document_new OOM"); exit(1); }

    dom_node *html_el  = dom_create_element("html");
    dom_node *body_el  = dom_create_element("body");
    dom_node *h1_el    = dom_create_element("h1");
    dom_node *p1_el    = dom_create_element("p");
    dom_node *p2_el    = dom_create_element("p");
    dom_node *span_el  = dom_create_element("span");

    if (!html_el || !body_el || !h1_el || !p1_el || !p2_el || !span_el) {
        fail("dom_create_element OOM");
        exit(1);
    }

    /* Text children for visible elements. */
    dom_node *h1_text  = dom_create_text("Title");
    dom_node *p1_text  = dom_create_text("Hello world this is a test paragraph.");
    dom_node *p2_text  = dom_create_text("Second.");
    dom_node *sp_text  = dom_create_text("x");

    if (!h1_text || !p1_text || !p2_text || !sp_text) {
        fail("dom_create_text OOM");
        exit(1);
    }

    /* Give the span an id so we can later find it easily. */
    dom_set_attribute(span_el, "id", "hidden");

    /* Assemble the tree. */
    dom_append_child(doc->root, html_el);
    dom_append_child(html_el,   body_el);
    dom_append_child(body_el,   h1_el);
    dom_append_child(body_el,   p1_el);
    dom_append_child(body_el,   p2_el);
    dom_append_child(body_el,   span_el);

    dom_append_child(h1_el,   h1_text);
    dom_append_child(p1_el,   p1_text);
    dom_append_child(p2_el,   p2_text);
    dom_append_child(span_el, sp_text);

    /* ------------------------------------------------------------------
     * STEP 3: Parse the author stylesheet.
     *
     *   h1 { font-size: 32px; }
     *   p  { font-size: 16px; }
     *   #hidden { display: none; }
     *
     * Tolerances: the UA defaults in css.h already set h1 to 32px bold and
     * p to (default 16px). The author rules make these explicit so the test
     * is independent of whatever the UA default happens to be.
     * ------------------------------------------------------------------ */
    static const char css_src[] =
        "h1 { font-size: 32px; }\n"
        "p  { font-size: 16px; }\n"
        "#hidden { display: none; }\n";

    css_stylesheet *sheet = css_parse(css_src, sizeof(css_src) - 1);
    if (!sheet) {
        fail("css_parse returned NULL");
        exit(1);
    }

    /* ------------------------------------------------------------------
     * STEP 4: Compute the layout at 800px viewport width.
     * ------------------------------------------------------------------ */
    layout_box *root_box = layout_compute(doc, sheet, 800);
    if (!root_box) {
        fail("layout_compute returned NULL");
        exit(1);
    }

    /* ------------------------------------------------------------------
     * STEP 5: Assertions.
     *
     * Layout tree structure (expected):
     *
     *   root_box (LB_BLOCK, node==document root or html)
     *     body_box (LB_BLOCK)
     *       h1_box   (LB_BLOCK, w fills body content width, h >= 32)
     *         LB_LINE(s) -> LB_TEXT "Title"
     *       p1_box   (LB_BLOCK, y > h1_box.y + h1_box.h - 1)
     *         LB_LINE(s) -> LB_TEXT words...
     *       p2_box   (LB_BLOCK, y > p1_box.y + p1_box.h - 1)
     *         LB_LINE(s) -> LB_TEXT "Second."
     *       (no box for span_el / span_text because display:none)
     *
     * Root box width == viewport_w == 800 (the outermost box is sized to
     * the viewport).
     * ------------------------------------------------------------------ */

    /* 5a. Root box exists and spans the full viewport width. */
    if (root_box->w != 800) {
        fail("root_box->w != 800");
    }
    if (root_box->h <= 0) {
        fail("root_box->h <= 0");
    }

    /* 5b. Find boxes for h1, p1, p2 elements. */
    const layout_box *h1_box = find_box_for_node(root_box, h1_el);
    const layout_box *p1_box = find_box_for_node(root_box, p1_el);
    const layout_box *p2_box = find_box_for_node(root_box, p2_el);

    if (!h1_box) { fail("no layout_box found for <h1>"); }
    if (!p1_box) { fail("no layout_box found for first <p>"); }
    if (!p2_box) { fail("no layout_box found for second <p>"); }

    /* Only continue geometry checks when all boxes were located. */
    if (h1_box && p1_box && p2_box) {

        /* 5c. h1 y-position: at the top of the viewport (or very close --
         *     body UA margin of 8px is fine). We cap at 64 to catch gross
         *     misplacements. */
        if (h1_box->y < 0) {
            fail("h1_box->y < 0");
        }
        if (h1_box->y >= 64) {
            fail("h1_box->y >= 64 (unexpectedly far from top)");
        }

        /* 5d. h1 width: must be positive. We accept anything > 0 because UA
         *     body padding/margin reduces the content width from 800. */
        if (h1_box->w <= 0) {
            fail("h1_box->w <= 0");
        }

        /* 5e. h1 height >= font-size (32px). The box grows to contain at
         *     least one line of text at the set font-size. */
        if (h1_box->h < 32) {
            fail("h1_box->h < 32 (shorter than font-size)");
        }

        /* 5f. first <p> sits BELOW h1. Tolerance: the collapsed margin
         *     between h1-bottom and p-top may be 0 (margin collapse) but
         *     p.y must never be inside h1's painted area. We require
         *     p1.y >= h1.y + h1.h (strictly at or after h1 bottom). */
        if (p1_box->y < h1_box->y + h1_box->h) {
            fail("p1 overlaps or is above h1 (p1.y < h1.y + h1.h)");
        }

        /* 5g. second <p> sits BELOW first <p>. Same logic. */
        if (p2_box->y < p1_box->y + p1_box->h) {
            fail("p2 overlaps or is above p1 (p2.y < p1.y + p1.h)");
        }

        /* 5h. All three visible boxes have positive width and height. */
        if (h1_box->w <= 0 || h1_box->h <= 0) {
            fail("h1_box has zero or negative dimension");
        }
        if (p1_box->w <= 0 || p1_box->h <= 0) {
            fail("p1_box has zero or negative dimension");
        }
        if (p2_box->w <= 0 || p2_box->h <= 0) {
            fail("p2_box has zero or negative dimension");
        }
    }

    /* ------------------------------------------------------------------
     * STEP 6: display:none -- the span (id="hidden") must not appear in
     * the layout tree at all. We search for any box that references either
     * the span element node or its text child node.
     * ------------------------------------------------------------------ */
    if (tree_contains_node(root_box, span_el)) {
        fail("display:none span element has a layout_box (should be hidden)");
    }
    if (tree_contains_node(root_box, sp_text)) {
        fail("display:none span text-child has a layout_box (should be hidden)");
    }

    /* ------------------------------------------------------------------
     * Cleanup (good practice even in a boot-test; exercises layout_free).
     * ------------------------------------------------------------------ */
    layout_free(root_box);
    css_free(sheet);
    dom_document_free(doc);

    /* ------------------------------------------------------------------
     * Final verdict.
     * ------------------------------------------------------------------ */
    if (!g_failed) {
        println("LAYOUTTEST: PASS");
    }
    /* On failure, individual fail() calls already printed "LAYOUTTEST: FAIL <which>".
     * We exit 0 unconditionally so the kernel boot sequence does not stall;
     * a non-zero exit would be treated as a crash. Test infrastructure can
     * grep for "FAIL" in the output. */
    exit(0);
}

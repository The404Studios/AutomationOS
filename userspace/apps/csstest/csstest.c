/*
 * csstest.c -- Boot self-test for the CSS parser + cascade (freestanding, ring 3).
 * =================================================================================
 *
 * Tests the css.{c,h} and dom.{c,h} APIs per the integration contract.
 * No standard headers; uses only what dom.h/css.h pull in transitively
 * (userspace/libc/malloc.h and userspace/libc/string.h).
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/csstest/csstest.c -o csstest.o
 *
 * Entry: crt0 (start.asm) calls  int main(int argc, char **argv)
 * Output (fd 1 / SYS_WRITE):
 *   CSSTEST: PASS
 *   -- or --
 *   CSSTEST: FAIL <assertion-name>
 *
 * Exit: SYS_EXIT(0) via crt0 after main returns 0.
 */

#include "../../lib/dom/dom.h"   /* pulls in malloc.h + string.h */
#include "../../lib/css/css.h"   /* css_stylesheet, css_compute, css_selftest */

/* =========================================================================
 * Syscall helpers (6-argument form per x86-64 SysV ABI).
 * ========================================================================= */
#define SYS_WRITE  3
#define SYS_EXIT   0

static inline long sc(long n, long a1, long a2, long a3,
                       long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 * Minimal output helpers (no stdio).
 * ========================================================================= */
static void put(const char *s)
{
    unsigned long n = strlen(s);
    sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0, 0);
}

/* =========================================================================
 * Test-result tracking.
 * ========================================================================= */
static int         g_failed    = 0;
static const char *g_fail_name = NULL;

static void fail(const char *label)
{
    if (!g_failed) {
        g_fail_name = label;
        g_failed    = 1;
    }
}

/* =========================================================================
 * The stylesheet under test.
 *
 * Rules:
 *   R1: p           -> color:#333333, font-size:14px
 *   R2: p.big       -> font-size:24px, font-weight:bold
 *   R3: #hi         -> color:red
 *   R4: div > p     -> margin-left:8px
 * ========================================================================= */
static const char CSS_SRC[] =
    "p { color: #333333; font-size: 14px; }\n"
    "p.big { font-size: 24px; font-weight: bold; }\n"
    "#hi { color: red; }\n"
    "div > p { margin-left: 8px; }\n";

/* =========================================================================
 * Colour constant for "red" named colour.
 * css.h documents 0xAARRGGBB encoding.  "red" = 0xFFFF0000.
 * ========================================================================= */
#define COLOR_RED       0xFFFF0000u
#define COLOR_DARK_GRAY 0xFF333333u

/* =========================================================================
 * run_csstest
 *
 * DOM structure:
 *
 *   doc
 *   └─ div
 *       ├─ p  id="hi"  class="big"        -- "Hi"
 *       └─ p                               -- "plain"
 *
 *   span  (root-level, OUTSIDE the div)
 * ========================================================================= */
static void run_csstest(void)
{
    /* ------------------------------------------------------------------ */
    /* Step 1: css_selftest() -- library's own KAT.                       */
    /* ------------------------------------------------------------------ */
    if (css_selftest() != 0) { fail("css_selftest"); return; }

    /* ------------------------------------------------------------------ */
    /* Step 2: Build the DOM.                                              */
    /* ------------------------------------------------------------------ */
    dom_document *doc = dom_document_new();
    if (!doc) { fail("doc_alloc"); return; }

    dom_node *div   = dom_create_element("div");
    dom_node *p_hi  = dom_create_element("p");
    dom_node *p_pln = dom_create_element("p");
    dom_node *span  = dom_create_element("span");

    if (!div || !p_hi || !p_pln || !span) {
        fail("elem_alloc");
        if (div)   dom_node_free(div);
        if (p_hi)  dom_node_free(p_hi);
        if (p_pln) dom_node_free(p_pln);
        if (span)  dom_node_free(span);
        dom_document_free(doc);
        return;
    }

    /* <div><p id="hi" class="big">Hi</p><p>plain</p></div> */
    dom_append_child(doc->root, div);
    dom_append_child(div, p_hi);
    dom_append_child(div, p_pln);

    dom_set_attribute(p_hi, "id",    "hi");
    dom_set_attribute(p_hi, "class", "big");
    dom_set_text(p_hi,  "Hi");
    dom_set_text(p_pln, "plain");

    /* span is outside the div -- appended to doc root directly, not inside div */
    dom_append_child(doc->root, span);
    dom_set_text(span, "outside");

    /* ------------------------------------------------------------------ */
    /* Step 3: Parse the stylesheet.                                       */
    /* ------------------------------------------------------------------ */
    css_stylesheet *sheet = css_parse(CSS_SRC, sizeof(CSS_SRC) - 1);
    if (!sheet) { fail("css_parse"); goto cleanup_dom; }

    /* ------------------------------------------------------------------ */
    /* Step 4a: Compute styles for p#hi.big                               */
    /*                                                                     */
    /*  Applicable rules (in cascade order):                              */
    /*    UA default: p -> font-size:16 (overridden), margin-t:12,        */
    /*                     margin-b:12 (UA defaults for <p>)              */
    /*    R1 (specificity 0,0,1): p -> color:#333333, font-size:14        */
    /*    R2 (specificity 0,1,1): p.big -> font-size:24, font-weight:bold */
    /*    R3 (specificity 1,0,0): #hi -> color:red                        */
    /*    R4 (specificity 0,0,2): div>p -> margin-left:8                  */
    /*                                                                     */
    /*  Expected (highest-specificity wins per property):                  */
    /*    color      = red    (R3 1,0,0 beats R1 0,0,1)                   */
    /*    font-size  = 24     (R2 0,1,1 beats R1 0,0,1)                   */
    /*    bold       = 1      (R2)                                         */
    /*    margin_l   = 8      (R4)                                         */
    /* ------------------------------------------------------------------ */
    {
        css_computed hi;
        css_compute(sheet, p_hi, &hi);

        if (hi.color != COLOR_RED)
            { fail("hi_color_red"); goto cleanup_sheet; }

        if (hi.font_size != 24)
            { fail("hi_fontsize_24"); goto cleanup_sheet; }

        if (hi.bold != 1)
            { fail("hi_bold"); goto cleanup_sheet; }

        if (hi.margin_l != 8)
            { fail("hi_margin_l_8"); goto cleanup_sheet; }
    }

    /* ------------------------------------------------------------------ */
    /* Step 4b: Compute styles for the second <p> (no id, no class).      */
    /*                                                                     */
    /*  Applicable rules:                                                  */
    /*    UA default: p -> margin-t:12, margin-b:12                       */
    /*    R1 (0,0,1): p -> color:#333333, font-size:14                    */
    /*    R4 (0,0,2): div>p -> margin-left:8                              */
    /*                                                                     */
    /*  Expected:                                                          */
    /*    color      = 0xFF333333                                          */
    /*    font-size  = 14                                                  */
    /*    bold       = 0                                                   */
    /*    margin_l   = 8                                                   */
    /* ------------------------------------------------------------------ */
    {
        css_computed pln;
        css_compute(sheet, p_pln, &pln);

        if (pln.color != COLOR_DARK_GRAY)
            { fail("pln_color_gray"); goto cleanup_sheet; }

        if (pln.font_size != 14)
            { fail("pln_fontsize_14"); goto cleanup_sheet; }

        if (pln.bold != 0)
            { fail("pln_bold_0"); goto cleanup_sheet; }

        if (pln.margin_l != 8)
            { fail("pln_margin_l_8"); goto cleanup_sheet; }
    }

    /* ------------------------------------------------------------------ */
    /* Step 5: Verify a non-matching element gets UA defaults only.        */
    /*                                                                     */
    /*  The <span> is:                                                     */
    /*    - NOT a <p>, so R1/R2/R4 never match.                           */
    /*    - Has no id="hi", so R3 never matches.                          */
    /*    - Not a child of div at all.                                     */
    /*                                                                     */
    /*  UA defaults for <span>: display:inline, everything else at its    */
    /*  base value.  The base font-size UA default is 16px.               */
    /*                                                                     */
    /*  Assertion: font_size == 16  (UA default, no author rule applies)  */
    /* ------------------------------------------------------------------ */
    {
        css_computed sp;
        css_compute(sheet, span, &sp);

        if (sp.font_size != 16)
            { fail("span_fontsize_ua_default"); goto cleanup_sheet; }
    }

cleanup_sheet:
    css_free(sheet);

cleanup_dom:
    dom_document_free(doc);
}

/* =========================================================================
 * Entry point (called by crt0 / start.asm).
 * ========================================================================= */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    run_csstest();

    if (!g_failed) {
        put("CSSTEST: PASS\n");
    } else {
        put("CSSTEST: FAIL ");
        put(g_fail_name ? g_fail_name : "unknown");
        put("\n");
    }

    /* crt0 will call SYS_EXIT(return_value); we always return 0. */
    return 0;
}

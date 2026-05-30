/*
 * domtest.c -- Boot self-test for the DOM data model (freestanding, ring 3).
 * ==========================================================================
 *
 * Exercises the dom.{c,h} API per the integration contract.  No standard
 * headers; uses only userspace/libc/malloc.h and userspace/libc/string.h
 * (transitively pulled in by dom.h itself).
 *
 * Build (NO fs:0x28 canary -- verified by objdump):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/domtest/domtest.c -o domtest.o
 *
 * Entry: crt0 (start.asm) calls  int main(int argc, char **argv)
 * Output (fd 1 / SYS_WRITE):
 *   DOMTEST: PASS
 *   -- or --
 *   DOMTEST: FAIL <assertion-name>
 *
 * Exit: SYS_EXIT(0) via crt0 after main returns 0.
 */

#include "../../lib/dom/dom.h"          /* pulls in malloc.h + string.h */
#include "../../lib/dom/dom_selector.h" /* dom_selector_selftest() */
#include "../../lib/dom/dom_event.h"    /* dom_event_selftest() */
#include "../../lib/dom/dom_serialize.h"/* dom_serialize_selftest() */
#include "../../lib/dom/dom_util.h"     /* dom_util_selftest() */

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
static int g_failed = 0;               /* first-failure name printed, then exit */
static const char *g_fail_name = NULL;

/* Call when an assertion fails.  Records the failure label (first wins). */
static void fail(const char *label)
{
    if (!g_failed) {
        g_fail_name = label;
        g_failed = 1;
    }
}

/* =========================================================================
 * Helper: count direct element children of a node.
 * ========================================================================= */
static int count_element_children(const dom_node *n)
{
    int c = 0;
    dom_node *ch = n->first_child;
    while (ch) {
        if (ch->type == DOM_NODE_ELEMENT) c++;
        ch = ch->next_sibling;
    }
    return c;
}

/* =========================================================================
 * Main DOM self-test.
 *
 * Builds:
 *   <html>
 *     <body>
 *       <div id="root">
 *         <p class="x">Hello</p>
 *         <span>World</span>
 *       </div>
 *     </body>
 *   </html>
 *
 * Then exercises the contract assertions from the task brief.
 * ========================================================================= */
static void run_domtest(void)
{
    /* ------------------------------------------------------------------ */
    /* 1. Create document and verify root type.                           */
    /* ------------------------------------------------------------------ */
    dom_document *doc = dom_document_new();
    if (!doc) { fail("doc_alloc"); return; }

    if (doc->root->type != DOM_NODE_DOCUMENT) { fail("root_type"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 2. Build the element tree.                                         */
    /* ------------------------------------------------------------------ */
    dom_node *html = dom_create_element("html");
    dom_node *body = dom_create_element("body");
    dom_node *div  = dom_create_element("div");
    dom_node *p    = dom_create_element("p");
    dom_node *span = dom_create_element("span");

    if (!html || !body || !div || !p || !span) {
        fail("elem_alloc");
        /* best-effort free of allocated nodes */
        if (html) { dom_node_free(html); html = NULL; }
        if (body) { dom_node_free(body); body = NULL; }
        if (div)  { dom_node_free(div);  div  = NULL; }
        if (p)    { dom_node_free(p);    p    = NULL; }
        if (span) { dom_node_free(span); span = NULL; }
        goto cleanup;
    }

    /* Build tree: doc -> html -> body -> div -> { p, span } */
    dom_append_child(doc->root, html);
    dom_append_child(html, body);
    dom_append_child(body, div);
    dom_append_child(div, p);
    dom_append_child(div, span);

    /* Set attributes. */
    dom_set_attribute(div, "id", "root");
    dom_set_attribute(p, "class", "x");

    /* Set text content of leaf elements. */
    dom_set_text(p, "Hello");    /* p now has a single TEXT child "Hello"   */
    dom_set_text(span, "World"); /* span now has a single TEXT child "World" */

    /* ------------------------------------------------------------------ */
    /* 3. doc->root->type == DOM_NODE_DOCUMENT  (re-checked here)         */
    /* ------------------------------------------------------------------ */
    if (doc->root->type != DOM_NODE_DOCUMENT) { fail("root_type_check"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 4. dom_get_element_by_id("root") returns the div.                  */
    /* ------------------------------------------------------------------ */
    dom_node *found_div = dom_get_element_by_id(doc, "root");
    if (found_div != div) { fail("get_by_id"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 5. div has exactly 2 element children (p and span).                */
    /* ------------------------------------------------------------------ */
    if (count_element_children(div) != 2) { fail("div_child_count"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 6. dom_get_text(div) == "HelloWorld"  (malloc'd; we must free).    */
    /* ------------------------------------------------------------------ */
    const char *div_text = dom_get_text(div);
    if (!div_text) { fail("div_text_null"); goto cleanup; }
    if (strcmp(div_text, "HelloWorld") != 0) {
        free((void *)div_text);
        fail("div_text_value");
        goto cleanup;
    }
    free((void *)div_text);

    /* ------------------------------------------------------------------ */
    /* 7. dom_get_attribute(p, "class") == "x".                          */
    /* ------------------------------------------------------------------ */
    const char *cls = dom_get_attribute(p, "class");
    if (!cls || strcmp(cls, "x") != 0) { fail("get_attr_class"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 8. dom_set_attribute round-trip: add "data-v"/"42", read it back.  */
    /* ------------------------------------------------------------------ */
    dom_set_attribute(p, "data-v", "42");
    const char *dv = dom_get_attribute(p, "data-v");
    if (!dv || strcmp(dv, "42") != 0) { fail("set_attr_roundtrip"); goto cleanup; }

    /* ------------------------------------------------------------------ */
    /* 9. dom_remove_attribute("data-v"): attribute must no longer exist. */
    /* ------------------------------------------------------------------ */
    dom_remove_attribute(p, "data-v");
    if (dom_get_attribute(p, "data-v") != NULL) {
        fail("remove_attr");
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /* 10. dom_set_text(p, "Goodbye") then dom_get_text(p) == "Goodbye".  */
    /*     dom_set_text on an ELEMENT drops all children and adds one TEXT.*/
    /* ------------------------------------------------------------------ */
    dom_set_text(p, "Goodbye");
    const char *p_text = dom_get_text(p);
    if (!p_text) { fail("set_text_null"); goto cleanup; }
    if (strcmp(p_text, "Goodbye") != 0) {
        free((void *)p_text);
        fail("set_text_value");
        goto cleanup;
    }
    free((void *)p_text);

    /* ------------------------------------------------------------------ */
    /* 11. dom_selftest() (library's own internal test suite).            */
    /* ------------------------------------------------------------------ */
    if (dom_selftest() != 0) { fail("dom_selftest"); goto cleanup; }

cleanup:
    dom_document_free(doc);
}

/* =========================================================================
 * Entry point (called by crt0 / start.asm).
 * ========================================================================= */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    run_domtest();

    /* -----------------------------------------------------------------------
     * Call the four additional DOM library self-tests.  Each returns 0 on
     * success or non-zero on any internal assertion failure.  The first
     * failure label recorded by fail() is what appears in the FAIL line.
     * --------------------------------------------------------------------- */
    if (!g_failed) {
        int sr = dom_selector_selftest();
        if (sr != 0) {
            /* Debug aid: print the raw return value so the log shows which
             * internal assertion tripped (negative = -(line number)).     */
            char nb[16], tmp[16];
            int v = (sr < 0) ? -sr : sr, j = 0, k = 0;
            if (v == 0) tmp[j++] = '0';
            while (v) { tmp[j++] = (char)('0' + v % 10); v /= 10; }
            if (sr < 0) nb[k++] = '-';
            while (j) nb[k++] = tmp[--j];
            nb[k] = 0;
            put("DOMTEST: dom_selector ret="); put(nb); put("\n");
            fail("dom_selector_selftest");
        }
    }
    if (!g_failed && dom_event_selftest()    != 0)  { fail("dom_event_selftest");    }
    if (!g_failed && dom_serialize_selftest()!= 0)  { fail("dom_serialize_selftest"); }
    if (!g_failed && dom_util_selftest()     != 0)  { fail("dom_util_selftest");     }

    if (!g_failed) {
        put("DOMTEST: PASS\n");
    } else {
        put("DOMTEST: FAIL ");
        put(g_fail_name ? g_fail_name : "unknown");
        put("\n");
    }

    /* crt0 will call SYS_EXIT(return_value); we always return 0. */
    return 0;
}

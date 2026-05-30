/*
 * userspace/apps/htmltest/htmltest.c
 *
 * Boot-time HTML parser validation test.
 * Freestanding ring-3. Linked with crt0 -> int main(int argc, char **argv).
 *
 * Build flags (NO fs:0x28):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Depends on:
 *   userspace/lib/dom/dom.h         (dom_node, dom_document, dom_get_element_by_id, ...)
 *   userspace/lib/html/html_parse.h (html_parse, html_selftest, html_get_inline_scripts)
 *   userspace/libc/syscall.h        (SYS_WRITE, SYS_EXIT via exit())
 *   userspace/libc/string.h         (strcmp, strstr, strlen)
 *   userspace/libc/malloc.h         (free)
 */

#include "../../lib/dom/dom.h"
#include "../../lib/html/html_parse.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/malloc.h"

/* ------------------------------------------------------------------ */
/*  Minimal write helper -- no printf dependency.                     */
/* ------------------------------------------------------------------ */
static void print(const char *s)
{
    unsigned long len = strlen(s);
    write(STDOUT_FILENO, s, len);
}

static void fail(const char *which)
{
    print("HTMLTEST: FAIL ");
    print(which);
    print("\n");
    exit(0);
}

/* ------------------------------------------------------------------ */
/*  Count direct children of a given element tag name.                */
/* ------------------------------------------------------------------ */
static int count_direct_children_by_tag(const dom_node *parent,
                                        const char *tag)
{
    int n = 0;
    dom_node *c = parent->first_child;
    while (c) {
        if (c->type == DOM_NODE_ELEMENT && strcmp(c->tag, tag) == 0)
            n++;
        c = c->next_sibling;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Return first direct child element matching tag; NULL if none.     */
/* ------------------------------------------------------------------ */
static dom_node *first_child_elem(const dom_node *parent, const char *tag)
{
    dom_node *c = parent->first_child;
    while (c) {
        if (c->type == DOM_NODE_ELEMENT && strcmp(c->tag, tag) == 0)
            return c;
        c = c->next_sibling;
    }
    return (dom_node *)0;
}

/* ------------------------------------------------------------------ */
/*  Walk helpers                                                       */
/* ------------------------------------------------------------------ */

/* Find first descendant element with matching tag (depth-first). */
typedef struct { const char *tag; dom_node *found; } find_tag_ctx;
static void find_tag_visitor(dom_node *n, void *ctx_)
{
    find_tag_ctx *ctx = (find_tag_ctx *)ctx_;
    if (ctx->found) return;
    if (n->type == DOM_NODE_ELEMENT && strcmp(n->tag, ctx->tag) == 0)
        ctx->found = n;
}
static dom_node *find_first_by_tag(dom_node *root, const char *tag)
{
    find_tag_ctx ctx;
    ctx.tag   = tag;
    ctx.found = (dom_node *)0;
    dom_walk(root, find_tag_visitor, &ctx);
    return ctx.found;
}

/* Find first COMMENT descendant whose text contains needle. */
typedef struct { const char *needle; dom_node *found; } find_comment_ctx;
static void find_comment_visitor(dom_node *n, void *ctx_)
{
    find_comment_ctx *ctx = (find_comment_ctx *)ctx_;
    if (ctx->found) return;
    if (n->type == DOM_NODE_COMMENT && n->text &&
        strstr(n->text, ctx->needle))
        ctx->found = n;
}
static dom_node *find_comment_containing(dom_node *root, const char *needle)
{
    find_comment_ctx ctx;
    ctx.needle = needle;
    ctx.found  = (dom_node *)0;
    dom_walk(root, find_comment_visitor, &ctx);
    return ctx.found;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* ----------------------------------------------------------------
     * Step 1: library self-test (KAT)
     * ---------------------------------------------------------------- */
    if (html_selftest() != 0)
        fail("html_selftest");

    /* ----------------------------------------------------------------
     * Step 2: parse known document
     * ---------------------------------------------------------------- */
    static const char DOC[] =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "  <head><title>Hi</title></head>\n"
        "  <body>\n"
        "    <div id=\"main\" class=\"container\">\n"
        "      <h1>Hello</h1>\n"
        "      <p>World <a href=\"/x\">link</a></p>\n"
        "      <ul><li>a</li><li>b</li></ul>\n"
        "      <!-- comment -->\n"
        "      <img src=\"/i.png\">\n"
        "    </div>\n"
        "    <script>var x=1;</script>\n"
        "  </body>\n"
        "</html>\n";

    unsigned long doc_len = strlen(DOC);
    struct dom_document *doc = html_parse(DOC, doc_len);
    if (!doc)
        fail("html_parse returned NULL");

    /* ----------------------------------------------------------------
     * Assertion A1: root is DOM_NODE_DOCUMENT
     * ---------------------------------------------------------------- */
    if (!doc->root)
        fail("A1: doc->root is NULL");
    if (doc->root->type != DOM_NODE_DOCUMENT)
        fail("A1: root is not DOM_NODE_DOCUMENT");

    /* ----------------------------------------------------------------
     * Assertion A2: document has an <html> element child
     * ---------------------------------------------------------------- */
    dom_node *html_el = first_child_elem(doc->root, "html");
    if (!html_el)
        fail("A2: no <html> child under document root");

    /* ----------------------------------------------------------------
     * Assertion A3: <head> contains <title> with text "Hi"
     * ---------------------------------------------------------------- */
    dom_node *head_el = first_child_elem(html_el, "head");
    if (!head_el)
        fail("A3: no <head> under <html>");

    dom_node *title_el = first_child_elem(head_el, "title");
    if (!title_el)
        fail("A3: no <title> under <head>");

    {
        const char *t = dom_get_text(title_el);
        if (!t)
            fail("A3: dom_get_text(title) returned NULL");
        int ok = (strcmp(t, "Hi") == 0);
        /* dom_get_text on an ELEMENT returns a malloc'd string */
        free((void *)t);
        if (!ok)
            fail("A3: <title> text is not \"Hi\"");
    }

    /* ----------------------------------------------------------------
     * Assertion A4: <body> exists and has a #main div with class "container"
     * ---------------------------------------------------------------- */
    dom_node *body_el = first_child_elem(html_el, "body");
    if (!body_el)
        fail("A4: no <body> under <html>");

    dom_node *main_div = dom_get_element_by_id(doc, "main");
    if (!main_div)
        fail("A4: dom_get_element_by_id(\"main\") returned NULL");
    if (strcmp(main_div->tag, "div") != 0)
        fail("A4: #main element is not a <div>");

    {
        const char *cls = dom_get_attribute(main_div, "class");
        if (!cls || strcmp(cls, "container") != 0)
            fail("A4: #main class is not \"container\"");
    }

    /* ----------------------------------------------------------------
     * Assertion A5: #main has one <h1> with text "Hello"
     * ---------------------------------------------------------------- */
    {
        int h1_count = count_direct_children_by_tag(main_div, "h1");
        if (h1_count != 1)
            fail("A5: #main does not have exactly 1 <h1> child");

        dom_node *h1_el = first_child_elem(main_div, "h1");
        const char *t   = dom_get_text(h1_el);
        if (!t)
            fail("A5: dom_get_text(<h1>) returned NULL");
        int ok = (strcmp(t, "Hello") == 0);
        free((void *)t);
        if (!ok)
            fail("A5: <h1> text is not \"Hello\"");
    }

    /* ----------------------------------------------------------------
     * Assertion A6: the <p> has an <a> child with href="/x"
     * ---------------------------------------------------------------- */
    {
        dom_node *p_el = first_child_elem(main_div, "p");
        if (!p_el)
            fail("A6: no <p> under #main");

        dom_node *a_el = first_child_elem(p_el, "a");
        if (!a_el)
            fail("A6: no <a> under <p>");

        const char *href = dom_get_attribute(a_el, "href");
        if (!href || strcmp(href, "/x") != 0)
            fail("A6: <a> href is not \"/x\"");
    }

    /* ----------------------------------------------------------------
     * Assertion A7: the <ul> has exactly 2 <li> children
     * ---------------------------------------------------------------- */
    {
        dom_node *ul_el = first_child_elem(main_div, "ul");
        if (!ul_el)
            fail("A7: no <ul> under #main");

        int li_count = count_direct_children_by_tag(ul_el, "li");
        if (li_count != 2)
            fail("A7: <ul> does not have exactly 2 <li> children");
    }

    /* ----------------------------------------------------------------
     * Assertion A8: <img> exists (void element, no close tag required)
     * ---------------------------------------------------------------- */
    {
        dom_node *img_el = find_first_by_tag(main_div, "img");
        if (!img_el)
            fail("A8: no <img> found under #main");

        /* Also verify src attribute is present */
        const char *src = dom_get_attribute(img_el, "src");
        if (!src || strcmp(src, "/i.png") != 0)
            fail("A8: <img> src is not \"/i.png\"");
    }

    /* ----------------------------------------------------------------
     * Assertion A9: comment node whose text contains "comment"
     * ---------------------------------------------------------------- */
    {
        dom_node *cmt = find_comment_containing(main_div, "comment");
        if (!cmt)
            fail("A9: no comment node containing \"comment\" found under #main");
    }

    /* ----------------------------------------------------------------
     * Assertion A10: html_get_inline_scripts returns 1 entry "var x=1;"
     * ---------------------------------------------------------------- */
    {
        int count = 0;
        char **scripts = html_get_inline_scripts(doc, &count);
        if (count != 1)
            fail("A10: html_get_inline_scripts count != 1");
        if (!scripts || !scripts[0])
            fail("A10: html_get_inline_scripts returned NULL entry");
        if (strcmp(scripts[0], "var x=1;") != 0)
            fail("A10: inline script body is not \"var x=1;\"");

        /* Free the returned array (each entry + array itself) */
        free(scripts[0]);
        free(scripts);
    }

    /* ----------------------------------------------------------------
     * All assertions passed.
     * ---------------------------------------------------------------- */
    dom_document_free(doc);

    print("HTMLTEST: PASS\n");
    exit(0);

    /* Unreachable; suppress no-return warning for non-noreturn main */
    return 0;
}

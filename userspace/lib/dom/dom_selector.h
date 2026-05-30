/*
 * dom_selector.h -- CSS selector matcher for AutomationOS browser DOM.
 * ====================================================================
 *
 * Backs querySelector() / querySelectorAll() for the freestanding
 * userspace browser.  Freestanding ring-3 -- no libc, no stdio.
 * Build flags: -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *              -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *              -mstackrealign -O2
 *
 * SUPPORTED selector forms
 * -------------------------
 *   Type selector         div
 *   Class selector        .foo
 *   ID selector           #bar
 *   Universal selector    *
 *   Attribute presence    [href]
 *   Attribute exact       [href="value"]  or  [href='value']
 *   Compound              div.foo#bar[attr="v"]   (all tests on one element)
 *   Descendant combinator "a b"             (space, b anywhere inside a)
 *   Child combinator      "a > b"           (b is direct child of a)
 *   Selector list         "a, b, c"         (match if ANY branch matches)
 *
 * NOT SUPPORTED (returns -1 = malformed if encountered in an otherwise
 * ambiguous position; silently unsupported in standalone contexts):
 *   Adjacent sibling      a + b
 *   General sibling       a ~ b
 *   Pseudo-classes        :hover :first-child :nth-child() etc.
 *   Pseudo-elements       ::before ::after etc.
 *   Attribute operators   [attr~=v] [attr|=v] [attr^=v] [attr$=v] [attr*=v]
 *   Namespace             ns|elem
 *   :is() :not() :where() :has()
 *
 * Case rules (HTML):
 *   Tag names   -- case-insensitive (dom.h stores tags lowercased already)
 *   Class names -- case-sensitive
 *   ID values   -- case-sensitive (per HTML spec)
 *   Attr names  -- case-insensitive (dom.h stores attr names lowercased)
 *
 * Threading: single-threaded; no internal state between calls.
 */
#ifndef DOM_SELECTOR_H
#define DOM_SELECTOR_H

#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dom_selector_match
 * ------------------
 * Test whether element `el` matches `selector`.
 *
 *  Returns:  1  -- match
 *            0  -- no match
 *           <0  -- malformed / unsupported selector
 *
 * Only tests the element itself (no tree traversal).  For compound +
 * combinatorial selectors the full selector is evaluated against the
 * element in its tree context (parent / ancestor chain is walked).
 */
int dom_selector_match(const struct dom_node *el, const char *selector);

/*
 * dom_query_selector
 * ------------------
 * Return the first descendant of `root` (depth-first, document order)
 * that matches `selector`.  `root` itself is NOT tested (mirrors the
 * browser behaviour where root is the context node, not a candidate).
 *
 * Returns NULL if no match or on malformed selector.
 */
struct dom_node *dom_query_selector(const struct dom_node *root,
                                    const char *selector);

/*
 * dom_query_selector_all
 * ----------------------
 * Find ALL matching descendants of `root` (depth-first, document order).
 * Results are written into caller-provided `out[0..max-1]`.
 *
 * Returns the number of matches written (may be less than total if `max`
 * is too small).  Returns 0 on no match or malformed selector.
 */
int dom_query_selector_all(const struct dom_node *root,
                           const char *selector,
                           struct dom_node **out, int max);

/*
 * dom_selector_selftest
 * ---------------------
 * Builds a small DOM in-process, runs a battery of match / query cases,
 * and returns 0 on full pass or a negative failure code.
 * Safe to call in a freestanding build (uses only malloc/free/string).
 */
int dom_selector_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_SELECTOR_H */

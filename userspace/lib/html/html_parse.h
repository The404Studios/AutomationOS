/* userspace/lib/html/html_parse.h
 * ============================================================================
 * HTML5-subset parser for AutomationOS ring-3 browser pipeline.
 *
 * Produces a real DOM tree (struct dom_document / struct dom_node defined in
 * ../dom/dom.h). Uses a small tokenizer + tree-builder. Not a full HTML5
 * implementation, but real enough that a hand-written page or typical
 * server-rendered HTML produces a sensible tree.
 *
 * FREESTANDING: no libc, no stdio, no standard headers. Allocation goes
 * through userspace/libc/malloc.h (malloc/free/calloc/realloc). String ops
 * via userspace/libc/string.h.
 *
 * ----------------------------------------------------------------------------
 * Supported element names (lowercased on parse; unknown tags treated as
 * generic inline elements):
 *   html head body title meta link script style
 *   div span p a h1 h2 h3 h4 h5 h6
 *   ul ol li
 *   table thead tbody tr th td
 *   img br hr
 *   b i em strong code pre
 *   header footer nav section article main aside
 *   form input button label select option textarea
 *
 * Void (self-closing) elements: br hr img input meta link
 *   (These are auto-closed regardless of trailing "/" or "</tag>".)
 *
 * Raw-text elements: script, style
 *   (Body is consumed as a single text node until the matching close tag;
 *    no nested-tag parsing happens inside.)
 *
 * Entities supported:
 *   &amp; &lt; &gt; &quot; &apos; &nbsp;
 *   &#NN; (decimal)  &#xHH; (hex)
 *   A small named-entity table (copy, reg, mdash, ndash, hellip, ...).
 *   Unknown entities are emitted literally (leading '&' preserved).
 *
 * Comments: <!-- ... --> are consumed and recorded as DOM_NODE_COMMENT
 *           children (so they don't disappear silently).
 *
 * <!DOCTYPE ...> is skipped (no doctype node is created).
 *
 * Insertion modes implemented (simplified vs. the HTML5 spec):
 *   INITIAL, IN_HEAD, IN_BODY, AFTER_BODY
 *   - <head>/<body> tags transition modes.
 *   - Most content auto-promotes to IN_BODY if seen before <body> opens.
 *   - </html> / </body> transition to AFTER_BODY but content after is still
 *     attached to <body> for forgiveness.
 *
 * KNOWN LIMITATIONS (dropped intentionally):
 *   - No active formatting element list / "adoption agency" algorithm.
 *     Misnested </b></i> may close out of order; we just pop until match.
 *   - No <table> foster parenting; <td>/<tr> outside a table become generic
 *     inline boxes.
 *   - No CDATA handling outside script/style (everything is HTML-mode).
 *   - No character-encoding sniffing; input is assumed UTF-8 / ASCII-clean.
 *   - No SVG / MathML namespace switching.
 *   - <template>, <noscript>, <iframe>, frameset are not specially handled
 *     (treated as generic elements).
 *   - Attributes without values get value = "" (the empty string).
 *   - Attribute names are lowercased; values are kept verbatim.
 *
 * Tree ownership: the returned struct dom_document and the entire dom_node
 * subtree are heap-owned. Strings (tag, attr name/value, text) are also
 * heap-owned (malloc'd inside the parser). The caller is responsible for
 * tearing the tree down (a future dom_document_free in ../dom/dom.h).
 * ============================================================================
 */

#ifndef USERSPACE_LIB_HTML_HTML_PARSE_H
#define USERSPACE_LIB_HTML_HTML_PARSE_H

#include "../dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a full HTML document. `html` is a buffer of length `len` bytes; it
 * does NOT need to be NUL-terminated (we honour `len`). Returns a freshly
 * allocated dom_document whose root has DOM_NODE_DOCUMENT and a single
 * <html> element child (with <head> and <body> sub-elements). Returns NULL
 * on allocation failure or if `html` is NULL. */
struct dom_document *html_parse(const char *html, unsigned long len);

/* Parse an HTML fragment (used by Element.innerHTML setter). Returns a
 * freshly-created anonymous element (tag = "fragment") whose children are
 * the parsed fragment in document order. Caller adopts the children into
 * its own tree (or frees the wrapper). Returns NULL on allocation failure. */
struct dom_node *html_parse_fragment(const char *html, unsigned long len);

/* Return a list of "src" attribute values for <script src="..."> in
 * encountered order. *count_out is set to the number of entries.
 * The returned array AND each string is malloc'd; the caller frees them
 * with free() (each entry, then the array itself). Returns NULL if no
 * scripts were found (*count_out = 0). */
char **html_get_script_srcs(const struct dom_document *doc, int *count_out);

/* Return a list of inline <script>...</script> bodies (text between the
 * open and close tags) in encountered order, EXCLUDING scripts that have
 * a src= attribute. Same ownership rules as html_get_script_srcs. */
char **html_get_inline_scripts(const struct dom_document *doc, int *count_out);

/* Built-in self-test. Parses a small known document and asserts a handful
 * of structural invariants. Returns 0 on pass, negative on first failure. */
int html_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIB_HTML_HTML_PARSE_H */

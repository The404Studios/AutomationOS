/*
 * dom_serialize.h -- HTML serialiser for AutomationOS browser DOM.
 * ================================================================
 *
 * Backs Element.innerHTML (GET) and Element.outerHTML.
 * Freestanding ring-3 userspace.  No libc/stdio.
 * Uses ONLY:
 *   - userspace/libc/malloc.h  (malloc/free/calloc/realloc)
 *   - userspace/libc/string.h  (strlen/strcmp/memcpy/memset/...)
 *
 * Build flags (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *       -mstackrealign -O2
 *
 * Threading: single-threaded.  No locks.
 */
#ifndef DOM_SERIALIZE_H
#define DOM_SERIALIZE_H

#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dom_serialize_inner
 * -------------------
 * Serialise a node's CHILDREN as HTML (innerHTML GET).
 * Writes into the caller's buffer `out` up to `cap-1` bytes, NUL-terminates.
 *
 * Returns bytes written (excluding NUL) on success, or a negative value on
 * error (buffer overflow, depth overflow, NULL inputs, …).
 *
 * If `cap` == 0 the function returns -1 immediately (no write possible).
 * If `el` is NULL returns -1.
 */
long dom_serialize_inner(const struct dom_node *el, char *out, unsigned long cap);

/*
 * dom_serialize_outer
 * -------------------
 * Serialise the node ITSELF plus its children (outerHTML).
 * Same buffer/return convention as dom_serialize_inner.
 *
 * If `el` is a DOM_NODE_DOCUMENT node, serialises the first ELEMENT child
 * (the document element), mirroring browser document.documentElement.outerHTML.
 */
long dom_serialize_outer(const struct dom_node *el, char *out, unsigned long cap);

/*
 * dom_html_escape_text
 * --------------------
 * Escape a string for HTML text context:
 *   &  ->  &amp;
 *   <  ->  &lt;
 *   >  ->  &gt;
 *
 * Writes into `out` up to `cap-1` bytes, NUL-terminates.
 * Returns bytes written (excluding NUL), or -1 on overflow / NULL input.
 */
long dom_html_escape_text(const char *s, char *out, unsigned long cap);

/*
 * dom_html_escape_attr
 * --------------------
 * Escape a string for an HTML attribute-value context.
 * In addition to the text escapes, also escapes:
 *   "  ->  &quot;
 *
 * Writes into `out` up to `cap-1` bytes, NUL-terminates.
 * Returns bytes written (excluding NUL), or -1 on overflow / NULL input.
 */
long dom_html_escape_attr(const char *s, char *out, unsigned long cap);

/*
 * dom_serialize_selftest
 * ----------------------
 * Builds a small DOM programmatically, serialises with dom_serialize_outer,
 * and asserts the result matches a known expected string byte-for-byte.
 *
 * Returns 0 on pass, negative on first failure.
 */
int dom_serialize_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_SERIALIZE_H */

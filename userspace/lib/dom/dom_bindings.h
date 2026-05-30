/*
 * dom_bindings.h -- expose the DOM to JavaScript.
 * ================================================
 *
 * Wires the dom_document (and its tree of dom_node) up to the JS engine
 * via js_native_* so scripts can use the standard browser globals:
 *
 *   document.getElementById("id")        -> Element | null
 *   document.createElement("div")        -> Element
 *   document.createTextNode("hi")        -> Text
 *   document.body, document.head, document.documentElement
 *   document.title  (get/set)
 *   document.URL    (get only)
 *
 *   element.tagName                       (string, uppercased)
 *   element.id (get/set)
 *   element.className (get/set)
 *   element.textContent (get/set)
 *   element.innerHTML (get/set)             [SET only sets textContent
 *                                            until html_parse_fragment is
 *                                            wired -- see dom_bindings.c]
 *   element.children                       (array-like)
 *   element.firstChild, .lastChild,
 *   element.parentNode, .nextSibling, .previousSibling
 *
 *   element.appendChild(child)
 *   element.removeChild(child)
 *   element.insertBefore(child, ref)
 *   element.getAttribute(name)
 *   element.setAttribute(name, value)
 *   element.hasAttribute(name)
 *   element.removeAttribute(name)
 *   element.querySelector / querySelectorAll          [stubs, see below]
 *
 *   text.textContent / text.data / text.nodeValue (TEXT nodes)
 *
 *   console.log/.error/.warn/.info        (kept from js_install_builtins)
 *
 * Lifecycle note: js_eval() resets the arena AND the native-class
 * registry. So an embedder MUST call dom_bindings_install(vm, doc)
 * AFTER each js_new()/js_eval() for which the DOM should be visible.
 * (Typical pattern: dom_bindings_install, parse, run_program, free
 *  document only when leaving the page.)
 */
#ifndef DOM_BINDINGS_H
#define DOM_BINDINGS_H

#include "dom.h"
#include "../js/js.h"
#include "../js/js_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install all DOM globals into `vm`. The `doc` pointer must remain
 * valid for as long as the VM continues to use these bindings (the
 * wrapper objects only hold the dom_node pointers, not copies). */
void dom_bindings_install(js_vm *vm, dom_document *doc);

/* Selftest: programmatically builds a tiny document, exposes it to the
 * VM, runs a JS snippet that exercises createElement/setAttribute/
 * appendChild/getElementById/textContent, and asserts the result.
 *
 * Returns 0 on pass, < 0 on failure.
 */
int  dom_bindings_selftest(js_vm *vm);

#ifdef __cplusplus
}
#endif

#endif /* DOM_BINDINGS_H */

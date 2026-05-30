/*
 * js_url.h -- URL, URLSearchParams, encodeURI* / decodeURI* for AutomationOS JS engine.
 * ======================================================================================
 *
 * Exposes the following Web-standard globals in the JS engine:
 *
 *   encodeURIComponent(s)   -- percent-encode all non-unreserved bytes
 *                              (unreserved: A-Za-z0-9 _ . ~ ! * ' ( ) - )
 *   decodeURIComponent(s)   -- decode %XX sequences; throws on bad input
 *   encodeURI(s)            -- like encodeURIComponent but leaves the
 *                              URI-legal set :/?#[]@!$&'()*+,;= alone
 *   decodeURI(s)            -- decode %XX but preserve the encodeURI-
 *                              reserved characters if found encoded
 *   URL(href[, base])       -- constructor; resolves relative URLs against base
 *     .href .protocol .host .hostname .port .pathname .search .hash .origin
 *   URLSearchParams(init)   -- key/value query-string collection
 *     .get(k) .set(k,v) .has(k) .delete(k) .append(k,v) .toString()
 *
 * URL parsing (C-side):
 *   Accepts absolute URLs (scheme://authority/path?query#fragment) and
 *   relative path-only references.  Resolution follows RFC 3986 §5.
 *   Port is stored as a decimal string or "" for default ports.
 *   Supported components: scheme, host, port, pathname, search, hash, origin.
 *
 * URLSearchParams iterable interface is NOT implemented.  The JS-side
 * .entries()/.keys()/.values()/.forEach() iteration protocol is omitted;
 * document this as a known gap.
 *
 * Build flags (NO fs:0x28 canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * Dependencies:
 *   #include "../js/js.h"
 *   #include "../js/js_native.h"
 *   Link: js_url.o  js_builtin.o  js_value.o  js_interp.o
 *         js_parse.o  js_lex.o  js_native.o
 *
 * Usage (call after every js_new()):
 *   js_vm *vm = js_new();
 *   js_url_install(vm);
 */

#ifndef JS_URL_H
#define JS_URL_H

#include "js.h"
#include "js_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * js_url_install -- register URL, URLSearchParams, encodeURIComponent,
 * decodeURIComponent, encodeURI, decodeURI in the VM's global scope.
 *
 * Must be called after every js_new() / arena reset.
 */
void js_url_install(js_vm *vm);

/*
 * js_url_selftest -- run the C-side parser + encoder self-test battery.
 *
 * Tests exercised (no live engine needed):
 *   - encodeURIComponent("a b/c")   == "a%20b%2Fc"
 *   - encodeURIComponent("hello")   == "hello"
 *   - decodeURIComponent("a%20b")   == "a b"
 *   - encodeURI("https://example.com/a b") == "https://example.com/a%20b"
 *   - encodeURI("https://x.com/?q=a&b=1") == "https://x.com/?q=a&b=1"
 *   - URL parser: absolute URL components
 *   - URL parser: relative "/p?x=1" resolved against "https://h:8/dir/"
 *     -> href == "https://h:8/p?x=1"
 *   - URL parser: relative "sub/page" resolved against "https://h/a/b"
 *     -> href == "https://h/a/sub/page"
 *   - URLSearchParams: get/set/has/delete/append/toString
 *
 * Returns 0 on pass, number of failures on fail.
 */
int js_url_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* JS_URL_H */

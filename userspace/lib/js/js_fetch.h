/*
 * js_fetch.h -- JS fetch() and XMLHttpRequest for AutomationOS.
 * =============================================================
 *
 * Exposes two browser-compatible Web APIs as globals in the JS engine:
 *
 *   fetch(url)         -- Returns a sync-resolved Response-like object.
 *   XMLHttpRequest     -- Constructor; .open()/.send() populate .status /
 *                         .responseText synchronously (like the legacy
 *                         synchronous XHR mode).
 *
 * SYNCHRONOUS MODEL (important):
 *   Both APIs block the caller thread until the HTTP response is fully
 *   received.  They DO NOT implement real Promise/microtask scheduling.
 *   fetch() returns an object whose .then(cb) calls `cb` immediately
 *   (synchronously) with the already-resolved Response; this lets
 *   promise-style userspace code work when the engine is sync.
 *   There is NO abort(), NO streaming, NO real Promise chain, NO
 *   concurrent requests.  This mirrors the deprecated
 *   XMLHttpRequest(false) synchronous XHR behaviour.
 *
 * URL syntax accepted:
 *   http://HOST[:PORT]/PATH
 *   https://HOST[:PORT]/PATH
 *   Default ports: 80 (http) / 443 (https).
 *   HOST may be a dotted-quad or a hostname (resolved via dns_resolve()).
 *
 * Build flags (same as rest of userspace -- NO fs:0x28 canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *   -fno-stack-protector -fno-pic -fno-pie
 *   -mno-red-zone -mstackrealign -O2
 *
 * Dependencies (link together):
 *   js_fetch.o  js_builtin.o  js_value.o  js_interp.o  js_parse.o
 *   js_lex.o    js_native.o   http.o      https.o       dns.o
 *   tlsconn.o   deflate.o
 *
 * Usage:
 *   js_vm *vm = js_new();
 *   js_fetch_install(vm);     // call after EACH js_new() / arena reset
 *   // Now JS code can call fetch() or new XMLHttpRequest().
 *
 * Limitations:
 *   - Synchronous only (no async/await without a real event loop).
 *   - GET requests only (POST/PUT/etc. not wired to http_get).
 *   - No request headers/body beyond what http_get sends by default.
 *   - No streaming / ReadableStream.
 *   - No AbortController.
 *   - .json() in the Response performs a full JSON.parse in C (sync).
 *   - Response body is capped at JS_FETCH_BODY_CAP bytes (1 MiB).
 *   - XMLHttpRequest: only open/send/status/responseText/onload wired.
 *   - No cookies, no credentials, no CORS enforcement.
 */

#ifndef JS_FETCH_H
#define JS_FETCH_H

#include "js.h"     /* js_vm */

/* Maximum response body size in bytes (1 MiB static BSS buffer). */
#define JS_FETCH_BODY_CAP  (1u << 20)   /* 1 048 576 bytes */

/*
 * js_fetch_install -- register fetch() and XMLHttpRequest in the VM's
 * global scope.
 *
 * Must be called after every js_new() because js_new() resets the global
 * environment and the native-class registry.  Idempotent per js_new()
 * call (registering twice on the same VM is a no-op beyond the second
 * class-id allocation, which stays bounded).
 */
void js_fetch_install(js_vm *vm);

/*
 * js_fetch_selftest -- offline self-test; no live network required.
 *
 * Exercises:
 *   1. URL parser: valid http/https URLs, missing scheme, bare host.
 *   2. fetch_install plumbing: installs globals without crashing.
 *   3. Stub response builder: constructs a Response object with known
 *      .status / .ok / .text() / .then() from synthetic data.
 *   4. XMLHttpRequest class registration: open/send/status properties.
 *
 * Returns 0 on pass, -1 on any failure.
 */
int js_fetch_selftest(void);

#endif /* JS_FETCH_H */

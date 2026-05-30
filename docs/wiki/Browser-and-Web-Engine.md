# Browser & Web Engine

`browser2` is AutomationOS's from-scratch web browser, and the flagship of the
userspace. It is not a port of anything: the DOM, the HTML parser, the CSS
engine, the layout engine, the JavaScript interpreter, and the web APIs are all
original code, each a separate freestanding library (no libc, no stdio — ring 3
like everything else), wired together into a real rendering pipeline that fetches
pages over the OS's own HTTP/HTTPS stack and paints them into a compositor
window.

This page documents the engine internals. For the network and TLS plumbing the
browser sits on, see [Networking & Security](Networking-and-Security.md); for
the compositor window it draws into and the rest of the app suite, see
[Desktop & Apps](Desktop-and-Apps.md).

---

## The app: `browser2` (`userspace/apps/browser2/`)

The browser is `userspace/apps/browser2/browser2.c` plus its chrome/animation
helpers (`browser2_ui.c`, `browser2_anim.c`). It links the whole web pipeline as
libraries and runs a bounded ~60 fps render loop: poll input, run JS timers,
animate inertial scroll, repaint the chrome + content. The pipeline per page is:

```
HTTP(S) fetch  --(http_get / https_get)-->  bytes
bytes          --(html_parse)----------->  DOM tree (dom_document / dom_node)
DOM + CSS      --(css_parse + cascade)-->  computed styles
styled DOM     --(layout_compute)------->  layout_box tree (block/inline/text)
<script> tags  --(js_eval_keep_env)----->  ES5 execution with DOM bindings
layout boxes   --(bitfont blit)--------->  pixels in a 800x600 compositor surface
```

It opens an 800×600 window through the `wl` compositor protocol (falling back to
`SYS_FB_ACQUIRE` direct framebuffer if needed) and blits text with the 8×16
bitmap font. Internal bounds keep it safe: `MAX_DOM_NODES 4096`,
`MAX_LAYOUT_BOXES 2048`, `MAX_SCRIPT_BYTES 65536`. On the non-interactive boot
path it prints `BROWSER2: ui ready (apis=5)` and
`BROWSER2: rendered <N> boxes for <URL>` after the first paint, then exits.

### `about:home` — the default page

The browser defaults to a built-in **`about:home`** page: a stable, Google-style
search page that is served from an in-program string with **no network access**,
so it renders deterministically and instantly. `about:` URLs are always
network-free. Typing into the address bar routes intelligently (handled in
`browser2.c`): an explicit `http://`/`https://`/`about:` URL passes through
verbatim; a bare domain (anything that looks like a host with a `.`) is prefixed
with `https://`; and free text becomes a Google search
(`https://www.google.com/search?q=...`). If a live fetch fails, `load_page()`
falls back to `about:home` rather than hanging.

---

## The DOM (`userspace/lib/dom/`)

The data model is a conventional DOM tree. `dom_node` carries a type
(`DOM_NODE_DOCUMENT` / `ELEMENT` / `TEXT` / `COMMENT`), the five tree links
(parent, first/last child, prev/next sibling), and — for elements — a lowercased
`tag` and an insertion-ordered singly-linked `dom_attr` list. A `void *user`
slot lets the layout engine hang a box off each node.

Unlike the JS engine (which owns a bump arena), DOM nodes and their strings are
`malloc`'d from `userspace/libc` because they must **outlive any single
`js_eval()`** — the DOM persists across all of a page's scripts. The library is
split into focused units: `dom.c` (core tree), `dom_selector.c` (selector
matching), `dom_event.c` (events), `dom_serialize.c` (serialization),
`dom_util.c`, and `dom_bindings.c` (the JS bridge — see below). A
`DOMTEST: PASS` self-test runs at boot.

---

## The HTML parser (`userspace/lib/html/html_parse.c`)

An HTML5-*subset* tokenizer + tree-builder that emits a real DOM tree. It is not
a full HTML5 implementation but is "real enough that a hand-written page or
typical server-rendered HTML produces a sensible tree." Verified from the
header:

- **Known elements** are lowercased and recognized: structural
  (`html`/`head`/`body`/`title`/`meta`/`link`/`script`/`style`), flow and text
  (`div`/`span`/`p`/`a`/`h1`–`h6`/`b`/`i`/`em`/`strong`/`code`/`pre`), lists
  (`ul`/`ol`/`li`), tables (`table`/`thead`/`tbody`/`tr`/`th`/`td`), HTML5
  sectioning (`header`/`footer`/`nav`/`section`/`article`/`main`/`aside`), and
  forms (`form`/`input`/`button`/`label`/`select`/`option`/`textarea`). Unknown
  tags become generic inline elements.
- **Void elements** (`br`/`hr`/`img`/`input`/`meta`/`link`) auto-close;
  **raw-text** elements (`script`/`style`) consume their body as one text node.
- **Entities**: `&amp; &lt; &gt; &quot; &apos; &nbsp;`, numeric `&#NN;` /
  `&#xHH;`, and a small named table (`copy`, `reg`, `mdash`, …). Comments become
  `DOM_NODE_COMMENT` nodes; `<!DOCTYPE>` is skipped.
- **Insertion modes** are simplified to `INITIAL` / `IN_HEAD` / `IN_BODY` /
  `AFTER_BODY`.

The header is honest about what is dropped: no adoption-agency algorithm
(misnested `</b></i>` may close out of order), no `<table>` foster parenting, no
encoding sniffing (UTF-8/ASCII assumed), and no SVG/MathML namespace switching.
A `HTMLTEST: PASS` self-test runs at boot.

---

## The CSS engine (`userspace/lib/css/css.c`)

A CSS-subset parser + cascade. It parses a `<style>` block or linked stylesheet
into rules, then **computes** the style for any element by cascading UA defaults
→ matched author rules → the element's inline `style="..."`.

- **Selectors**: type, class, id, universal, descendant (`a b`), child
  (`a > b`), compound (`div.x`, `h1#title`), and selector lists. Specificity is
  the standard `(#id, .class, type)` tuple; inline style wins.
- **Properties**: `color`, `background-color`, `font-size` (px / unitless / pt),
  `font-weight`, `font-style`, `text-decoration`, `display`
  (block / inline / inline-block / none), `margin`/`padding` (shorthand + per
  side), `width`/`height` (px or auto), `text-align`, `line-height`, and
  `border-width` (+ per side). Inherited properties (color, font-size,
  font-weight, font-style, text-align, line-height) inherit from the parent's
  computed style.
- **Colors**: `#rgb`, `#rrggbb`, `rgb()`, `rgba()` (alpha honoured),
  `transparent`, and ~15 named colors.

Known gaps the header documents: no `@media`/`@import`/`@keyframes`/`@font-face`,
no pseudo-classes/elements (`:hover`, `::before`), no attribute or sibling
selectors, no relative units (em/rem/%/vh/vw — everything is px), and no
`font-family` (the renderer uses one bitmap font).

> Source caveat to reconcile: the `css.h` header is internally inconsistent about
> `!important` — the feature summary says "`!important` is honoured … beat normal
> ones regardless of specificity," while the KNOWN LIMITATIONS list says
> "No `!important`." The documented behaviour and the limitation contradict each
> other; treat `!important` support as unverified until the implementation is
> checked.

A `CSSTEST: PASS` self-test runs at boot.

---

## The layout engine (`userspace/lib/layout/layout.c`)

A block + inline **flow** layout engine. It walks the DOM, computes the CSS for
each element, and produces a tree of `layout_box` rectangles in absolute
viewport coordinates for the painter.

- **Block formatting**: block containers stack vertically, each taking the parent
  content width; height grows to enclose content.
- **Inline formatting**: inline boxes and anonymous `LB_TEXT` segments flow
  horizontally with greedy whitespace-driven word wrap; each wrapped line is an
  `LB_LINE` pseudo-box.
- **Box model**: explicit `width` honoured (else fill the slot minus margins);
  inline-block fits content; padding shrinks the inline content area; vertical
  margins between sibling blocks collapse to the max of the two.
- **Text metrics**: a fixed **8 px advance per char** (matching the 8×16 bitmap
  font); `display: none` skips the element and its subtree.

Honest limits: no float/clear, no positioning other than static (no
relative/absolute/fixed/sticky), no flex/grid, no transforms or z-index (paint
order = document order), no vertical-align (approximate baseline), no bidi/RTL,
and tables render as a stack of block rows (no column sizing). A
`LAYOUTTEST: PASS` self-test runs at boot.

---

## The JavaScript engine (`userspace/lib/js/`)

A from-scratch, freestanding **ES5-subset tree-walking interpreter**. It is
strictly self-contained: no libc, no malloc — all memory (AST nodes, value cells,
environments, scratch strings) comes from a single ~6 MB static **bump arena**
per VM. There is **no garbage collector**; within a single run nothing is freed,
which is correct for batch script execution, and the arena is reset to a
checkpoint after each top-level run so long-running shells reclaim everything
between statements. If the arena fills, allocation returns a clean out-of-memory
error rather than crashing.

The engine is split into `js_lex.c` (lexer), `js_parse.c` (parser),
`js_value.c` (value model + arena), `js_interp.c` (tree-walker), and
`js_builtin.c` (built-ins). The boot `js_selftest()` exercises arithmetic
precedence, loops, closures, strings, objects, arrays, JSON, `Math`, and
coercion — the smoke test reports `JS engine verified (ES5 interpreter:
closures/JSON/builtins)`.

### `js_eval` vs `js_eval_keep_env` (the env-reset gotcha)

There are two entry points, and the difference matters for the browser:

- `js_eval()` **resets** the arena, the global environment, the native-class
  registry, and the intern table at the *start* of every call (REPL semantics).
- `js_eval_keep_env()` does **not** reset any of that — so native globals an
  embedder registered (e.g. the DOM's `document`) survive, and DOM mutations
  accumulate across calls.

`browser2` relies on this precisely: it installs the DOM bindings + web APIs
**once**, after a single reset, then runs every `<script>` on the page with
`js_eval_keep_env()` so `document` and accumulated state persist across all of a
page's scripts — matching real multi-`<script>` semantics. Using plain `js_eval`
there would wipe `document` (turning it into a ReferenceError) and discard each
script's DOM changes. (This is documented in `browser2.c`'s lifecycle comment.)

---

## The web APIs (the "apis=5")

The DOM bindings plus five web-API modules are installed into the VM after the
reset; `browser2` prints `apis=5` to confirm all five wired up. Each is a
separate library with its own boot self-test, and each must be (re)installed
after every `js_new()`.

| API | Module | Surface (verified from source) |
|---|---|---|
| DOM bindings | `dom/dom_bindings.c` | `window` / `document` / element access visible to JS; event handler side-table |
| `console` | `js/js_console.c` | `console.log` / `console.error` → the engine's print sink (wired to `SYS_WRITE`) |
| Timers | `js/js_timers.c` | `setTimeout` / timer queue, pumped by the browser's render loop (`js_timers_run`) |
| `fetch` / XHR | `js/js_fetch.c` | `fetch(url)` and `XMLHttpRequest` — **synchronous** (block until response; `.then(cb)` calls `cb` immediately). GET only, body capped at 1 MiB, no Promises/streaming/CORS |
| Storage | `js/js_storage.c` | `localStorage` (persisted to `/home/.localstorage` across reboots) and `sessionStorage` (in-memory) |
| URL | `js/js_url.c` | `URL` / `URLSearchParams`, `encodeURIComponent` / `decodeURIComponent` / `encodeURI` / `decodeURI` (RFC 3986 resolution) |

The `fetch`/XHR module is what ties JavaScript to the network stack: it resolves
the host via `dns_resolve()` and calls the same `http_get` / `https_get`
(`https://` → TLS) used elsewhere, so a page's scripts can hit `https://` URLs
through the OS's own TLS 1.2 client. (See
[Networking & Security](Networking-and-Security.md) for the crypto/TLS details
and the encrypted-but-unauthenticated trust caveat.)

---

## Per-layer self-tests

Every web layer ships a boot-time self-test that prints `PASS`/`FAIL`, run from
`init` as standalone apps so a regression is caught deterministically without a
display or a network:

| Marker | App | Covers |
|---|---|---|
| `DOMTEST: PASS` | `apps/domtest` | DOM tree construction, selectors, events |
| `HTMLTEST: PASS` | `apps/htmltest` | the HTML tokenizer + tree-builder |
| `CSSTEST: PASS` | `apps/csstest` | the CSS parser + cascade |
| `LAYOUTTEST: PASS` | `apps/layouttest` | the flow layout engine |
| `WEBTEST: PASS` | `apps/webtest` | the JS engine + DOM-bindings bridge |
| `WEBAPITEST: PASS` | `apps/webapitest` | the JS web-API surface (timers/fetch/storage/console/url) |

The boot smoke test gates the whole wave on these plus `BROWSER2: ui ready
(apis=5)` and a `BROWSER2: rendered <N>` line, so a green boot means the entire
DOM → HTML → CSS → layout → JS → render pipeline executed end-to-end.

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Kernel Internals](Kernel-Internals.md)
- [Networking & Security](Networking-and-Security.md)
- [Desktop & Apps](Desktop-and-Apps.md)
- [Drivers & I/O](Drivers-and-IO.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)

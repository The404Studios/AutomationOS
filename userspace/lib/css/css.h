/* userspace/lib/css/css.h
 * ============================================================================
 * CSS-subset parser + cascade for AutomationOS ring-3 browser pipeline.
 *
 * Takes a CSS source block (typically from <style> or a linked stylesheet),
 * parses it into a list of rules, then "computes" the style for any DOM
 * element by cascading (1) built-in UA defaults, (2) matched author rules,
 * (3) the element's inline style="..." attribute.
 *
 * FREESTANDING: malloc/free + string.h only. No stdio, no libc beyond that.
 *
 * ----------------------------------------------------------------------------
 * Supported selectors (parser tolerates more, ignores rest):
 *   type                e.g.  div
 *   class               e.g.  .foo
 *   id                  e.g.  #bar
 *   universal           e.g.  *
 *   descendant          e.g.  a b
 *   child               e.g.  a > b
 *   compound            e.g.  div.x, h1#title
 *   selector list       e.g.  h1, h2, .lead   (comma-separated)
 *
 *   Specificity model (a, b, c) = (#id count, #class count, #type count).
 *   Inline style wins over any matched author rule.
 *
 * Supported properties:
 *   color
 *   background-color   (also accepts "background")
 *   font-size          (px or unitless = px; pt converted at 1pt ~= 1.333px)
 *   font-weight        (bold | normal | numeric: >=600 -> bold)
 *   font-style         (italic | normal)
 *   text-decoration    (underline | none)
 *   display            (block | inline | inline-block | none)
 *   margin             (1, 2, 3 or 4 px values - top right bottom left)
 *   margin-top / margin-right / margin-bottom / margin-left
 *   padding            (same shorthand as margin)
 *   padding-top / padding-right / padding-bottom / padding-left
 *   width              (px or "auto")
 *   height             (px or "auto")
 *   text-align         (left | center | right)
 *   line-height        (px, %, or unitless multiplier stored as px)
 *   border-width       (px; basic; also border shorthand extracts width)
 *   border-top-width / border-right-width / border-bottom-width / border-left-width
 *
 * !important is honoured: !important declarations beat normal ones regardless
 * of specificity (important normal-order cascade on top).
 *
 * Inherited properties (color, font-size, font-weight, font-style, text-align,
 * line-height) are inherited from the parent element's computed style when not
 * explicitly set on the element.
 *
 * Color values:
 *   #rgb            -> #rrggbb expansion
 *   #rrggbb
 *   rgb(r,g,b)      0..255 each
 *   rgba(r,g,b,a)   alpha 0.0-1.0 (a is honoured)
 *   transparent
 *   ~15 named colors: black white red green blue yellow cyan magenta
 *                     gray silver maroon olive navy purple teal lime orange
 *
 * Property values that don't match the recognised forms are silently dropped.
 *
 * KNOWN LIMITATIONS:
 *   - No @media, @import, @keyframes, @font-face, @supports (parsed and
 *     skipped at top level; nested rules inside are not extracted).
 *   - No pseudo-classes / pseudo-elements (:hover, ::before...) -- the
 *     selector tokenizer skips them. Their rules are still parsed but
 *     never match.
 *   - No attribute selectors ([href]).
 *   - No adjacent-sibling (+) or general-sibling (~) combinators.
 *   - No inheritance of unset properties (each property has a hard default).
 *     This is a known divergence from real CSS.
 *   - No relative units (em, rem, %, vh, vw); everything is px.
 *   - No font-family; the renderer uses a single bitmap font.
 *   - No box-shadow, no border (border-* properties are dropped; they would
 *     be added in a future pass).
 *   - No !important.
 *
 * Built-in UA defaults applied before any author rule:
 *   body                 -> margin: 8
 *   h1                   -> font-size: 32, bold, margin-y: 16
 *   h2                   -> font-size: 24, bold, margin-y: 14
 *   h3                   -> font-size: 19, bold, margin-y: 12
 *   h4                   -> font-size: 16, bold, margin-y: 10
 *   h5                   -> font-size: 14, bold, margin-y: 8
 *   h6                   -> font-size: 13, bold, margin-y: 6
 *   p                    -> margin-y: 12
 *   ul, ol               -> margin-y: 12, padding-left: 24
 *   li                   -> display: block (simplified; no markers)
 *   a                    -> color: blue (#0000ee), underline
 *   b, strong            -> bold
 *   i, em                -> italic
 *   code, pre            -> (no font-family change here; bold=0, italic=0)
 *   pre                  -> margin-y: 12
 *   hr                   -> display: block, margin-y: 8
 *   br                   -> display: inline
 *   div, section, header, footer, nav, main, article, aside, form
 *                        -> display: block
 *   img                  -> display: inline
 *   span, a, b, i, em, strong, code, label, button, input, select, textarea
 *                        -> display: inline
 *   table                -> display: block (simplified)
 *   tr                   -> display: block
 *   td, th               -> display: inline-block, padding: 4
 *   head, title, script, style, meta, link
 *                        -> display: none
 * ============================================================================
 */

#ifndef USERSPACE_LIB_CSS_CSS_H
#define USERSPACE_LIB_CSS_CSS_H

#include "../dom/dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque stylesheet handle. */
typedef struct css_stylesheet css_stylesheet;

/* Display kind. */
enum css_display_kind {
    CSS_DISP_INLINE = 0,
    CSS_DISP_BLOCK,
    CSS_DISP_NONE,
    CSS_DISP_INLINE_BLOCK
};

/* Text alignment. */
enum css_text_align_kind {
    CSS_ALIGN_LEFT = 0,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_RIGHT
};

/* Computed style for a single element, as consumed by layout/paint. */
typedef struct {
    unsigned int color;          /* 0xAARRGGBB; default 0xFF000000 black     */
    unsigned int background;     /* 0xAARRGGBB; default 0 transparent        */
    int          font_size;      /* px; default 16                            */
    int          bold;           /* 0/1                                       */
    int          italic;         /* 0/1                                       */
    int          underline;      /* 0/1                                       */
    int          display;        /* enum css_display_kind                     */
    int          margin_t, margin_r, margin_b, margin_l;     /* px           */
    int          padding_t, padding_r, padding_b, padding_l; /* px           */
    int          width, height;  /* px; -1 = auto                             */
    int          text_align;     /* enum css_text_align_kind                  */
    /* --- APPENDED fields (safe to add; existing layout unchanged) --- */
    int          line_height;    /* px (or % value if line_height_pct=1); 0=normal */
    int          line_height_pct;/* 1 if line_height is a percentage, else 0  */
    int          border_t, border_r, border_b, border_l; /* border-width px  */
    int          font_size_pct;  /* 1 if font_size was set as a percentage    */
    int          width_pct;      /* 1 if width was set as a percentage        */
} css_computed;

/* Parse a CSS text block. `len` is honoured (input need not be NUL-terminated).
 * Returns a heap-owned stylesheet. Returns NULL on allocation failure; on
 * malformed input the parser is lenient -- the offending rule is skipped. */
css_stylesheet *css_parse(const char *css, unsigned long len);

/* Release the stylesheet and all rules / selectors / declarations it owns. */
void css_free(css_stylesheet *sheet);

/* Compute the effective style for one element. `sheet` may be NULL (UA
 * defaults + inline style only). `el` MUST be a DOM_NODE_ELEMENT. The
 * caller provides storage in `out`; it is fully overwritten. */
void css_compute(const css_stylesheet *sheet,
                 const struct dom_node *el,
                 css_computed       *out);

/* Built-in self-test. Returns 0 on pass, negative on first failure. */
int css_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* USERSPACE_LIB_CSS_CSS_H */

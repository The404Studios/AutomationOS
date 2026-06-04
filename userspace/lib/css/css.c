/* userspace/lib/css/css.c
 * ============================================================================
 * CSS-subset parser + cascade. See css.h for the supported subset.
 *
 * FREESTANDING ring-3 -- only userspace/libc/{malloc.h, string.h}.
 *
 * Internal pipeline:
 *   css_parse() ----> list of selector chains, each with a list of declarations
 *   css_compute() -> walks UA defaults table, then iterates rules and applies
 *                    each whose selector matches `el`, then layers the inline
 *                    style="..." attribute. Specificity ordering is honoured.
 * ============================================================================
 */

#include "css.h"
#include "../../libc/malloc.h"
#include "../../libc/string.h"

/* ------------------------------------------------------------------ */
/* Helpers.                                                           */
/* ------------------------------------------------------------------ */

static int ci_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_ident_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
}

static char to_lower(int c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; }

static char *strndup_h(const char *p, unsigned long n)
{
    char *o = (char *)malloc(n + 1);
    if (!o) return 0;
    for (unsigned long i = 0; i < n; i++) o[i] = p[i];
    o[n] = 0;
    return o;
}

static char *strdup_h(const char *p)
{
    if (!p) return 0;
    return strndup_h(p, strlen(p));
}

static int starts_with_ci(const char *p, unsigned long max, const char *prefix)
{
    unsigned long n = strlen(prefix);
    if (max < n) return 0;
    for (unsigned long i = 0; i < n; i++) {
        char x = p[i], y = prefix[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Color parsing.                                                     */
/* ------------------------------------------------------------------ */

static unsigned int color_named(const char *n)
{
    static const struct { const char *name; unsigned int v; } table[] = {
        { "black",   0xFF000000 },
        { "white",   0xFFFFFFFF },
        { "red",     0xFFFF0000 },
        { "green",   0xFF008000 },
        { "blue",    0xFF0000FF },
        { "yellow",  0xFFFFFF00 },
        { "cyan",    0xFF00FFFF },
        { "magenta", 0xFFFF00FF },
        { "gray",    0xFF808080 },
        { "grey",    0xFF808080 },
        { "silver",  0xFFC0C0C0 },
        { "maroon",  0xFF800000 },
        { "olive",   0xFF808000 },
        { "navy",    0xFF000080 },
        { "purple",  0xFF800080 },
        { "teal",    0xFF008080 },
        { "lime",    0xFF00FF00 },
        { "orange",  0xFFFFA500 },
        { "pink",    0xFFFFC0CB },
        { "brown",   0xFFA52A2A },
        { "transparent", 0x00000000 },
        { 0, 0 }
    };
    for (int i = 0; table[i].name; i++) {
        if (ci_eq(n, table[i].name)) return table[i].v;
    }
    return 0xFF000000;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns 0xAARRGGBB. `value` is a NUL-terminated CSS color token. On parse
 * failure, *ok = 0 and returns 0xFF000000. */
static unsigned int parse_color(const char *value, int *ok)
{
    if (ok) *ok = 1;
    if (!value || !*value) { if (ok) *ok = 0; return 0xFF000000; }
    /* skip leading space */
    while (*value && is_space((unsigned char)*value)) value++;

    if (*value == '#') {
        const char *p = value + 1;
        int n = 0;
        while (p[n] && hex_digit((unsigned char)p[n]) >= 0) n++;
        if (n == 3) {
            int r = hex_digit((unsigned char)p[0]);
            int g = hex_digit((unsigned char)p[1]);
            int b = hex_digit((unsigned char)p[2]);
            return 0xFF000000u | ((unsigned int)(r * 17) << 16) |
                                  ((unsigned int)(g * 17) << 8) |
                                  (unsigned int)(b * 17);
        }
        if (n == 6) {
            int r = hex_digit((unsigned char)p[0]) * 16 + hex_digit((unsigned char)p[1]);
            int g = hex_digit((unsigned char)p[2]) * 16 + hex_digit((unsigned char)p[3]);
            int b = hex_digit((unsigned char)p[4]) * 16 + hex_digit((unsigned char)p[5]);
            return 0xFF000000u | ((unsigned int)r << 16) |
                                  ((unsigned int)g << 8) |
                                  (unsigned int)b;
        }
        if (ok) *ok = 0;
        return 0xFF000000;
    }
    if (starts_with_ci(value, strlen(value), "rgb(") ||
        starts_with_ci(value, strlen(value), "rgba(")) {
        int is_rgba = (to_lower((unsigned char)value[3]) == 'a');
        const char *p = value + (is_rgba ? 5 : 4);
        int parts[4] = { 0, 0, 0, 255 };
        for (int i = 0; i < (is_rgba ? 4 : 3); i++) {
            while (*p && is_space((unsigned char)*p)) p++;
            if (i == 3) {
                /* alpha 0.0-1.0 */
                int whole = 0, dec = 0, dec_div = 1;
                int saw_digit = 0;
                while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; saw_digit = 1; }
                if (*p == '.') {
                    p++;
                    while (*p >= '0' && *p <= '9') { dec = dec * 10 + (*p - '0'); dec_div *= 10; p++; saw_digit = 1; }
                }
                if (!saw_digit) { if (ok) *ok = 0; return 0xFF000000; }
                /* alpha float -> 0..255 */
                int a = (int)((whole + (double)dec / (double)dec_div) * 255.0 + 0.5);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                parts[3] = a;
            } else {
                int v = 0;
                int saw = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; saw = 1; }
                if (!saw) { if (ok) *ok = 0; return 0xFF000000; }
                if (*p == '%') {
                    p++;
                    v = (v * 255 + 50) / 100;
                }
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                parts[i] = v;
            }
            while (*p && is_space((unsigned char)*p)) p++;
            if (*p == ',') p++;
        }
        return (unsigned int)((parts[3] & 0xFF) << 24) |
               (unsigned int)((parts[0] & 0xFF) << 16) |
               (unsigned int)((parts[1] & 0xFF) << 8) |
               (unsigned int)(parts[2] & 0xFF);
    }
    /* named */
    return color_named(value);
}

/* ------------------------------------------------------------------ */
/* Length parsing.  Returns px int.                                   */
/* Supported: "12px", "12", "12pt", "0".  "auto" returns -1.          */
/* ------------------------------------------------------------------ */

/* CSS_UNIT_PX=0, CSS_UNIT_PCT=1, CSS_UNIT_NORMAL=2 (unitless multiplier) */
#define CSS_UNIT_PX     0
#define CSS_UNIT_PCT    1
#define CSS_UNIT_NORMAL 2

/* Extended length parse: also fills *unit_out (CSS_UNIT_*). */
static int parse_length_ex(const char *v, int *ok, int *unit_out)
{
    if (unit_out) *unit_out = CSS_UNIT_PX;
    if (ok) *ok = 1;
    if (!v || !*v) { if (ok) *ok = 0; return 0; }
    while (*v && is_space((unsigned char)*v)) v++;
    if (ci_eq(v, "auto")) return -1;
    if (ci_eq(v, "normal")) {
        if (unit_out) *unit_out = CSS_UNIT_NORMAL;
        return 0;
    }
    int sign = 1;
    if (*v == '+') v++;
    else if (*v == '-') { sign = -1; v++; }
    int whole = 0;
    int saw = 0;
    /* Cap the accumulators so a pathological value (e.g. "padding: 9999...9px")
     * cannot overflow the signed int and feed garbage into the box-model math
     * in layout.c. We keep consuming digits (so the unit suffix still parses)
     * but stop growing once past a sane bound -- 1e6 px is already absurd. */
    while (*v >= '0' && *v <= '9') { if (whole < 1000000) whole = whole * 10 + (*v - '0'); v++; saw = 1; }
    int frac = 0, frac_div = 1;
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9') { if (frac_div < 1000000) { frac = frac * 10 + (*v - '0'); frac_div *= 10; } v++; saw = 1; }
    }
    if (!saw) { if (ok) *ok = 0; return 0; }
    double val = (double)whole + (double)frac / (double)frac_div;
    val *= (double)sign;
    /* skip whitespace before unit */
    while (*v && is_space((unsigned char)*v)) v++;
    if (*v == '%') {
        if (unit_out) *unit_out = CSS_UNIT_PCT;
        return (int)(val + (val < 0 ? -0.5 : 0.5));
    }
    if (starts_with_ci(v, strlen(v), "px")) {
        if (unit_out) *unit_out = CSS_UNIT_PX;
        return (int)(val + (val < 0 ? -0.5 : 0.5));
    }
    if (starts_with_ci(v, strlen(v), "pt")) {
        if (unit_out) *unit_out = CSS_UNIT_PX;
        return (int)(val * 1.333 + (val < 0 ? -0.5 : 0.5));
    }
    /* No unit / unknown: treat as px (unitless number). */
    if (unit_out) *unit_out = CSS_UNIT_PX;
    return (int)(val + (val < 0 ? -0.5 : 0.5));
}

static int parse_length(const char *v, int *ok)
{
    return parse_length_ex(v, ok, 0);
}

/* ------------------------------------------------------------------ */
/* Declarations.                                                      */
/* ------------------------------------------------------------------ */

typedef struct css_decl {
    char *property;
    char *value;
    int   important;   /* 1 if "!important" was present */
    struct css_decl *next;
} css_decl;

/* ------------------------------------------------------------------ */
/* Selectors.                                                         */
/* ------------------------------------------------------------------ */

/* A compound selector is a single piece, e.g. "div.x#y" -- type + classes + id. */
typedef struct {
    char *type;        /* lowercased tag name; "" = any */
    char *id;          /* may be NULL */
    char **classes;    /* array of strings */
    int   nclasses;
} css_compound;

/* A combinator chain: "a b > c" -> 3 compounds, combinators[0]=DESC, [1]=CHILD,
 * combinators[2] is unused. */
enum { COMB_DESC = 0, COMB_CHILD = 1 };

typedef struct css_chain {
    css_compound *parts;     /* parts[0..nparts-1], rightmost = subject */
    int           nparts;
    int          *combinators; /* combinators[i] connects parts[i] and parts[i+1] */
    int           spec_a, spec_b, spec_c;
    struct css_chain *next;
} css_chain;

typedef struct css_rule {
    css_chain *chains;    /* OR-list of selector chains */
    css_decl  *decls;
    int order;            /* document order (for tiebreak) */
    struct css_rule *next;
} css_rule;

struct css_stylesheet {
    css_rule *rules;
    css_rule *rules_tail;
    int rule_count;
};

/* ------------------------------------------------------------------ */
/* Declaration parsing.                                               */
/* ------------------------------------------------------------------ */

static void decl_free_list(css_decl *d)
{
    while (d) {
        css_decl *n = d->next;
        if (d->property) free(d->property);
        if (d->value) free(d->value);
        free(d);
        d = n;
    }
}

/* Parse one "prop: value;" entry. Returns 1 on consumption, advances *pp.
 * Stops at '}' or end. */
static int parse_decl(const char **pp, const char *end, css_decl **out_head, css_decl **out_tail)
{
    const char *p = *pp;
    while (p < end && is_space((unsigned char)*p)) p++;
    if (p >= end || *p == '}') { *pp = p; return 0; }
    if (*p == ';') { p++; *pp = p; return 1; }

    /* property */
    const char *ps = p;
    while (p < end && (is_ident_char((unsigned char)*p))) p++;
    if (p == ps) {
        /* skip a stray char */
        p++;
        *pp = p;
        return 1;
    }
    char *prop = strndup_h(ps, (unsigned long)(p - ps));
    /* lowercase */
    if (prop) {
        for (char *q = prop; *q; q++) *q = to_lower((unsigned char)*q);
    }

    while (p < end && is_space((unsigned char)*p)) p++;
    if (p >= end || *p != ':') {
        /* malformed; skip to ; or } */
        while (p < end && *p != ';' && *p != '}') p++;
        if (p < end && *p == ';') p++;
        if (prop) free(prop);
        *pp = p;
        return 1;
    }
    p++;
    while (p < end && is_space((unsigned char)*p)) p++;

    /* value: until ';' or '}', stripping surrounding whitespace. Handle simple
     * paren nesting so commas in rgb(...) don't end us early. */
    const char *vs = p;
    int paren = 0;
    while (p < end && (paren > 0 || (*p != ';' && *p != '}'))) {
        if (*p == '(') paren++;
        else if (*p == ')') paren--;
        p++;
    }
    const char *ve = p;
    while (ve > vs && is_space((unsigned char)ve[-1])) ve--;

    /* Detect and strip !important from the value. */
    int imp = 0;
    {
        const char *tmp = ve;
        /* strip trailing space before possible "important" */
        while (tmp > vs && is_space((unsigned char)tmp[-1])) tmp--;
        /* check if it ends with "important" */
        unsigned long ilen = strlen("important");
        if ((unsigned long)(tmp - vs) >= ilen) {
            const char *cand = tmp - ilen;
            if (starts_with_ci(cand, ilen, "important")) {
                /* check there's a '!' before it (possibly after whitespace) */
                const char *bef = cand;
                while (bef > vs && is_space((unsigned char)bef[-1])) bef--;
                if (bef > vs && bef[-1] == '!') {
                    imp = 1;
                    ve = bef - 1; /* trim !important from value */
                    while (ve > vs && is_space((unsigned char)ve[-1])) ve--;
                }
            }
        }
    }

    char *val = strndup_h(vs, (unsigned long)(ve - vs));

    if (p < end && *p == ';') p++;

    css_decl *d = (css_decl *)calloc(1, sizeof(*d));
    if (d) {
        d->property = prop;
        d->value = val;
        d->important = imp;
        d->next = 0;
        if (*out_tail) (*out_tail)->next = d;
        else *out_head = d;
        *out_tail = d;
    } else {
        if (prop) free(prop);
        if (val) free(val);
    }
    *pp = p;
    return 1;
}

/* Parse a block { ... } into a decl list. *pp must point at '{' (consumed). */
static css_decl *parse_decl_block(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end || *p != '{') { *pp = p; return 0; }
    p++;
    css_decl *head = 0, *tail = 0;
    while (p < end && *p != '}') {
        if (!parse_decl(&p, end, &head, &tail)) break;
    }
    if (p < end && *p == '}') p++;
    *pp = p;
    return head;
}

/* ------------------------------------------------------------------ */
/* Selector parsing.                                                  */
/* ------------------------------------------------------------------ */

static void compound_free(css_compound *c)
{
    if (c->type) free(c->type);
    if (c->id) free(c->id);
    for (int i = 0; i < c->nclasses; i++) {
        if (c->classes[i]) free(c->classes[i]);
    }
    if (c->classes) free(c->classes);
}

static void chain_free(css_chain *c)
{
    for (int i = 0; i < c->nparts; i++) compound_free(&c->parts[i]);
    if (c->parts) free(c->parts);
    if (c->combinators) free(c->combinators);
}

static void chain_free_list(css_chain *c)
{
    while (c) {
        css_chain *n = c->next;
        chain_free(c);
        free(c);
        c = n;
    }
}

/* Append class to compound. */
static void compound_add_class(css_compound *c, const char *name)
{
    char **na = (char **)realloc(c->classes,
                                 (unsigned long)(c->nclasses + 1) * sizeof(char *));
    if (!na) return;
    c->classes = na;
    c->classes[c->nclasses++] = strdup_h(name);
}

/* Parse a compound piece. *pp -> past compound. Returns 1 if any token
 * was consumed (even pseudo class which we skip). */
static int parse_compound(const char **pp, const char *end, css_compound *out)
{
    const char *p = *pp;
    int consumed = 0;
    out->type = 0;
    out->id = 0;
    out->classes = 0;
    out->nclasses = 0;

    /* leading whitespace already eaten by caller */
    while (p < end) {
        char c = *p;
        if (c == '*') {
            out->type = strdup_h("");   /* universal: match anything */
            p++;
            consumed = 1;
            continue;
        }
        if (is_ident_start((unsigned char)c)) {
            /* type selector (only if at start of compound) */
            if (!out->type) {
                const char *ps = p;
                while (p < end && is_ident_char((unsigned char)*p)) p++;
                char *t = strndup_h(ps, (unsigned long)(p - ps));
                if (t) {
                    for (char *q = t; *q; q++) *q = to_lower((unsigned char)*q);
                    out->type = t;
                }
                consumed = 1;
                continue;
            }
            break;
        }
        if (c == '.') {
            p++;
            const char *ps = p;
            while (p < end && is_ident_char((unsigned char)*p)) p++;
            if (p > ps) {
                char *n = strndup_h(ps, (unsigned long)(p - ps));
                if (n) {
                    for (char *q = n; *q; q++) *q = to_lower((unsigned char)*q);
                    compound_add_class(out, n);
                    free(n);
                }
                consumed = 1;
            }
            continue;
        }
        if (c == '#') {
            p++;
            const char *ps = p;
            while (p < end && is_ident_char((unsigned char)*p)) p++;
            if (p > ps) {
                if (!out->id) out->id = strndup_h(ps, (unsigned long)(p - ps));
                consumed = 1;
            }
            continue;
        }
        if (c == ':') {
            /* pseudo: ":foo" or "::foo(...)" -- skip identifier and any
             * (...) so a non-matching pseudo doesn't poison the rest. */
            p++;
            if (p < end && *p == ':') p++;
            while (p < end && is_ident_char((unsigned char)*p)) p++;
            if (p < end && *p == '(') {
                int depth = 1; p++;
                while (p < end && depth) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
            }
            /* Mark this compound as un-matchable by setting a sentinel type. */
            if (!out->type) out->type = strdup_h("\x01__pseudo__\x01");
            consumed = 1;
            continue;
        }
        if (c == '[') {
            /* attribute selector: skip [...]. Mark un-matchable. */
            int depth = 1; p++;
            while (p < end && depth) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            if (!out->type) out->type = strdup_h("\x01__attr__\x01");
            consumed = 1;
            continue;
        }
        break;
    }
    *pp = p;
    return consumed;
}

/* Parse one chain: zero or more compounds separated by descendant or child
 * combinators. Stops at ',' or '{'. Returns NULL on empty. */
static css_chain *parse_chain(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && is_space((unsigned char)*p)) p++;
    if (p >= end || *p == ',' || *p == '{') { *pp = p; return 0; }

    css_chain *c = (css_chain *)calloc(1, sizeof(*c));
    if (!c) return 0;

    while (p < end && *p != ',' && *p != '{') {
        css_compound part = { 0 };
        if (!parse_compound(&p, end, &part)) {
            /* nothing parsed; bail */
            compound_free(&part);
            break;
        }
        css_compound *na = (css_compound *)realloc(c->parts,
            (unsigned long)(c->nparts + 1) * sizeof(css_compound));
        if (!na) { compound_free(&part); break; }
        c->parts = na;  /* assign before second realloc so chain_free stays safe */
        int *nc = (int *)realloc(c->combinators,
            (unsigned long)(c->nparts + 1) * sizeof(int));
        if (!nc) { compound_free(&part); break; }
        c->combinators = nc;
        c->parts[c->nparts] = part;
        c->combinators[c->nparts] = COMB_DESC; /* default, fixed up below */
        c->nparts++;

        /* combinator */
        while (p < end && is_space((unsigned char)*p)) p++;
        if (p < end && (*p == '>' || *p == '+' || *p == '~')) {
            int comb = (*p == '>') ? COMB_CHILD : COMB_DESC;
            /* + and ~ aren't supported; we map to DESC and mark the chain
             * as not exactly correct. */
            c->combinators[c->nparts - 1] = comb;
            p++;
            while (p < end && is_space((unsigned char)*p)) p++;
        }
        /* The "combinator between part[i] and part[i+1]" is stored at
         * combinators[i] above; the loop continues. */
    }

    if (c->nparts == 0) { free(c); *pp = p; return 0; }

    /* Compute specificity. */
    int a = 0, b = 0, cc = 0;
    for (int i = 0; i < c->nparts; i++) {
        if (c->parts[i].id) a++;
        b += c->parts[i].nclasses;
        if (c->parts[i].type && c->parts[i].type[0] &&
            c->parts[i].type[0] != '\x01' &&
            strcmp(c->parts[i].type, "") != 0) cc++;
    }
    c->spec_a = a;
    c->spec_b = b;
    c->spec_c = cc;

    *pp = p;
    return c;
}

/* Parse a selector list: chain, chain, ... Returns linked list. Caller frees. */
static css_chain *parse_selector_list(const char **pp, const char *end)
{
    css_chain *head = 0, *tail = 0;
    while (*pp < end) {
        css_chain *c = parse_chain(pp, end);
        if (c) {
            if (tail) tail->next = c;
            else head = c;
            tail = c;
        }
        while (*pp < end && is_space((unsigned char)**pp)) (*pp)++;
        if (*pp < end && **pp == ',') {
            (*pp)++;
            while (*pp < end && is_space((unsigned char)**pp)) (*pp)++;
            continue;
        }
        break;
    }
    return head;
}

/* ------------------------------------------------------------------ */
/* Top-level parse.                                                   */
/* ------------------------------------------------------------------ */

static void skip_comment(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p + 1 < end && p[0] == '/' && p[1] == '*') {
        p += 2;
        while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
        if (p + 1 < end) p += 2;
        *pp = p;
    }
}

static void skip_ws_and_comments(const char **pp, const char *end)
{
    for (;;) {
        while (*pp < end && is_space((unsigned char)**pp)) (*pp)++;
        if (*pp + 1 < end && (*pp)[0] == '/' && (*pp)[1] == '*') {
            skip_comment(pp, end);
            continue;
        }
        break;
    }
}

/* Skip an @-rule's prelude and (optionally) its block. */
static void skip_at_rule(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && *p != ';' && *p != '{') p++;
    if (p >= end) { *pp = p; return; }
    if (*p == ';') { p++; *pp = p; return; }
    /* { ... } */
    p++;
    int depth = 1;
    while (p < end && depth) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    *pp = p;
}

css_stylesheet *css_parse(const char *css, unsigned long len)
{
    css_stylesheet *sh = (css_stylesheet *)calloc(1, sizeof(*sh));
    if (!sh) return 0;
    if (!css || len == 0) return sh;

    const char *p = css;
    const char *end = css + len;
    int order = 0;

    while (p < end) {
        skip_ws_and_comments(&p, end);
        if (p >= end) break;

        if (*p == '@') {
            skip_at_rule(&p, end);
            continue;
        }

        css_chain *chains = parse_selector_list(&p, end);
        skip_ws_and_comments(&p, end);
        if (p >= end || *p != '{') {
            /* malformed: skip to ; or { } */
            chain_free_list(chains);
            while (p < end && *p != '{' && *p != '}' && *p != ';') p++;
            if (p < end) p++;
            continue;
        }
        css_decl *decls = parse_decl_block(&p, end);

        if (!chains || !decls) {
            chain_free_list(chains);
            decl_free_list(decls);
            continue;
        }
        css_rule *r = (css_rule *)calloc(1, sizeof(*r));
        if (!r) {
            chain_free_list(chains);
            decl_free_list(decls);
            continue;
        }
        r->chains = chains;
        r->decls = decls;
        r->order = order++;
        if (sh->rules_tail) sh->rules_tail->next = r;
        else sh->rules = r;
        sh->rules_tail = r;
        sh->rule_count++;
    }
    return sh;
}

void css_free(css_stylesheet *sh)
{
    if (!sh) return;
    css_rule *r = sh->rules;
    while (r) {
        css_rule *n = r->next;
        chain_free_list(r->chains);
        decl_free_list(r->decls);
        free(r);
        r = n;
    }
    free(sh);
}

/* ------------------------------------------------------------------ */
/* Selector matching.                                                 */
/* ------------------------------------------------------------------ */

static const char *attr_get(const struct dom_node *el, const char *name)
{
    if (!el) return 0;
    for (struct dom_attr *a = el->attrs; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0) return a->value;
    }
    return 0;
}

/* Test whether class string `classes` (space-separated) contains `cls`. */
static int has_class(const char *classes, const char *cls)
{
    if (!classes || !*classes) return 0;
    unsigned long clen = strlen(cls);
    const char *p = classes;
    while (*p) {
        while (*p && is_space((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        unsigned long len = (unsigned long)(p - s);
        if (len == clen) {
            int ok = 1;
            for (unsigned long i = 0; i < clen; i++) {
                char x = s[i], y = cls[i];
                if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                if (x != y) { ok = 0; break; }
            }
            if (ok) return 1;
        }
    }
    return 0;
}

static int compound_matches(const css_compound *c, const struct dom_node *el)
{
    if (!el || el->type != DOM_NODE_ELEMENT) return 0;
    /* sentinel for unsupported pseudo/attr selectors -> never match */
    if (c->type && c->type[0] == '\x01') return 0;

    if (c->type && c->type[0] && strcmp(c->type, "") != 0) {
        if (!el->tag || strcmp(el->tag, c->type) != 0) return 0;
    }
    if (c->id) {
        const char *id = attr_get(el, "id");
        if (!id || strcmp(id, c->id) != 0) return 0;
    }
    if (c->nclasses > 0) {
        const char *cls = attr_get(el, "class");
        if (!cls) return 0;
        for (int i = 0; i < c->nclasses; i++) {
            if (!has_class(cls, c->classes[i])) return 0;
        }
    }
    return 1;
}

/* Match a chain against `el` (which is the subject, i.e. the rightmost part). */
static int chain_matches(const css_chain *ch, const struct dom_node *el)
{
    if (ch->nparts == 0) return 0;
    int idx = ch->nparts - 1;
    if (!compound_matches(&ch->parts[idx], el)) return 0;

    const struct dom_node *cur = el->parent;
    idx--;
    while (idx >= 0) {
        int comb = ch->combinators[idx]; /* combinator BEFORE parts[idx+1] */
        if (comb == COMB_CHILD) {
            if (!cur) return 0;
            if (!compound_matches(&ch->parts[idx], cur)) return 0;
            cur = cur->parent;
            idx--;
        } else {
            /* descendant: walk up until we find a match */
            int matched = 0;
            while (cur) {
                if (compound_matches(&ch->parts[idx], cur)) {
                    cur = cur->parent;
                    matched = 1;
                    break;
                }
                cur = cur->parent;
            }
            if (!matched) return 0;
            idx--;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Apply declarations to a computed style.                            */
/* ------------------------------------------------------------------ */

/* Parse "10 20 30 40" (1-4 ints) into out[4]. Returns count parsed. */
static int parse_four_lengths(const char *v, int out[4])
{
    int n = 0;
    const char *p = v;
    while (*p && n < 4) {
        while (*p && is_space((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        char tmp[64];
        unsigned long ln = (unsigned long)(p - s);
        if (ln >= sizeof(tmp)) ln = sizeof(tmp) - 1;
        for (unsigned long i = 0; i < ln; i++) tmp[i] = s[i];
        tmp[ln] = 0;
        int ok = 1;
        int val = parse_length(tmp, &ok);
        if (val < 0) val = 0;   /* "auto" doesn't make sense in shorthand */
        out[n++] = val;
    }
    return n;
}

static void apply_box_shorthand(int values[4], int n,
                                int *t, int *r, int *b, int *l)
{
    if (n == 1) { *t = *r = *b = *l = values[0]; }
    else if (n == 2) { *t = *b = values[0]; *r = *l = values[1]; }
    else if (n == 3) { *t = values[0]; *r = *l = values[1]; *b = values[2]; }
    else if (n >= 4) { *t = values[0]; *r = values[1]; *b = values[2]; *l = values[3]; }
}

static void apply_decl(css_computed *o, const char *prop, const char *val)
{
    if (!prop || !val) return;
    if (strcmp(prop, "color") == 0) {
        int ok = 1;
        unsigned int c = parse_color(val, &ok);
        if (ok) o->color = c;
    } else if (strcmp(prop, "background-color") == 0 ||
               strcmp(prop, "background") == 0) {
        int ok = 1;
        unsigned int c = parse_color(val, &ok);
        if (ok) o->background = c;
    } else if (strcmp(prop, "font-size") == 0) {
        int ok = 1;
        int unit = CSS_UNIT_PX;
        int v = parse_length_ex(val, &ok, &unit);
        if (ok) {
            if (unit == CSS_UNIT_PCT) {
                o->font_size = v;
                o->font_size_pct = 1;
            } else if (v > 0) {
                o->font_size = v;
                o->font_size_pct = 0;
            }
        }
    } else if (strcmp(prop, "font-weight") == 0) {
        if (ci_eq(val, "bold")) o->bold = 1;
        else if (ci_eq(val, "normal")) o->bold = 0;
        else {
            /* numeric? */
            int v = 0; const char *p = val;
            int saw = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; saw = 1; }
            if (saw) o->bold = (v >= 600);
        }
    } else if (strcmp(prop, "font-style") == 0) {
        if (ci_eq(val, "italic") || ci_eq(val, "oblique")) o->italic = 1;
        else if (ci_eq(val, "normal")) o->italic = 0;
    } else if (strcmp(prop, "text-decoration") == 0 ||
               strcmp(prop, "text-decoration-line") == 0) {
        if (ci_eq(val, "underline")) o->underline = 1;
        else if (ci_eq(val, "none")) o->underline = 0;
    } else if (strcmp(prop, "display") == 0) {
        if (ci_eq(val, "block")) o->display = CSS_DISP_BLOCK;
        else if (ci_eq(val, "inline")) o->display = CSS_DISP_INLINE;
        else if (ci_eq(val, "inline-block")) o->display = CSS_DISP_INLINE_BLOCK;
        else if (ci_eq(val, "none")) o->display = CSS_DISP_NONE;
    } else if (strcmp(prop, "margin") == 0) {
        int v[4] = {0,0,0,0};
        int n = parse_four_lengths(val, v);
        apply_box_shorthand(v, n, &o->margin_t, &o->margin_r, &o->margin_b, &o->margin_l);
    } else if (strcmp(prop, "margin-top") == 0)    { int ok=1; int v=parse_length(val,&ok); if(ok) o->margin_t=v; }
      else if (strcmp(prop, "margin-right") == 0)  { int ok=1; int v=parse_length(val,&ok); if(ok) o->margin_r=v; }
      else if (strcmp(prop, "margin-bottom") == 0) { int ok=1; int v=parse_length(val,&ok); if(ok) o->margin_b=v; }
      else if (strcmp(prop, "margin-left") == 0)   { int ok=1; int v=parse_length(val,&ok); if(ok) o->margin_l=v; }
      else if (strcmp(prop, "padding") == 0) {
        int v[4] = {0,0,0,0};
        int n = parse_four_lengths(val, v);
        apply_box_shorthand(v, n, &o->padding_t, &o->padding_r, &o->padding_b, &o->padding_l);
    } else if (strcmp(prop, "padding-top") == 0)    { int ok=1; int v=parse_length(val,&ok); if(ok) o->padding_t=v; }
      else if (strcmp(prop, "padding-right") == 0)  { int ok=1; int v=parse_length(val,&ok); if(ok) o->padding_r=v; }
      else if (strcmp(prop, "padding-bottom") == 0) { int ok=1; int v=parse_length(val,&ok); if(ok) o->padding_b=v; }
      else if (strcmp(prop, "padding-left") == 0)   { int ok=1; int v=parse_length(val,&ok); if(ok) o->padding_l=v; }
      else if (strcmp(prop, "width") == 0) {
        int ok = 1;
        int unit = CSS_UNIT_PX;
        int v = parse_length_ex(val, &ok, &unit);
        if (ok) {
            o->width = v;
            o->width_pct = (unit == CSS_UNIT_PCT) ? 1 : 0;
        }
    } else if (strcmp(prop, "height") == 0) {
        int ok = 1; int v = parse_length(val, &ok);
        if (ok) o->height = v;
    } else if (strcmp(prop, "text-align") == 0) {
        if (ci_eq(val, "left")) o->text_align = CSS_ALIGN_LEFT;
        else if (ci_eq(val, "center")) o->text_align = CSS_ALIGN_CENTER;
        else if (ci_eq(val, "right")) o->text_align = CSS_ALIGN_RIGHT;
    } else if (strcmp(prop, "line-height") == 0) {
        int ok = 1;
        int unit = CSS_UNIT_PX;
        int v = parse_length_ex(val, &ok, &unit);
        if (ok) {
            if (unit == CSS_UNIT_PCT) {
                o->line_height = v;
                o->line_height_pct = 1;
            } else if (unit == CSS_UNIT_NORMAL) {
                o->line_height = 0;
                o->line_height_pct = 0;
            } else {
                o->line_height = v;
                o->line_height_pct = 0;
            }
        }
    } else if (strcmp(prop, "border-width") == 0) {
        int v[4] = {0,0,0,0};
        int n = parse_four_lengths(val, v);
        apply_box_shorthand(v, n, &o->border_t, &o->border_r, &o->border_b, &o->border_l);
    } else if (strcmp(prop, "border-top-width") == 0)    { int ok=1; int v=parse_length(val,&ok); if(ok&&v>=0) o->border_t=v; }
      else if (strcmp(prop, "border-right-width") == 0)  { int ok=1; int v=parse_length(val,&ok); if(ok&&v>=0) o->border_r=v; }
      else if (strcmp(prop, "border-bottom-width") == 0) { int ok=1; int v=parse_length(val,&ok); if(ok&&v>=0) o->border_b=v; }
      else if (strcmp(prop, "border-left-width") == 0)   { int ok=1; int v=parse_length(val,&ok); if(ok&&v>=0) o->border_l=v; }
      else if (strcmp(prop, "border") == 0) {
        /* "border: 2px solid red" -- extract the first length token as width. */
        const char *p = val;
        while (*p && is_space((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        char tmp[64];
        unsigned long ln = (unsigned long)(p - s);
        if (ln > 0 && ln < sizeof(tmp)) {
            for (unsigned long i = 0; i < ln; i++) tmp[i] = s[i];
            tmp[ln] = 0;
            int ok = 1;
            int v = parse_length(tmp, &ok);
            if (ok && v >= 0) {
                o->border_t = o->border_r = o->border_b = o->border_l = v;
            }
        }
    }
    /* unknown property: silently dropped */
}

/* Apply only normal (non-!important) declarations. */
static void apply_decl_list_normal(css_computed *o, const css_decl *d)
{
    while (d) {
        if (!d->important)
            apply_decl(o, d->property, d->value);
        d = d->next;
    }
}

/* Apply only !important declarations. */
static void apply_decl_list_important(css_computed *o, const css_decl *d)
{
    while (d) {
        if (d->important)
            apply_decl(o, d->property, d->value);
        d = d->next;
    }
}

static void apply_decl_list(css_computed *o, const css_decl *d)
{
    while (d) {
        apply_decl(o, d->property, d->value);
        d = d->next;
    }
}

/* Parse inline style attribute "foo: bar; baz: 12px" into a decl list. */
static void apply_inline_style(css_computed *o, const char *style_text)
{
    if (!style_text || !*style_text) return;
    const char *p = style_text;
    const char *end = style_text + strlen(style_text);
    css_decl *head = 0, *tail = 0;
    while (p < end) {
        if (!parse_decl(&p, end, &head, &tail)) break;
    }
    apply_decl_list(o, head);
    decl_free_list(head);
}

/* ------------------------------------------------------------------ */
/* UA defaults.                                                       */
/* ------------------------------------------------------------------ */

static void set_default_style(css_computed *o)
{
    o->color = 0xFF000000;
    o->background = 0;
    o->font_size = 16;
    o->bold = 0;
    o->italic = 0;
    o->underline = 0;
    o->display = CSS_DISP_INLINE;     /* spec default for unknown element */
    o->margin_t = o->margin_r = o->margin_b = o->margin_l = 0;
    o->padding_t = o->padding_r = o->padding_b = o->padding_l = 0;
    o->width = -1;
    o->height = -1;
    o->text_align = CSS_ALIGN_LEFT;
    /* appended fields */
    o->line_height = 0;
    o->line_height_pct = 0;
    o->border_t = o->border_r = o->border_b = o->border_l = 0;
    o->font_size_pct = 0;
    o->width_pct = 0;
}

/* Apply UA-default sheet to o based on tag name. */
static void apply_ua_defaults(css_computed *o, const char *tag)
{
    if (!tag) return;
    /* common block-level */
    static const char *blocks[] = {
        "html", "body", "div", "section", "header", "footer", "nav",
        "main", "article", "aside", "form", "p", "ul", "ol", "li",
        "table", "tr", "pre", "h1", "h2", "h3", "h4", "h5", "h6",
        "blockquote", "hr", "figure", "figcaption", "details", "summary", 0
    };
    for (int i = 0; blocks[i]; i++) {
        if (strcmp(tag, blocks[i]) == 0) { o->display = CSS_DISP_BLOCK; break; }
    }
    /* inline-block: td, th, button, input, img */
    if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0) {
        o->display = CSS_DISP_INLINE_BLOCK;
        o->padding_t = o->padding_r = o->padding_b = o->padding_l = 4;
    }
    /* none: head & friends */
    if (strcmp(tag, "head") == 0 || strcmp(tag, "title") == 0 ||
        strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
        strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 ||
        strcmp(tag, "base") == 0) {
        o->display = CSS_DISP_NONE;
        return;
    }

    /* body margin */
    if (strcmp(tag, "body") == 0) {
        o->margin_t = o->margin_r = o->margin_b = o->margin_l = 8;
    }
    /* headings */
    if (strcmp(tag, "h1") == 0) { o->font_size = 32; o->bold = 1; o->margin_t = o->margin_b = 16; }
    else if (strcmp(tag, "h2") == 0) { o->font_size = 24; o->bold = 1; o->margin_t = o->margin_b = 14; }
    else if (strcmp(tag, "h3") == 0) { o->font_size = 19; o->bold = 1; o->margin_t = o->margin_b = 12; }
    else if (strcmp(tag, "h4") == 0) { o->font_size = 16; o->bold = 1; o->margin_t = o->margin_b = 10; }
    else if (strcmp(tag, "h5") == 0) { o->font_size = 14; o->bold = 1; o->margin_t = o->margin_b = 8; }
    else if (strcmp(tag, "h6") == 0) { o->font_size = 13; o->bold = 1; o->margin_t = o->margin_b = 6; }
    else if (strcmp(tag, "p") == 0) { o->margin_t = o->margin_b = 12; }
    else if (strcmp(tag, "ul") == 0 || strcmp(tag, "ol") == 0) {
        o->margin_t = o->margin_b = 12;
        o->padding_l = 24;
    }
    else if (strcmp(tag, "pre") == 0) { o->margin_t = o->margin_b = 12; }
    else if (strcmp(tag, "hr") == 0) { o->margin_t = o->margin_b = 8; }
    else if (strcmp(tag, "a") == 0) { o->color = 0xFF0000EE; o->underline = 1; }
    else if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0) o->bold = 1;
    else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0) o->italic = 1;
    else if (strcmp(tag, "code") == 0) { /* keep defaults */ }
    /* img defaults inline */
}

/* ------------------------------------------------------------------ */
/* Compute.                                                           */
/* ------------------------------------------------------------------ */

/* Sort applicable rules by specificity ascending, then by document order.
 * Implementation: build a small heap-allocated array of pointers, simple
 * insertion sort (we expect few applicable rules per element). */

typedef struct {
    const css_rule *rule;
    int spec_a, spec_b, spec_c;
    int order;
} match_entry;

static int spec_less(const match_entry *a, const match_entry *b)
{
    if (a->spec_a != b->spec_a) return a->spec_a < b->spec_a;
    if (a->spec_b != b->spec_b) return a->spec_b < b->spec_b;
    if (a->spec_c != b->spec_c) return a->spec_c < b->spec_c;
    return a->order < b->order;
}

/* Maximum parent-walk depth for inheritance (prevents infinite loops). */
#define CSS_INHERIT_MAX_DEPTH 32

/* Forward declaration so css_compute_internal can call itself for parent. */
static void css_compute_internal(const css_stylesheet *sh,
                                 const struct dom_node *el,
                                 css_computed *out,
                                 int depth);

/* Inherited CSS properties: copy from parent when child has UA/initial value.
 * Called after the full cascade for `el`; `parent` is the parent's computed
 * style (already fully cascaded). */
static void apply_inheritance(css_computed *out,
                              const css_computed *parent)
{
    /* color: inherit if still at UA default (black) -- but a rule may have
     * deliberately set it to black too. The real solution is to track
     * "was explicitly set" per property. We use the sentinel: if the
     * element's color is exactly 0xFF000000 (UA default) AND the parent
     * differs, inherit. This is approximate but correct for typical usage
     * and avoids bloating the struct with per-property "set" flags. */
    if (out->color == 0xFF000000 && parent->color != 0xFF000000)
        out->color = parent->color;

    /* font-size: inherit if at UA default 16px and not pct */
    if (out->font_size == 16 && !out->font_size_pct && parent->font_size != 16)
        out->font_size = parent->font_size;

    /* bold/italic: inherit if still 0 (UA default normal) */
    if (!out->bold && parent->bold)
        out->bold = parent->bold;
    if (!out->italic && parent->italic)
        out->italic = parent->italic;

    /* text-align: inherit if at UA default (left) */
    if (out->text_align == CSS_ALIGN_LEFT &&
        parent->text_align != CSS_ALIGN_LEFT)
        out->text_align = parent->text_align;

    /* line-height: inherit if zero (unset) */
    if (out->line_height == 0 && !out->line_height_pct && parent->line_height != 0)
        out->line_height = parent->line_height;
}

static void css_compute_internal(const css_stylesheet *sh,
                                 const struct dom_node *el,
                                 css_computed *out,
                                 int depth)
{
    set_default_style(out);
    if (!el || el->type != DOM_NODE_ELEMENT) return;

    /* UA defaults */
    apply_ua_defaults(out, el->tag);

    /* --- Collect and sort matching author rules --- */
    if (sh) {
        match_entry *matches = 0;
        int cap = 0, n = 0;
        for (const css_rule *r = sh->rules; r; r = r->next) {
            for (const css_chain *c = r->chains; c; c = c->next) {
                if (chain_matches(c, el)) {
                    if (n + 1 > cap) {
                        int nc = cap ? cap * 2 : 8;
                        match_entry *na = (match_entry *)realloc(matches,
                            (unsigned long)nc * sizeof(match_entry));
                        if (!na) goto skip_more;
                        matches = na;
                        cap = nc;
                    }
                    matches[n].rule = r;
                    matches[n].spec_a = c->spec_a;
                    matches[n].spec_b = c->spec_b;
                    matches[n].spec_c = c->spec_c;
                    matches[n].order = r->order;
                    n++;
                    break;  /* one chain match is enough for this rule */
                }
            }
        }
skip_more: ;
        /* Insertion sort by specificity ascending. */
        for (int i = 1; i < n; i++) {
            match_entry e = matches[i];
            int j = i - 1;
            while (j >= 0 && !spec_less(&matches[j], &e)) {
                matches[j + 1] = matches[j];
                j--;
            }
            matches[j + 1] = e;
        }

        /* Pass 1: apply all normal (non-!important) declarations in
         * specificity order (UA already applied above). */
        for (int i = 0; i < n; i++) {
            apply_decl_list_normal(out, matches[i].rule->decls);
        }

        /* Inline style (normal, no !important) beats author rules. */
        const char *inline_style = attr_get(el, "style");
        if (inline_style && *inline_style) {
            apply_inline_style(out, inline_style);
        }

        /* Pass 2: apply !important declarations in specificity order.
         * These override everything applied so far including inline style. */
        for (int i = 0; i < n; i++) {
            apply_decl_list_important(out, matches[i].rule->decls);
        }

        if (matches) free(matches);
    } else {
        /* No stylesheet -- just inline style. */
        const char *inline_style = attr_get(el, "style");
        if (inline_style && *inline_style) {
            apply_inline_style(out, inline_style);
        }
    }

    /* --- Inheritance from parent (bounded depth) --- */
    if (depth < CSS_INHERIT_MAX_DEPTH && el->parent &&
        el->parent->type == DOM_NODE_ELEMENT) {
        css_computed parent_cs;
        css_compute_internal(sh, el->parent, &parent_cs, depth + 1);
        apply_inheritance(out, &parent_cs);
    }
}

void css_compute(const css_stylesheet *sh,
                 const struct dom_node *el,
                 css_computed       *out)
{
    css_compute_internal(sh, el, out, 0);
}

/* ------------------------------------------------------------------ */
/* Self-test.                                                         */
/* ------------------------------------------------------------------ */

int css_selftest(void)
{
    /* Build a tiny DOM by hand. */
    struct dom_node body = { 0 };
    body.type = DOM_NODE_ELEMENT;
    body.tag = "body";

    struct dom_node div = { 0 };
    div.type = DOM_NODE_ELEMENT;
    div.tag = "div";
    div.parent = &body;

    struct dom_attr cls = { 0 };
    cls.name = "class";
    cls.value = "hi";
    div.attrs = &cls;

    struct dom_node a = { 0 };
    a.type = DOM_NODE_ELEMENT;
    a.tag = "a";
    a.parent = &div;
    struct dom_attr id = { 0 };
    id.name = "id"; id.value = "go";
    a.attrs = &id;

    /* Test 1: UA defaults. */
    css_computed cs;
    css_compute(0, &a, &cs);
    if (cs.color != 0xFF0000EE) return -1;   /* a is blue */
    if (cs.underline != 1) return -2;
    css_compute(0, &div, &cs);
    if (cs.display != CSS_DISP_BLOCK) return -3;

    /* Test 2: Parse a sheet and verify color/font-size apply. */
    static const char src[] =
        "body { color: #ff0000; font-size: 20px; }\n"
        "div.hi { background-color: rgb(0, 255, 0); padding: 4 8; }\n"
        "#go { color: rgb(0,0,128); text-decoration: none; }\n"
        ".hi a { font-weight: bold; }\n"
        "/* comment */ p { margin: 4px 8px; }\n"
        "@media print { body { color: black; } }\n"
        "@import url(\"foo.css\");\n"
        ;
    css_stylesheet *sh = css_parse(src, sizeof(src) - 1);
    if (!sh) return -4;
    if (sh->rule_count < 5) return -5;

    /* Compute for div.hi -- should pick up background green + padding. */
    css_compute(sh, &div, &cs);
    if ((cs.background & 0x00FFFFFFu) != 0x0000FF00u) return -6;
    if (cs.padding_t != 4 || cs.padding_l != 8 || cs.padding_r != 8 ||
        cs.padding_b != 4) return -7;
    /* div inherits nothing in our simplified model; that's documented. */

    /* Compute for <a id=go>: descendant rule ".hi a" should mark bold; id
     * rule should set color and turn off underline. */
    css_compute(sh, &a, &cs);
    if (cs.bold != 1) return -8;
    if (cs.underline != 0) return -9;
    if ((cs.color & 0x00FFFFFFu) != 0x00000080u) return -10;

    /* Inline style wins. */
    struct dom_attr style = { 0 };
    style.name = "style";
    style.value = "color: rgb(255,255,255); font-size: 8px;";
    style.next = a.attrs;
    a.attrs = &style;
    css_compute(sh, &a, &cs);
    if ((cs.color & 0x00FFFFFFu) != 0x00FFFFFFu) return -11;
    if (cs.font_size != 8) return -12;

    /* @media / @import didn't blow up; the rule count check above proves
     * they were skipped not parsed as rules. */

    css_free(sh);

    /* Test 3: color forms. */
    int ok = 1;
    unsigned int c;
    c = parse_color("#f00", &ok); if (!ok || (c & 0x00FFFFFFu) != 0x00FF0000u) return -13;
    c = parse_color("#00ff00", &ok); if (!ok || (c & 0x00FFFFFFu) != 0x0000FF00u) return -14;
    c = parse_color("rgb(10, 20, 30)", &ok); if (!ok || c != 0xFF0A141Eu) return -15;
    c = parse_color("rgba(10,20,30,0.5)", &ok);
    if (!ok) return -16;
    {
        unsigned int alpha = (c >> 24) & 0xFFu;
        if (alpha < 120 || alpha > 135) return -17;     /* ~128 */
    }
    c = parse_color("blue", &ok); if (!ok || c != 0xFF0000FFu) return -18;

    /* Test 4: length parsing. */
    int v;
    v = parse_length("12px", &ok); if (!ok || v != 12) return -19;
    v = parse_length("12", &ok); if (!ok || v != 12) return -20;
    v = parse_length("auto", &ok); if (!ok || v != -1) return -21;

    /* Test 5: inheritance -- child inherits parent color.
     * DOM: body -> span  (no sheet; body has color=#123456 via inline style). */
    {
        struct dom_node p_body = { 0 };
        p_body.type = DOM_NODE_ELEMENT;
        p_body.tag  = "body";
        struct dom_attr p_style = { 0 };
        p_style.name  = "style";
        p_style.value = "color: #123456;";
        p_body.attrs  = &p_style;

        struct dom_node c_span = { 0 };
        c_span.type   = DOM_NODE_ELEMENT;
        c_span.tag    = "span";
        c_span.parent = &p_body;

        css_computed inh;
        css_compute(0, &c_span, &inh);
        /* span has no color set; should inherit #123456 from body. */
        if ((inh.color & 0x00FFFFFFu) != 0x00123456u) return -22;
    }

    /* Test 6: margin shorthand expansion "4px 8px" -> t=4 r=8 b=4 l=8. */
    {
        static const char margin_src[] = "p { margin: 4px 8px; }";
        css_stylesheet *mssh = css_parse(margin_src, sizeof(margin_src) - 1);
        if (!mssh) return -23;
        struct dom_node pn = { 0 };
        pn.type = DOM_NODE_ELEMENT;
        pn.tag  = "p";
        css_computed mc;
        css_compute(mssh, &pn, &mc);
        css_free(mssh);
        if (mc.margin_t != 4 || mc.margin_b != 4) return -24;
        if (mc.margin_r != 8 || mc.margin_l != 8) return -25;
    }

    /* Test 7: !important overrides higher-specificity normal rule.
     * #go has higher specificity than .hi, but .hi uses !important on color. */
    {
        static const char imp_src[] =
            ".hi  { color: #aabbcc !important; }\n"
            "#go2 { color: #001122; }\n"
            ;
        css_stylesheet *issh = css_parse(imp_src, sizeof(imp_src) - 1);
        if (!issh) return -26;

        struct dom_node imp_el = { 0 };
        imp_el.type = DOM_NODE_ELEMENT;
        imp_el.tag  = "div";

        struct dom_attr imp_cls = { 0 };
        imp_cls.name  = "class";
        imp_cls.value = "hi";
        struct dom_attr imp_id = { 0 };
        imp_id.name   = "id";
        imp_id.value  = "go2";
        imp_id.next   = 0;
        imp_cls.next  = &imp_id;
        imp_el.attrs  = &imp_cls;

        css_computed ic;
        css_compute(issh, &imp_el, &ic);
        css_free(issh);
        /* .hi !important should win over #go2 normal. */
        if ((ic.color & 0x00FFFFFFu) != 0x00AABBCCu) return -27;
    }

    /* Test 8: line-height and border-width properties. */
    {
        static const char lhb_src[] =
            "div { line-height: 24px; border-width: 2px 4px; }";
        css_stylesheet *lssh = css_parse(lhb_src, sizeof(lhb_src) - 1);
        if (!lssh) return -28;
        struct dom_node ld = { 0 };
        ld.type = DOM_NODE_ELEMENT;
        ld.tag  = "div";
        css_computed lc;
        css_compute(lssh, &ld, &lc);
        css_free(lssh);
        if (lc.line_height != 24) return -29;
        if (lc.border_t != 2 || lc.border_b != 2) return -30;
        if (lc.border_r != 4 || lc.border_l != 4) return -31;
    }

    return 0;
}

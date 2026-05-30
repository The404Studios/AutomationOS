/*
 * dom_selector.c -- CSS selector matcher (querySelector / querySelectorAll).
 * ==========================================================================
 *
 * Freestanding ring-3.  No libc/stdio/stack-protector canary.
 * Build: -std=gnu11 -ffreestanding -nostdlib -fno-builtin
 *        -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
 *        -mstackrealign -O2
 *
 * See dom_selector.h for the supported CSS subset.
 *
 * Internal design overview
 * ========================
 *  1. A selector string is first split on top-level commas into "branches".
 *     A match succeeds if any branch matches.
 *
 *  2. Each branch is parsed into a chain of "steps" separated by combinators
 *     (descendant = space, child = '>').  The rightmost step is the subject;
 *     left steps constrain its ancestors.
 *
 *  3. A "step" (selector_step) holds up to SEL_MAX_SIMPLE simple selectors
 *     (type, id, class, attr) that all must hold on one element.
 *
 *  4. Matching a branch against an element works right-to-left: the subject
 *     step tests the element itself, then each preceding step walks up the
 *     parent chain applying its combinator.
 *
 *  5. dom_query_selector / dom_query_selector_all do a depth-first pre-order
 *     walk of the subtree (children only, not the root node itself) and
 *     collect matches.
 *
 * All parsing is done on a temporary stack-allocated buffer (or malloc'd for
 * the branch copy).  No persistent global state.
 */

#include "dom_selector.h"

/* ------------------------------------------------------------------ */
/*  Private helpers -- no stdio, no libc beyond what dom.h pulls in   */
/* ------------------------------------------------------------------ */

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Defensive walk cap -- prevents runaway on malformed trees. */
#define SEL_WALK_MAX_DEPTH   256
#define SEL_WALK_MAX_NODES   (1u << 20)

/* Max simple selectors per compound step  (e.g. div.a.b#c[x="y"])
 * NOTE: these maxima directly size selector_step / selector_branch, which are
 * stack/heap allocated. Keep them modest -- an 8x8 branch is already ~8KB. */
#define SEL_MAX_SIMPLE   8

/* Max steps per branch  (e.g.  "a > b c d" = 4) */
#define SEL_MAX_STEPS    8

/* Max selector string length we are willing to process */
#define SEL_MAX_LEN      2048

/* Max branches in a selector list */
#define SEL_MAX_BRANCHES 16

/* ---- tiny string helpers (avoid dragging in more libc) ------------ */

static int sel_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int sel_strlen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void sel_memcpy(char *dst, const char *src, int n)
{
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

/* Zero `n` bytes at `dst`. Hand-rolled to stay self-contained under
 * -fno-builtin (no reliance on the libc memset symbol). */
static void sel_memset(void *dst, int val, unsigned long n)
{
    unsigned char *p = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)val;
}

/* NUL-terminate a copy of src[0..n-1] into dst which has room for n+1. */
static void sel_copy(char *dst, const char *src, int n)
{
    sel_memcpy(dst, src, n);
    dst[n] = '\0';
}

static int sel_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Case-insensitive ASCII compare */
static int sel_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
    return (unsigned char)ca - (unsigned char)cb;
}

/*
 * Check whether the space-separated token list `list` contains `token`.
 * Used for class matching: class attribute may hold "foo bar baz".
 */
static int class_list_contains(const char *list, const char *token)
{
    if (!list || !token) return 0;
    int tlen = sel_strlen(token);
    const char *p = list;
    while (*p) {
        /* skip leading spaces */
        while (*p && sel_isspace(*p)) p++;
        /* find end of token */
        const char *start = p;
        while (*p && !sel_isspace(*p)) p++;
        int len = (int)(p - start);
        if (len == tlen) {
            int match = 1;
            for (int i = 0; i < len; i++) {
                if (start[i] != token[i]) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Simple-selector kinds                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    SIMPLE_UNIVERSAL = 0,  /* *             */
    SIMPLE_TYPE,           /* div           */
    SIMPLE_CLASS,          /* .foo          */
    SIMPLE_ID,             /* #bar          */
    SIMPLE_ATTR_PRESENCE,  /* [href]        */
    SIMPLE_ATTR_EXACT      /* [href="val"]  */
} simple_kind;

/* Maximum storage for a name or value within a simple selector. */
#define SIMPLE_BUF 64

typedef struct {
    simple_kind kind;
    char        name[SIMPLE_BUF];   /* tag, class token, id, or attr name */
    char        value[SIMPLE_BUF];  /* attr value (SIMPLE_ATTR_EXACT only) */
} simple_sel;

/* ------------------------------------------------------------------ */
/*  Step = one compound selector + combinator that follows it         */
/* ------------------------------------------------------------------ */

typedef enum {
    COMB_NONE       = 0,  /* rightmost (subject) step         */
    COMB_DESCENDANT,      /* ' '  ancestor anywhere above     */
    COMB_CHILD            /* '>'  direct parent only          */
} combinator;

typedef struct {
    simple_sel  simples[SEL_MAX_SIMPLE];
    int         nsimples;
    combinator  comb;     /* combinator connecting THIS step to the NEXT
                             (more-subject) step to its right; NONE for
                             the rightmost (subject) step itself          */
} selector_step;

/* ------------------------------------------------------------------ */
/*  Branch = ordered list of steps (left = root-side, right = subject)*/
/* ------------------------------------------------------------------ */

typedef struct {
    selector_step steps[SEL_MAX_STEPS];
    int           nsteps;
} selector_branch;

/* ------------------------------------------------------------------ */
/*  Parser                                                             */
/* ------------------------------------------------------------------ */

/*
 * Parse a single simple selector starting at *pp.
 * Advances *pp past the consumed characters.
 * Returns 1 on success, 0 on end-of-compound, -1 on error.
 *
 * Stops (returns 0) at: whitespace, '>', ',', '[' (handled separately),
 * or end of string -- after the caller has already tested for special chars.
 */

/* Forward decl */
static int parse_attr(const char **pp, simple_sel *out);

/*
 * parse_compound: parse a run of simple selectors (no whitespace, no comma,
 * no combinator) into step->simples[].  Returns 1 ok, -1 error.
 */
static int parse_compound(const char **pp, selector_step *step)
{
    step->nsimples = 0;

    while (**pp && **pp != ',' && !sel_isspace(**pp) && **pp != '>') {
        if (step->nsimples >= SEL_MAX_SIMPLE) return -1;  /* too complex */
        simple_sel *s = &step->simples[step->nsimples];

        char c = **pp;

        if (c == '*') {
            s->kind = SIMPLE_UNIVERSAL;
            s->name[0] = '\0';
            s->value[0] = '\0';
            (*pp)++;
            step->nsimples++;
        } else if (c == '.') {
            /* class */
            (*pp)++;
            s->kind = SIMPLE_CLASS;
            int i = 0;
            while (**pp && **pp != '.' && **pp != '#' && **pp != '[' &&
                   **pp != ',' && !sel_isspace(**pp) && **pp != '>') {
                if (i >= SIMPLE_BUF - 1) return -1;
                s->name[i++] = **pp;
                (*pp)++;
            }
            if (i == 0) return -1;  /* empty class name */
            s->name[i] = '\0';
            s->value[0] = '\0';
            step->nsimples++;
        } else if (c == '#') {
            /* id */
            (*pp)++;
            s->kind = SIMPLE_ID;
            int i = 0;
            while (**pp && **pp != '.' && **pp != '#' && **pp != '[' &&
                   **pp != ',' && !sel_isspace(**pp) && **pp != '>') {
                if (i >= SIMPLE_BUF - 1) return -1;
                s->name[i++] = **pp;
                (*pp)++;
            }
            if (i == 0) return -1;
            s->name[i] = '\0';
            s->value[0] = '\0';
            step->nsimples++;
        } else if (c == '[') {
            /* attribute */
            int r = parse_attr(pp, s);
            if (r < 0) return -1;
            step->nsimples++;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   c == '_' || c == '-') {
            /* type selector: letters, digits, hyphen, underscore */
            s->kind = SIMPLE_TYPE;
            int i = 0;
            while (**pp && **pp != '.' && **pp != '#' && **pp != '[' &&
                   **pp != ',' && !sel_isspace(**pp) && **pp != '>') {
                if (i >= SIMPLE_BUF - 1) return -1;
                /* lowercase for case-insensitive tag compare */
                char ch = **pp;
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                s->name[i++] = ch;
                (*pp)++;
            }
            s->name[i] = '\0';
            s->value[0] = '\0';
            step->nsimples++;
        } else {
            /* Unknown / unsupported character */
            return -1;
        }
    }

    return (step->nsimples > 0) ? 1 : 0;
}

/*
 * parse_attr: parse "[name]" or "[name="value"]" or "[name='value']"
 * starting at '['.  Advances *pp past the closing ']'.
 * Returns 1 ok, -1 error.
 */
static int parse_attr(const char **pp, simple_sel *out)
{
    if (**pp != '[') return -1;
    (*pp)++;  /* skip '[' */

    /* attribute name */
    int ni = 0;
    while (**pp && **pp != ']' && **pp != '=') {
        if (ni >= SIMPLE_BUF - 1) return -1;
        char ch = **pp;
        /* lowercase attribute name */
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        out->name[ni++] = ch;
        (*pp)++;
    }
    if (ni == 0) return -1;
    out->name[ni] = '\0';
    out->value[0] = '\0';

    if (**pp == ']') {
        /* presence only */
        out->kind = SIMPLE_ATTR_PRESENCE;
        (*pp)++;  /* skip ']' */
        return 1;
    }

    if (**pp != '=') return -1;
    (*pp)++;  /* skip '=' */

    /* value -- may be quoted */
    char quote = 0;
    if (**pp == '"' || **pp == '\'') {
        quote = **pp;
        (*pp)++;
    }

    int vi = 0;
    while (**pp) {
        if (quote) {
            if (**pp == quote) { (*pp)++; break; }
        } else {
            if (**pp == ']') break;
        }
        if (vi >= SIMPLE_BUF - 1) return -1;
        out->value[vi++] = **pp;
        (*pp)++;
    }
    out->value[vi] = '\0';

    /* expect closing ']' */
    if (**pp != ']') return -1;
    (*pp)++;

    out->kind = SIMPLE_ATTR_EXACT;
    return 1;
}

/*
 * parse_branch: parse one comma-free selector branch into `br`.
 * Returns 1 ok, -1 error.
 *
 * Grammar (right-to-left reading):
 *   branch    := step (combinator step)*
 *   combinator:= SPACE | '>'
 *   step      := compound
 */
static int parse_branch(const char *s, selector_branch *br)
{
    br->nsteps = 0;
    const char *p = s;

    while (*p) {
        /* skip leading whitespace */
        while (*p && sel_isspace(*p)) p++;
        if (!*p) break;

        if (br->nsteps >= SEL_MAX_STEPS) return -1;
        selector_step *step = &br->steps[br->nsteps];
        step->nsimples = 0;
        step->comb = COMB_NONE;

        /* parse compound */
        int r = parse_compound(&p, step);
        if (r < 0) return -1;
        if (r == 0) {
            /* nothing parsed -- might be a lone '>' after skipping space */
            if (*p == '>') {
                /* combinator without left operand: error */
                return -1;
            }
            break;
        }

        br->nsteps++;

        /* skip whitespace then check for combinator */
        const char *after_step = p;
        while (*p && sel_isspace(*p)) p++;

        if (*p == '>') {
            step->comb = COMB_CHILD;
            p++;
            while (*p && sel_isspace(*p)) p++;
        } else if (*p && *p != ',') {
            /* whitespace that was actually a descendant combinator */
            step->comb = COMB_DESCENDANT;
            /* p already past the whitespace */
        } else {
            /* end of branch */
            (void)after_step;
            break;
        }
    }

    return (br->nsteps > 0) ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/*  Simple-selector evaluation against a single DOM node              */
/* ------------------------------------------------------------------ */

static int eval_simple(const struct dom_node *el, const simple_sel *s)
{
    if (!el || el->type != DOM_NODE_ELEMENT) return 0;

    switch (s->kind) {
    case SIMPLE_UNIVERSAL:
        return 1;

    case SIMPLE_TYPE:
        /* Case-insensitive type match (HTML semantics; selftest also asserts
           "H1" matches an <h1>). Don't assume both sides are pre-lowercased. */
        if (!el->tag) return 0;
        return sel_icmp(el->tag, s->name) == 0;

    case SIMPLE_CLASS: {
        const char *cls = dom_get_attribute(el, "class");
        if (!cls) return 0;
        return class_list_contains(cls, s->name);
    }

    case SIMPLE_ID: {
        const char *id = dom_get_attribute(el, "id");
        if (!id) return 0;
        return sel_strcmp(id, s->name) == 0;
    }

    case SIMPLE_ATTR_PRESENCE:
        return dom_has_attribute(el, s->name);

    case SIMPLE_ATTR_EXACT: {
        const char *v = dom_get_attribute(el, s->name);
        if (!v) return 0;
        return sel_strcmp(v, s->value) == 0;
    }
    }
    return 0;
}

/* Test all simple selectors in a step against el.  Returns 1 if all pass. */
static int eval_step(const struct dom_node *el, const selector_step *step)
{
    if (!el || el->type != DOM_NODE_ELEMENT) return 0;
    if (step->nsimples == 0) return 0;  /* empty step never matches */
    for (int i = 0; i < step->nsimples; i++) {
        if (!eval_simple(el, &step->simples[i])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Branch matching (handles combinators + ancestor chain)            */
/* ------------------------------------------------------------------ */

/*
 * match_branch_from_step
 * ----------------------
 * Try to match branch->steps[si] (and all steps to its left) against el
 * and its ancestors, working right-to-left.
 *
 * `si` is the index of the current step in `br->steps[]`.
 *   si == br->nsteps-1  is the rightmost (subject) step.
 *   si == 0             is the leftmost (root-most) step.
 *
 * The combinator stored in step[si-1].comb describes how step[si-1]
 * (the step to the LEFT of si) connects to step[si].  In other words,
 * when we succeed at step[si] and need to satisfy step[si-1], we look
 * at step[si-1].comb to know whether to check el->parent only (CHILD)
 * or any ancestor (DESCENDANT).
 *
 * Returns 1 = match, 0 = no match.
 */
static int match_branch_from_step(const struct dom_node *el,
                                  const selector_branch *br, int si,
                                  int depth)
{
    if (depth > SEL_WALK_MAX_DEPTH) return 0;
    if (!el || el->type != DOM_NODE_ELEMENT) return 0;

    /* Test current step against el */
    if (!eval_step(el, &br->steps[si])) return 0;

    /* If this is the leftmost step, we're done -- full match. */
    if (si == 0) return 1;

    /* Otherwise, must satisfy the step to the left (si-1) using the
       combinator that si-1 stored (it connects si-1 -> si). */
    combinator comb = br->steps[si - 1].comb;
    int next_si = si - 1;

    if (comb == COMB_CHILD) {
        /* Parent must match steps[next_si] */
        const struct dom_node *par = el->parent;
        if (!par || par->type != DOM_NODE_ELEMENT) return 0;
        return match_branch_from_step(par, br, next_si, depth + 1);
    } else {
        /* COMB_DESCENDANT: try every ancestor */
        const struct dom_node *anc = el->parent;
        int guard = 0;
        while (anc && anc->type == DOM_NODE_ELEMENT &&
               guard++ < SEL_WALK_MAX_DEPTH) {
            if (match_branch_from_step(anc, br, next_si, depth + 1)) return 1;
            anc = anc->parent;
        }
        return 0;
    }
}

/* Test whether el matches a parsed branch. */
static int match_branch(const struct dom_node *el, const selector_branch *br)
{
    if (!br || br->nsteps == 0) return 0;
    /* Start from the rightmost (subject) step. */
    return match_branch_from_step(el, br, br->nsteps - 1, 0);
}

/* ------------------------------------------------------------------ */
/*  Top-level split on commas (respecting brackets and quotes)        */
/* ------------------------------------------------------------------ */

/*
 * split_branches
 * --------------
 * Split `sel` into comma-separated branches.  Commas inside [] or quotes
 * are ignored.  Fills `branches` with malloc'd copies and sets `*count`.
 * Returns 1 ok, -1 on error/OOM.  Caller must free branches[*] on done.
 */
static int split_branches(const char *sel, char **branches, int *count)
{
    *count = 0;
    int len = sel_strlen(sel);
    if (len <= 0 || len >= SEL_MAX_LEN) return -1;

    /* We iterate through sel tracking bracket/quote nesting */
    const char *p = sel;
    const char *start = p;
    int depth = 0;    /* bracket nesting */
    char in_q = 0;    /* inside quote */

    while (1) {
        char c = *p;
        if (c == '\0') {
            /* end of selector -- store last branch */
            int blen = (int)(p - start);
            /* strip leading/trailing whitespace */
            while (blen > 0 && sel_isspace(start[0])) { start++; blen--; }
            while (blen > 0 && sel_isspace(start[blen - 1])) blen--;
            if (blen > 0) {
                if (*count >= SEL_MAX_BRANCHES) return -1;
                char *copy = (char *)malloc((size_t)(blen + 1));
                if (!copy) return -1;
                sel_copy(copy, start, blen);
                branches[(*count)++] = copy;
            }
            break;
        }
        if (in_q) {
            if (c == in_q) in_q = 0;
        } else if (c == '"' || c == '\'') {
            in_q = c;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (depth > 0) depth--;
        } else if (c == ',' && depth == 0) {
            int blen = (int)(p - start);
            while (blen > 0 && sel_isspace(start[0])) { start++; blen--; }
            while (blen > 0 && sel_isspace(start[blen - 1])) blen--;
            if (blen > 0) {
                if (*count >= SEL_MAX_BRANCHES) return -1;
                char *copy = (char *)malloc((size_t)(blen + 1));
                if (!copy) return -1;
                sel_copy(copy, start, blen);
                branches[(*count)++] = copy;
            }
            start = p + 1;
        }
        p++;
    }
    return 1;
}

static void free_branches(char **branches, int count)
{
    for (int i = 0; i < count; i++) {
        if (branches[i]) free(branches[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int dom_selector_match(const struct dom_node *el, const char *selector)
{
    if (!el || !selector) return 0;
    if (el->type != DOM_NODE_ELEMENT) return 0;

    char *branches[SEL_MAX_BRANCHES];
    int nbranches = 0;
    int rc = split_branches(selector, branches, &nbranches);
    if (rc < 0) return rc;   /* malformed */
    if (nbranches == 0) { return -1; }

    int result = 0;
    int any_error = 0;

    /* A selector_branch is ~8.5 KB (SEL_MAX_STEPS * SEL_MAX_SIMPLE * SIMPLE_BUF).
     * The userspace stack in-OS is only 8-16 KB, so it MUST NOT live on the
     * stack: a single 8.5 KB frame here overflows the user stack and silently
     * clobbers adjacent memory (heap/BSS), which manifested as a layout-
     * sensitive heisenbug (e.g. an element's tag being corrupted before an
     * assertion ran). Allocate it on the heap instead -- the same approach the
     * query functions already use via parse_all_branches(). */
    selector_branch *br = (selector_branch *)malloc(sizeof(selector_branch));
    if (!br) {
        free_branches(branches, nbranches);
        return -1;
    }

    for (int i = 0; i < nbranches; i++) {
        /* Zero-initialize: parse_branch only writes the steps/simples it
         * actually consumes, so any field beyond br->nsteps / step.nsimples is
         * left untouched. malloc memory is uninitialized, so clear it here so
         * any unwritten field reads as zero. */
        sel_memset(br, 0, sizeof *br);
        int pr = parse_branch(branches[i], br);
        if (pr < 0) {
            any_error = 1;
            continue;
        }
        if (match_branch(el, br)) {
            result = 1;
            break;
        }
    }

    free(br);
    free_branches(branches, nbranches);

    if (result) return 1;
    if (any_error) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Depth-first walk helpers                                           */
/* ------------------------------------------------------------------ */

/*
 * The DFS walkers take PRE-PARSED branches. A selector_branch is several KB,
 * so it must never be a per-recursion-frame stack local (that overflows the
 * user stack on deep DOMs -- and re-parsing at every node was O(nodes*branches)
 * wasteful). The caller parses once into a heap array and passes it down; the
 * recursion frames here hold only pointers + a couple of ints.
 */

/* Recursive DFS pre-order visit: test each child (not the root). */
static struct dom_node *dfs_first(const struct dom_node *node,
                                  const selector_branch *parsed, int nparsed,
                                  int depth, unsigned long *visited)
{
    if (!node || depth > SEL_WALK_MAX_DEPTH) return NULL;

    for (struct dom_node *child = node->first_child;
         child; child = child->next_sibling) {
        if (*visited >= SEL_WALK_MAX_NODES) return NULL;
        (*visited)++;

        if (child->type == DOM_NODE_ELEMENT) {
            for (int i = 0; i < nparsed; i++) {
                if (match_branch(child, &parsed[i])) return child;
            }
        }

        struct dom_node *found = dfs_first(child, parsed, nparsed,
                                           depth + 1, visited);
        if (found) return found;
    }
    return NULL;
}

static int dfs_all(const struct dom_node *node,
                   const selector_branch *parsed, int nparsed,
                   struct dom_node **out, int max, int *count,
                   int depth, unsigned long *visited)
{
    if (!node || depth > SEL_WALK_MAX_DEPTH) return 0;

    for (struct dom_node *child = node->first_child;
         child; child = child->next_sibling) {
        if (*visited >= SEL_WALK_MAX_NODES) return 0;
        (*visited)++;

        if (child->type == DOM_NODE_ELEMENT) {
            for (int i = 0; i < nparsed; i++) {
                if (match_branch(child, &parsed[i])) {
                    if (*count < max) out[(*count)++] = child;
                    break;  /* branch matched -- don't double-count */
                }
            }
        }

        if (*count < max) {
            dfs_all(child, parsed, nparsed, out, max, count,
                    depth + 1, visited);
        }
    }
    return 0;
}

/* Parse the split branch strings into a heap array of selector_branch.
 * Returns the array (caller frees) and sets *nparsed, or NULL on OOM. */
static selector_branch *parse_all_branches(char **branches, int nbranches,
                                           int *nparsed)
{
    *nparsed = 0;
    if (nbranches <= 0) return NULL;
    selector_branch *parsed =
        (selector_branch *)malloc(sizeof(selector_branch) * (size_t)nbranches);
    if (!parsed) return NULL;
    /* malloc memory is uninitialized; clear so any step/simple slot that
     * parse_branch does not populate reads as zero (matches the hosted
     * zeroed-stack behaviour -- see note in dom_selector_match). */
    sel_memset(parsed, 0, sizeof(selector_branch) * (size_t)nbranches);
    for (int i = 0; i < nbranches; i++) {
        if (parse_branch(branches[i], &parsed[*nparsed]) >= 0)
            (*nparsed)++;   /* skip malformed branches */
    }
    return parsed;
}

struct dom_node *dom_query_selector(const struct dom_node *root,
                                    const char *selector)
{
    if (!root || !selector) return NULL;

    char *branches[SEL_MAX_BRANCHES];
    int nbranches = 0;
    if (split_branches(selector, branches, &nbranches) < 0) return NULL;
    if (nbranches == 0) return NULL;

    int nparsed = 0;
    selector_branch *parsed = parse_all_branches(branches, nbranches, &nparsed);
    struct dom_node *found = NULL;
    if (parsed) {
        unsigned long visited = 0;
        found = dfs_first(root, parsed, nparsed, 0, &visited);
        free(parsed);
    }
    free_branches(branches, nbranches);
    return found;
}

int dom_query_selector_all(const struct dom_node *root,
                           const char *selector,
                           struct dom_node **out, int max)
{
    if (!root || !selector || !out || max <= 0) return 0;

    char *branches[SEL_MAX_BRANCHES];
    int nbranches = 0;
    if (split_branches(selector, branches, &nbranches) < 0) return 0;
    if (nbranches == 0) return 0;

    int nparsed = 0;
    selector_branch *parsed = parse_all_branches(branches, nbranches, &nparsed);
    int count = 0;
    if (parsed) {
        unsigned long visited = 0;
        dfs_all(root, parsed, nparsed, out, max, &count, 0, &visited);
        free(parsed);
    }
    free_branches(branches, nbranches);
    return count;
}

/* ================================================================== */
/*  Self-test                                                         */
/* ================================================================== */

/*
 * Build the following DOM tree for testing:
 *
 *   document
 *   └── html
 *       ├── head
 *       │   └── title          (text: "Test Page")
 *       └── body [class="page"]
 *           ├── div #main [class="container wide"]
 *           │   ├── h1 [class="title"]  (text: "Hello")
 *           │   ├── p  [class="text"]   (text: "First")
 *           │   └── p  [class="text highlight"]  (text: "Second")
 *           └── footer [class="footer"]
 *               ├── a [href="https://example.com"]  (text: "Link")
 *               └── span [data-id="42"]             (text: "Span")
 *
 * Test battery (selector -> expected match result for a specific element,
 * or expected count for querySelectorAll):
 */

/* Tiny assertion macro -- no stdio, stores first failure line */
static int selftest_failures = 0;
static int selftest_first_fail = 0;

#define ST_ASSERT(cond) do { \
    if (!(cond)) { \
        selftest_failures++; \
        if (!selftest_first_fail) selftest_first_fail = __LINE__; \
    } \
} while (0)

int dom_selector_selftest(void)
{
    selftest_failures   = 0;
    selftest_first_fail = 0;

    /* ---- build tree ---- */
    dom_document *doc = dom_document_new();
    if (!doc) return -100;

    dom_node *html   = dom_create_element("html");
    dom_node *head   = dom_create_element("head");
    dom_node *title  = dom_create_element("title");
    dom_node *body   = dom_create_element("body");
    dom_node *divmain= dom_create_element("div");
    dom_node *h1     = dom_create_element("h1");
    dom_node *p1     = dom_create_element("p");
    dom_node *p2     = dom_create_element("p");
    dom_node *footer = dom_create_element("footer");
    dom_node *anchor = dom_create_element("a");
    dom_node *span   = dom_create_element("span");

    if (!html || !head || !title || !body || !divmain ||
        !h1 || !p1 || !p2 || !footer || !anchor || !span) {
        dom_document_free(doc);
        return -101;
    }

    /* Attributes */
    dom_set_attribute(body,    "class",   "page");
    dom_set_attribute(divmain, "id",      "main");
    dom_set_attribute(divmain, "class",   "container wide");
    dom_set_attribute(h1,      "class",   "title");
    dom_set_attribute(p1,      "class",   "text");
    dom_set_attribute(p2,      "class",   "text highlight");
    dom_set_attribute(anchor,  "href",    "https://example.com");
    dom_set_attribute(span,    "data-id", "42");

    /* Text */
    dom_set_text(title,  "Test Page");
    dom_set_text(h1,     "Hello");
    dom_set_text(p1,     "First");
    dom_set_text(p2,     "Second");
    dom_set_text(anchor, "Link");
    dom_set_text(span,   "Span");

    /* Tree structure */
    dom_append_child(doc->root, html);
    dom_append_child(html,   head);
    dom_append_child(html,   body);
    dom_append_child(head,   title);
    dom_append_child(body,   divmain);
    dom_append_child(body,   footer);
    dom_append_child(divmain, h1);
    dom_append_child(divmain, p1);
    dom_append_child(divmain, p2);
    dom_append_child(footer, anchor);
    dom_append_child(footer, span);

    /* ---------------------------------------------------------------- */
    /*  1. dom_selector_match -- simple selectors                       */
    /* ---------------------------------------------------------------- */

    /* Universal */
    ST_ASSERT(dom_selector_match(h1, "*") == 1);
    ST_ASSERT(dom_selector_match(body, "*") == 1);

    /* Type */
    ST_ASSERT(dom_selector_match(h1,   "h1")    == 1);
    ST_ASSERT(dom_selector_match(h1,   "H1")    == 1);   /* case-insensitive */
    ST_ASSERT(dom_selector_match(h1,   "div")   == 0);
    ST_ASSERT(dom_selector_match(body, "body")  == 1);
    ST_ASSERT(dom_selector_match(span, "span")  == 1);
    ST_ASSERT(dom_selector_match(span, "div")   == 0);

    /* Class -- single token */
    ST_ASSERT(dom_selector_match(body,    ".page")      == 1);
    ST_ASSERT(dom_selector_match(body,    ".other")     == 0);
    ST_ASSERT(dom_selector_match(divmain, ".container") == 1);
    ST_ASSERT(dom_selector_match(divmain, ".wide")      == 1);
    ST_ASSERT(dom_selector_match(divmain, ".narrow")    == 0);
    ST_ASSERT(dom_selector_match(p2,      ".highlight") == 1);
    ST_ASSERT(dom_selector_match(p1,      ".highlight") == 0);

    /* ID */
    ST_ASSERT(dom_selector_match(divmain, "#main")  == 1);
    ST_ASSERT(dom_selector_match(divmain, "#other") == 0);
    ST_ASSERT(dom_selector_match(h1,      "#main")  == 0);

    /* Attribute presence */
    ST_ASSERT(dom_selector_match(anchor, "[href]")    == 1);
    ST_ASSERT(dom_selector_match(anchor, "[class]")   == 0);
    ST_ASSERT(dom_selector_match(span,   "[data-id]") == 1);
    ST_ASSERT(dom_selector_match(h1,     "[href]")    == 0);

    /* Attribute exact (double-quoted) */
    ST_ASSERT(dom_selector_match(anchor, "[href=\"https://example.com\"]") == 1);
    ST_ASSERT(dom_selector_match(anchor, "[href=\"http://other.com\"]")    == 0);

    /* Attribute exact (single-quoted) */
    ST_ASSERT(dom_selector_match(span,   "[data-id='42']") == 1);
    ST_ASSERT(dom_selector_match(span,   "[data-id='0']")  == 0);

    /* ---------------------------------------------------------------- */
    /*  2. dom_selector_match -- compound selectors                     */
    /* ---------------------------------------------------------------- */

    ST_ASSERT(dom_selector_match(divmain, "div.container")   == 1);
    ST_ASSERT(dom_selector_match(divmain, "div.wide")        == 1);
    ST_ASSERT(dom_selector_match(divmain, "div#main")        == 1);
    ST_ASSERT(dom_selector_match(divmain, "div#main.container.wide") == 1);
    ST_ASSERT(dom_selector_match(divmain, "div#main.narrow") == 0);
    ST_ASSERT(dom_selector_match(p2, "p.text.highlight")     == 1);
    ST_ASSERT(dom_selector_match(p1, "p.text.highlight")     == 0);

    /* ---------------------------------------------------------------- */
    /*  3. dom_selector_match -- descendant combinator                  */
    /* ---------------------------------------------------------------- */

    /* h1 is inside div#main which is inside body */
    ST_ASSERT(dom_selector_match(h1,  "div h1")       == 1);
    ST_ASSERT(dom_selector_match(h1,  "body h1")      == 1);
    ST_ASSERT(dom_selector_match(h1,  "html h1")      == 1);
    ST_ASSERT(dom_selector_match(h1,  "footer h1")    == 0);
    ST_ASSERT(dom_selector_match(p2,  "div p")        == 1);
    ST_ASSERT(dom_selector_match(p2,  ".container p") == 1);
    ST_ASSERT(dom_selector_match(p2,  "footer p")     == 0);
    ST_ASSERT(dom_selector_match(anchor, "footer a")  == 1);
    ST_ASSERT(dom_selector_match(anchor, "div a")     == 0);

    /* three-level: html body div h1 */
    ST_ASSERT(dom_selector_match(h1, "body div h1")   == 1);
    ST_ASSERT(dom_selector_match(h1, "html body h1")  == 1);

    /* ---------------------------------------------------------------- */
    /*  4. dom_selector_match -- child combinator                       */
    /* ---------------------------------------------------------------- */

    ST_ASSERT(dom_selector_match(h1,     "div > h1")    == 1);
    ST_ASSERT(dom_selector_match(h1,     "body > h1")   == 0);  /* body is not parent of h1 */
    ST_ASSERT(dom_selector_match(divmain,"body > div")  == 1);
    ST_ASSERT(dom_selector_match(anchor, "footer > a")  == 1);
    ST_ASSERT(dom_selector_match(anchor, "body > a")    == 0);

    /* mix: child + descendant */
    ST_ASSERT(dom_selector_match(h1, "body > div h1")  == 1);
    ST_ASSERT(dom_selector_match(h1, "body > div > h1") == 1);
    ST_ASSERT(dom_selector_match(h1, "html > body > div > h1") == 1);
    ST_ASSERT(dom_selector_match(h1, "html > div > h1") == 0);   /* wrong intermediate */

    /* ---------------------------------------------------------------- */
    /*  5. dom_selector_match -- selector list (comma)                  */
    /* ---------------------------------------------------------------- */

    ST_ASSERT(dom_selector_match(h1,   "h1, p")   == 1);
    ST_ASSERT(dom_selector_match(p1,   "h1, p")   == 1);
    ST_ASSERT(dom_selector_match(span, "h1, p")   == 0);
    ST_ASSERT(dom_selector_match(span, "h1, span") == 1);
    ST_ASSERT(dom_selector_match(body, "footer, body, div") == 1);
    ST_ASSERT(dom_selector_match(anchor, "a[href], span") == 1);

    /* ---------------------------------------------------------------- */
    /*  6. dom_query_selector                                           */
    /* ---------------------------------------------------------------- */

    /* First match in document order */
    ST_ASSERT(dom_query_selector(doc->root, "p") == p1);
    ST_ASSERT(dom_query_selector(doc->root, "h1") == h1);
    ST_ASSERT(dom_query_selector(doc->root, ".highlight") == p2);
    ST_ASSERT(dom_query_selector(doc->root, "#main") == divmain);
    ST_ASSERT(dom_query_selector(doc->root, "[href]") == anchor);
    ST_ASSERT(dom_query_selector(doc->root, "section") == NULL);

    /* Context-rooted: search within body only */
    ST_ASSERT(dom_query_selector(body, "p") == p1);
    ST_ASSERT(dom_query_selector(footer, "p") == NULL);
    ST_ASSERT(dom_query_selector(footer, "a") == anchor);

    /* ---------------------------------------------------------------- */
    /*  7. dom_query_selector_all                                       */
    /* ---------------------------------------------------------------- */

    struct dom_node *results[32];
    int n;

    n = dom_query_selector_all(doc->root, "p", results, 32);
    ST_ASSERT(n == 2);
    ST_ASSERT(results[0] == p1);
    ST_ASSERT(results[1] == p2);

    n = dom_query_selector_all(doc->root, ".text", results, 32);
    ST_ASSERT(n == 2);
    ST_ASSERT(results[0] == p1);
    ST_ASSERT(results[1] == p2);

    n = dom_query_selector_all(doc->root, "h1, p", results, 32);
    ST_ASSERT(n == 3);   /* h1, p1, p2 */

    n = dom_query_selector_all(doc->root, "div *", results, 32);
    /* h1, p1, p2 (all descendants of div, but also descendants of those --
       h1/p1/p2 are direct children; their text children are TEXT not ELEMENT
       so they won't match *) */
    ST_ASSERT(n == 3);

    n = dom_query_selector_all(doc->root, "footer > *", results, 32);
    ST_ASSERT(n == 2);   /* anchor, span */

    n = dom_query_selector_all(doc->root, "[href]", results, 32);
    ST_ASSERT(n == 1);
    ST_ASSERT(results[0] == anchor);

    /* max cap test */
    n = dom_query_selector_all(doc->root, "p", results, 1);
    ST_ASSERT(n == 1);
    ST_ASSERT(results[0] == p1);

    /* no match */
    n = dom_query_selector_all(doc->root, "section", results, 32);
    ST_ASSERT(n == 0);

    /* ---------------------------------------------------------------- */
    /*  8. Malformed / edge cases                                       */
    /* ---------------------------------------------------------------- */

    ST_ASSERT(dom_selector_match(h1, "")           < 0);
    ST_ASSERT(dom_selector_match(h1, "[unclosed")  < 0);
    ST_ASSERT(dom_selector_match(h1, "#")          < 0);
    ST_ASSERT(dom_selector_match(h1, ".")          < 0);

    /* ---------------------------------------------------------------- */
    /*  Cleanup                                                         */
    /* ---------------------------------------------------------------- */
    dom_document_free(doc);

    return (selftest_failures == 0) ? 0 : -selftest_first_fail;
}

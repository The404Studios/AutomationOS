/*
 * ide_astprint.c -- AST printer + span-based text editing helpers.
 *
 * Freestanding C: no libc, no malloc, no stdio. Own static str helpers,
 * static scratch buffers, bounded everything. See ide_astprint.h.
 *
 * `ast_kind_name` is declared extern in ide_ast.h and resolved at link
 * time against ide_ast.c -- intentionally unresolved in a compile-only
 * check of this TU.
 */
#include "ide_astprint.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                           */
/* ------------------------------------------------------------------ */
#define AP_MAX_DEPTH 64   /* recursion / indent cap                   */
#define AP_MAX_LINES 4096 /* total lines emitted by astprint_tree     */
#define AP_MAX_PARAMS 64  /* params walked in a signature             */

/* ------------------------------------------------------------------ */
/* Tiny freestanding string helpers (file-local)                      */
/* ------------------------------------------------------------------ */

/* strlen, but never walks past a sane bound and is NULL-safe. */
static int ap_slen(const char* s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* Append at most (cap-1 - *pos) bytes of NUL-terminated `s` to out at *pos,
 * always keeping a NUL terminator. Updates *pos. Stops cleanly at the cap.
 * Returns 1 if the whole string fit, 0 if it was truncated. */
static int ap_puts(char* out, int* pos, int cap, const char* s)
{
    int i;
    if (!out || !pos || cap < 1)
        return 0;
    if (!s)
        return 1;
    for (i = 0; s[i] != '\0'; i++) {
        if (*pos >= cap - 1) {     /* leave room for the NUL */
            out[cap - 1] = '\0';
            return 0;
        }
        out[*pos] = s[i];
        (*pos)++;
    }
    out[*pos] = '\0';
    return 1;
}

/* Append a single char with the same cap discipline as ap_puts. */
static int ap_putc(char* out, int* pos, int cap, char c)
{
    if (!out || !pos || cap < 1)
        return 0;
    if (*pos >= cap - 1) {
        out[cap - 1] = '\0';
        return 0;
    }
    out[*pos] = c;
    (*pos)++;
    out[*pos] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Text splicing (minimal-diff insert)                                */
/* ------------------------------------------------------------------ */

/* Right-to-left copy so overlapping ranges shift correctly (own memmove). */
static void ap_shift_right(char* base, int from, int count, int delta)
{
    int i;
    /* moving [from, from+count) up by delta: copy tail-first */
    for (i = count - 1; i >= 0; i--)
        base[from + i + delta] = base[from + i];
}

int text_splice_n(char* buf, int* len, int cap, int at_off,
                  const char* ins, int ins_len)
{
    int cur, tail;

    if (!buf || !len || cap < 0 || ins_len < 0)
        return 0;
    if (ins_len > 0 && !ins)
        return 0;

    cur = *len;
    if (cur < 0)
        return 0;
    if (at_off < 0 || at_off > cur)
        return 0;

    /* Need room for the existing bytes + insertion + a NUL terminator. */
    if (cur + ins_len + 1 > cap)
        return 0;
    if (ins_len == 0)
        return 1; /* nothing to do; buffer unchanged */

    /* Shift the tail [at_off, cur) right by ins_len, then drop in `ins`. */
    tail = cur - at_off;
    ap_shift_right(buf, at_off, tail, ins_len);
    for (int i = 0; i < ins_len; i++)
        buf[at_off + i] = ins[i];

    *len = cur + ins_len;
    buf[*len] = '\0';
    return 1;
}

int text_splice(char* buf, int* len, int cap, int at_off, const char* ins)
{
    return text_splice_n(buf, len, cap, at_off, ins, ap_slen(ins));
}

/* ------------------------------------------------------------------ */
/* AST tree printer                                                   */
/* ------------------------------------------------------------------ */

/* Emit one node's line (no children). Returns 1 if it fully fit. */
static int ap_emit_node(const AstNode* n, int depth, char* out, int* pos,
                        int cap)
{
    int d, ok = 1;

    for (d = 0; d < depth && d < AP_MAX_DEPTH; d++)
        ok &= ap_puts(out, pos, cap, "  ");

    ok &= ap_puts(out, pos, cap, ast_kind_name(n->kind));

    if (n->name[0] != '\0') {
        ok &= ap_puts(out, pos, cap, " '");
        ok &= ap_puts(out, pos, cap, n->name);
        ok &= ap_putc(out, pos, cap, '\'');
    }
    if (n->type_str[0] != '\0') {
        ok &= ap_puts(out, pos, cap, " : ");
        ok &= ap_puts(out, pos, cap, n->type_str);
    }
    ok &= ap_putc(out, pos, cap, '\n');
    return ok;
}

/* Recursive walk. *lines counts emitted lines (global line cap).
 * Returns 0 once we are out of room/lines so callers can stop early. */
static int ap_walk(const AstNode* n, int depth, char* out, int* pos, int cap,
                   int* lines)
{
    const AstNode* c;
    int seen;

    if (!n)
        return 1;
    if (depth > AP_MAX_DEPTH)
        return 1; /* depth-capped: silently prune */
    if (*lines >= AP_MAX_LINES)
        return 0;
    if (*pos >= cap - 1)
        return 0;

    if (!ap_emit_node(n, depth, out, pos, cap))
        return 0; /* truncated mid-line: stop */
    (*lines)++;

    /* Walk children, but never trust links past nchildren. */
    seen = 0;
    for (c = n->first_child; c != 0 && seen < n->nchildren; c = c->next) {
        seen++;
        if (!ap_walk(c, depth + 1, out, pos, cap, lines))
            return 0;
    }
    return 1;
}

int astprint_tree(const AstNode* root, char* out, int cap)
{
    int pos = 0, lines = 0;

    if (!root || !out || cap < 1)
        return 0;
    out[0] = '\0';
    ap_walk(root, 0, out, &pos, cap, &lines);
    return pos;
}

/* ------------------------------------------------------------------ */
/* Function signature renderer                                        */
/* ------------------------------------------------------------------ */

int astprint_signature(const AstNode* funcdef, char* out, int cap)
{
    int pos = 0, np, seen;
    const AstNode* p;

    if (!funcdef || !out || cap < 1)
        return 0;
    out[0] = '\0';

    /* return type */
    if (funcdef->type_str[0] != '\0') {
        ap_puts(out, &pos, cap, funcdef->type_str);
        ap_putc(out, &pos, cap, ' ');
    }
    /* function name */
    if (funcdef->name[0] != '\0')
        ap_puts(out, &pos, cap, funcdef->name);

    ap_putc(out, &pos, cap, '(');

    np = 0;
    seen = 0;
    for (p = funcdef->first_child;
         p != 0 && seen < funcdef->nchildren && np < AP_MAX_PARAMS;
         p = p->next) {
        seen++;
        if (p->kind != AST_PARAM)
            continue; /* skip the body / non-param children */

        if (np > 0)
            ap_puts(out, &pos, cap, ", ");

        if (p->type_str[0] != '\0') {
            ap_puts(out, &pos, cap, p->type_str);
            if (p->name[0] != '\0')
                ap_putc(out, &pos, cap, ' ');
        }
        if (p->name[0] != '\0')
            ap_puts(out, &pos, cap, p->name);
        np++;
    }

    if (np == 0)
        ap_puts(out, &pos, cap, "void");

    ap_putc(out, &pos, cap, ')');
    return pos;
}

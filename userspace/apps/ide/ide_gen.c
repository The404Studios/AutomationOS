/*
 * ide_gen.c -- the CODE-GENERATION module ("blueprint -> code").
 *
 * When the user APPLYs a recommended ACTION (e.g. "Add claim_slot() wrapper",
 * "Add cooldown_gate"), this turns that intent into source. The edits are REAL
 * minimal diffs driven by the AST's source spans, never a naive append:
 *
 *   EDIT 1  insert a call to the new helper at the TOP of the focused
 *           function's body, at the exact byte offset of its '{' (+1), so the
 *           next analyze immediately sees the new call.
 *   EDIT 2  append the helper stub at end-of-file.
 *
 * Both edits go through text_splice(), the bounded insert-only primitive, so
 * every surrounding byte -- comments, whitespace, code -- survives untouched
 * (round-trip fidelity). The missing gate/lifecycle becomes present and
 * coherence rises.
 *
 * Freestanding C: no libc/malloc/stdio. All scratch is STATIC. Every helper is
 * `static` and `gen_`-prefixed. All buffer math is bounded by IDE_SRC_CAP.
 */
#include "ide.h"
#include "ide_ast.h"
#include "ide_astprint.h"
#include "ide_library.h"

/* ---- tiny freestanding string helpers (no libc) ----------------------- */

/* copy s into d (cap bytes incl NUL); returns bytes written (excl NUL). */
static int gen_strcpy(char* d, const char* s, int cap) {
    int n = 0;
    if (!d || cap <= 0) return 0;
    while (s && s[n] && n < cap - 1) { d[n] = s[n]; n++; }
    d[n] = 0;
    return n;
}

/* append s onto d starting at *pos (cap = full buffer size); clamps + NULs. */
static void gen_append(char* d, int* pos, int cap, const char* s) {
    int i = 0;
    if (!d || !pos || cap <= 0) return;
    while (s && s[i] && *pos < cap - 1) { d[*pos] = s[i]; (*pos)++; i++; }
    if (*pos < cap) d[*pos] = 0;
}

static char gen_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* case-insensitive: does haystack contain needle? */
static int gen_icontains(const char* hay, const char* needle) {
    int i, j;
    if (!hay || !needle || !needle[0]) return 0;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            if (!hay[i + j]) break;
            if (gen_lower(hay[i + j]) != gen_lower(needle[j])) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

/* Sanitize a title into a valid C identifier into out[cap]. Lowercases, maps
 * runs of non-[A-Za-z0-9_] to a single '_', ensures a leading letter, trims a
 * trailing underscore. Returns length. */
static int gen_sanitize_ident(const char* title, char* out, int cap) {
    int n = 0, prev_us = 1; /* prev_us=1 => suppress a leading '_' */
    if (!out || cap <= 0) return 0;
    for (int i = 0; title && title[i] && n < cap - 1; i++) {
        char c = title[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9');
        if (ok) {
            char lc = gen_lower(c);
            /* a leading digit is not a valid identifier start */
            if (n == 0 && lc >= '0' && lc <= '9') {
                out[n++] = 'g';
                if (n < cap - 1) out[n++] = '_';
            }
            out[n++] = lc;
            prev_us = 0;
        } else if (!prev_us && n < cap - 1) {
            out[n++] = '_';
            prev_us = 1;
        }
    }
    while (n > 0 && out[n - 1] == '_') n--;   /* trim trailing '_' */
    if (n == 0) n = gen_strcpy(out, "gen_helper", cap);
    out[n] = 0;
    return n;
}

/* Walk fn's children, return the first AST_COMPOUND (the function body). */
static AstNode* gen_find_body(AstNode* fn) {
    AstNode* c;
    if (!fn) return 0;
    for (c = fn->first_child; c; c = c->next)
        if (c->kind == AST_COMPOUND) return c;
    return 0;
}

/* ---------------------------------------------------------------------- */

int gen_apply_action(Ide* a, int idx) {
    /* static scratch: keep everything off the (tiny) stack */
    static char name[M_NAME];
    static char call[M_NAME + 16];
    static char stub[512];

    if (!a) return 0;

    /* --- 1. bounds-check the action index and read its title ----------- */
    if (idx < 0 || idx >= a->model.nactions) return 0;
    const char* title    = a->model.actions[idx].title;
    const int   orig_len = a->src_len;

    /* --- 2. decide helper name / body / return type from the title ----- */
    const char* ret;
    const char* body;
    if (gen_icontains(title, "claim")) {
        gen_strcpy(name, "claim_slot", sizeof name);
        ret  = "int";
        body = "return find_free_bullet_slot();";
    } else if (gen_icontains(title, "cooldown")) {
        gen_strcpy(name, "cooldown_gate", sizeof name);
        ret  = "int";
        body = "return is_ready_to_fire(t);";
    } else if (gen_icontains(title, "release") || gen_icontains(title, "pool")) {
        gen_strcpy(name, "projectile_pool_release_dead", sizeof name);
        ret  = "void";
        body = "/* free dead entries */";
    } else if (gen_icontains(title, "cache")) {
        gen_strcpy(name, "cache_nearest_enemy", sizeof name);
        ret  = "int";
        body = "/* cache query */";
    } else {
        gen_sanitize_ident(title, name, sizeof name);
        ret  = "int";
        body = "return 0;";
    }

    /* --- 3. locate the focus function + its body via the AST ----------- */
    AstNode* fn   = 0;
    AstNode* fbody = 0;
    if (a->focus_func >= 0 && a->focus_func < a->model.nfuncs) {
        fn = ast_find_func(a->model.funcs[a->focus_func].name);
        fbody = gen_find_body(fn);
    }

    /* --- 4. EDIT 1: insert a call at the top of the focus body --------- */
    /* The body's '{' byte offset = fbody->span.start_off; insert just after. */
    if (fbody) {
        int cp = 0;
        gen_append(call, &cp, (int)sizeof call, "\n    ");
        gen_append(call, &cp, (int)sizeof call, name);
        gen_append(call, &cp, (int)sizeof call, "();");

        int at = fbody->span.start_off + 1;     /* right after the '{' */
        if (at >= 1 && at <= a->src_len && at < IDE_SRC_CAP)
            text_splice(a->src, &a->src_len, IDE_SRC_CAP, at, call);
        /* if it didn't fit, src_len is unchanged -- harmless */
    }

    /* --- 5. EDIT 2: append the helper stub at EOF ---------------------- */
    {
        int p = 0;
        gen_append(stub, &p, (int)sizeof stub,
                   "\n\n/* generated by IDE blueprint */\nstatic ");
        gen_append(stub, &p, (int)sizeof stub, ret);
        gen_append(stub, &p, (int)sizeof stub, " ");
        gen_append(stub, &p, (int)sizeof stub, name);
        /* IDE-FORGE-0 audit fix: a bare "(...)" parameter list is invalid C
         * -- the on-device compiler rejected the very stubs GENERATE wrote.
         * "(void)" matches the spliced no-arg call site and compiles. */
        gen_append(stub, &p, (int)sizeof stub, "(void) {\n    ");
        gen_append(stub, &p, (int)sizeof stub, body);
        gen_append(stub, &p, (int)sizeof stub, "\n}\n");

        int at = a->src_len;                     /* append */
        if (at >= 0 && at <= a->src_len && at < IDE_SRC_CAP)
            text_splice(a->src, &a->src_len, IDE_SRC_CAP, at, stub);
    }

    /* --- 6. did anything change? --------------------------------------- */
    if (a->src_len == orig_len) return 0;

    /* --- 7. persist (ignore failure on a read-only fs; keep in-memory) - */
    ide_write_file(a->cur_file, a->src, a->src_len);

    /* --- 8. re-parse + re-analyze so the new call shows up ------------- */
    model_parse(&a->model, a->src, a->src_len, a->cur_file);

    /* restore focus: clamp into range and mirror into the model */
    if (a->model.nfuncs <= 0) {
        a->focus_func   = 0;
        a->model.focus  = -1;
    } else {
        if (a->focus_func < 0) a->focus_func = 0;
        if (a->focus_func >= a->model.nfuncs)
            a->focus_func = a->model.nfuncs - 1;
        a->model.focus = a->focus_func;
    }

    model_analyze(&a->model);
    return 1;
}







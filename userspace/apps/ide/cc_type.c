/*
 * cc_type.c -- type size / pointer / element-size helpers and a rough
 *              best-effort expression type inferencer for the C-subset
 *              compiler backend (see cc.h for the v1 subset rules).
 *
 * Freestanding: no libc, no malloc, no stdio. All string handling is done
 * with the small static helpers below; outputs go into caller-provided,
 * bounded buffers and are always NUL-terminated.
 *
 * v1 type model:
 *   - char / signed char / unsigned char / _Bool / bool ............ 1 byte
 *   - everything else (int, long, pointers, typedef'd handles) ..... 8 bytes
 *   - a type string containing '*' is a pointer -> 8 bytes
 */
#include "cc.h"

/* ---------------------------------------------------------------------- *
 *  tiny freestanding string helpers (self-contained, file-local)
 * ---------------------------------------------------------------------- */

static int ct_len(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Copy up to cap-1 bytes of src into dst, always NUL-terminating. cap<=0 noop. */
static void ct_copy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) {
        while (i < cap - 1 && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* Does s contain ch? */
static int ct_has(const char* s, char ch)
{
    int i;
    if (!s) return 0;
    for (i = 0; s[i]; i++)
        if (s[i] == ch) return 1;
    return 0;
}

/* Compare against a NUL-terminated literal; 1 if exactly equal. */
static int ct_eq(const char* a, const char* b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* Skip leading spaces/tabs; return pointer into s. */
static const char* ct_skip_ws(const char* s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

/* Copy src into dst (cap), trimming leading and trailing whitespace. */
static void ct_copy_trim(char* dst, int cap, const char* src)
{
    int end;
    const char* p;
    if (!dst || cap <= 0) return;
    p = ct_skip_ws(src);
    ct_copy(dst, cap, p);
    /* trim trailing whitespace in dst */
    end = ct_len(dst);
    while (end > 0) {
        char c = dst[end - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            dst[--end] = '\0';
        else
            break;
    }
}

/* Append " *" to dst (within cap) iff there is room. */
static void ct_append_star(char* dst, int cap)
{
    int n = ct_len(dst);
    if (n + 2 <= cap - 1) {    /* need " *" + NUL */
        dst[n]     = ' ';
        dst[n + 1] = '*';
        dst[n + 2] = '\0';
    }
}

/* ---------------------------------------------------------------------- *
 *  struct registry -- maps a struct/union tag to its laid-out fields.
 *
 *  Built by scanning ast_root() for AST_RECORD nodes (and typedef'd records).
 *  v1 layout: fields are laid out SEQUENTIALLY; each field consumes
 *  cc_sizeof_type(field.type) bytes (char/_Bool=1, everything else incl.
 *  pointers=8). Non-char fields are 8-byte aligned (sequential char runs
 *  are packed). Unions are handled as offset-0 for every field.
 * ---------------------------------------------------------------------- */

#define CT_MAX_STRUCTS 32
#define CT_MAX_RFIELDS 32

typedef struct {
    char name[CC_NAME];     /* field name                               */
    int  offset;            /* byte offset within the struct            */
    int  size;             /* field size in bytes (1 or 8)             */
} CtRField;

typedef struct {
    char tag[CC_NAME];      /* struct/union tag OR typedef name         */
    int  is_union;
    int  nfields;
    CtRField fields[CT_MAX_RFIELDS];
} CtStruct;

static CtStruct ct_structs[CT_MAX_STRUCTS];
static int      ct_nstructs;
static int      ct_built;            /* 1 once the registry is populated */

/* Strip a leading "struct "/"union "/"enum " keyword and any '*'/array/space
 * decoration, leaving the bare tag/typedef name in out[cap]. */
static void ct_struct_tag(const char* type_str, char* out, int cap)
{
    char buf[CC_NAME];
    const char* p;
    int i;

    if (!out || cap <= 0) return;
    out[0] = '\0';
    ct_copy_trim(buf, (int)sizeof(buf), type_str);
    p = buf;

    /* skip a leading record keyword if present */
    if (ct_len(p) > 7 && p[0]=='s'&&p[1]=='t'&&p[2]=='r'&&p[3]=='u'&&
        p[4]=='c'&&p[5]=='t'&&p[6]==' ') p += 7;
    else if (ct_len(p) > 6 && p[0]=='u'&&p[1]=='n'&&p[2]=='i'&&p[3]=='o'&&
             p[4]=='n'&&p[5]==' ') p += 6;
    else if (ct_len(p) > 5 && p[0]=='e'&&p[1]=='n'&&p[2]=='u'&&p[3]=='m'&&
             p[4]==' ') p += 5;

    /* copy the bare identifier, stopping at space/'*'/'['  */
    i = 0;
    while (*p && i < cap - 1 &&
           *p != ' ' && *p != '\t' && *p != '*' && *p != '[') {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

/* Find (or 0) a struct entry whose tag matches `tag`. */
static CtStruct* ct_find_struct(const char* tag)
{
    int i;
    if (!tag || !tag[0]) return 0;
    for (i = 0; i < ct_nstructs; i++)
        if (ct_eq(ct_structs[i].tag, tag))
            return &ct_structs[i];
    return 0;
}

/* Register one AST_RECORD's fields under `tag`. */
static void ct_register_record(const char* tag, const AstNode* rec, int is_union)
{
    CtStruct* s;
    const AstNode* f;
    int off = 0;

    if (!tag || !tag[0] || !rec) return;
    if (ct_find_struct(tag)) return;            /* already known */
    if (ct_nstructs >= CT_MAX_STRUCTS) return;

    s = &ct_structs[ct_nstructs++];
    ct_copy(s->tag, (int)sizeof(s->tag), tag);
    s->is_union = is_union;
    s->nfields  = 0;

    for (f = rec->first_child; f; f = f->next) {
        int fsz;
        if (f->kind != AST_FIELD) continue;
        if (!f->name[0]) continue;
        if (s->nfields >= CT_MAX_RFIELDS) break;
        fsz = cc_sizeof_type(f->type_str);
        if (fsz <= 0) fsz = 8;
        if (!is_union) {
            /* 8-align non-char fields for ABI-ish sequential layout */
            if (fsz != 1 && (off & 7)) off = (off + 7) & ~7;
        }
        ct_copy(s->fields[s->nfields].name, (int)sizeof(s->fields[0].name),
                f->name);
        s->fields[s->nfields].offset = is_union ? 0 : off;
        s->fields[s->nfields].size   = fsz;
        s->nfields++;
        if (!is_union) off += fsz;
    }
}

/* Walk the AST collecting every AST_RECORD (tagged) and typedef'd record. */
static void ct_scan(const AstNode* n, int depth)
{
    const AstNode* c;
    if (!n || depth > 64) return;

    if (n->kind == AST_RECORD) {
        /* tag stored in name; union-ness: a record with no enumerators that
         * the parser produced from "union" -- we can't always tell, default
         * struct. (Spans don't carry the keyword.) Treat as struct. */
        if (n->name[0])
            ct_register_record(n->name, n, 0);
    } else if (n->kind == AST_TYPEDEF) {
        /* typedef struct {..} Name;  -> the record body is a child, and the
         * typedef name is in n->name. Also map "struct Tag" typedefs. */
        char tag[CC_NAME];
        const AstNode* rec = 0;
        for (c = n->first_child; c; c = c->next) {
            if (c->kind == AST_RECORD) { rec = c; break; }
        }
        if (rec && n->name[0]) {
            ct_register_record(n->name, rec, 0);
            /* also register under the record's own tag if it has one */
            if (rec->name[0])
                ct_register_record(rec->name, rec, 0);
        }
        /* typedef <existing struct type> Name; : alias the field set */
        if (!rec && n->name[0] && n->type_str[0]) {
            ct_struct_tag(n->type_str, tag, (int)sizeof(tag));
            {
                CtStruct* base = ct_find_struct(tag);
                if (base && !ct_find_struct(n->name) &&
                    ct_nstructs < CT_MAX_STRUCTS) {
                    ct_structs[ct_nstructs] = *base;
                    ct_copy(ct_structs[ct_nstructs].tag,
                            (int)sizeof(ct_structs[0].tag), n->name);
                    ct_nstructs++;
                }
            }
        }
    }

    for (c = n->first_child; c; c = c->next)
        ct_scan(c, depth + 1);
}

void cc_build_struct_registry(void)
{
    ct_nstructs = 0;
    ct_built    = 1;
    ct_scan(ast_root(), 0);
}

int cc_member_offset(const char* struct_type, const char* field,
                     int* field_size_out)
{
    char tag[CC_NAME];
    CtStruct* s;
    int i;

    if (field_size_out) *field_size_out = 8;     /* safe fallback */
    if (!ct_built) cc_build_struct_registry();   /* lazy build */

    if (!struct_type || !field) return 0;

    ct_struct_tag(struct_type, tag, (int)sizeof(tag));
    s = ct_find_struct(tag);
    if (!s) return 0;                            /* unknown -> offset 0, size 8 */

    for (i = 0; i < s->nfields; i++) {
        if (ct_eq(s->fields[i].name, field)) {
            if (field_size_out) *field_size_out = s->fields[i].size;
            return s->fields[i].offset;
        }
    }
    return 0;                                    /* unknown field */
}

/* ---------------------------------------------------------------------- *
 *  AST-based variable-type recovery
 *
 *  CcLocal/CcGlobal don't always carry a usable struct type string (locals
 *  store only a byte size). To infer the struct type of `s` in `s.field`, we
 *  search the AST for the declaration (PARAM/VAR_DECL/FIELD) whose name
 *  matches and return its rendered type_str. Best-effort, depth-bounded.
 * ---------------------------------------------------------------------- */
static const AstNode* ct_find_decl(const AstNode* n, const char* name, int depth)
{
    const AstNode* c;
    if (!n || depth > 64 || !name || !name[0]) return 0;

    if ((n->kind == AST_VAR_DECL || n->kind == AST_PARAM) &&
        n->name[0] && ct_eq(n->name, name) && n->type_str[0])
        return n;

    for (c = n->first_child; c; c = c->next) {
        const AstNode* r = ct_find_decl(c, name, depth + 1);
        if (r) return r;
    }
    return 0;
}

/* ---------------------------------------------------------------------- *
 *  public: size / pointer / element-size
 * ---------------------------------------------------------------------- */

int cc_is_pointer(const char* type_str)
{
    return ct_has(type_str, '*') ? 1 : 0;
}

int cc_sizeof_type(const char* type_str)
{
    /* Work on a trimmed copy so leading/trailing spaces don't fool us. */
    char buf[CC_NAME];
    if (!type_str) return 8;

    /* Any '*' => pointer => 8. */
    if (ct_has(type_str, '*')) return 8;

    ct_copy_trim(buf, (int)sizeof(buf), type_str);

    if (ct_eq(buf, "char")        ||
        ct_eq(buf, "signed char") ||
        ct_eq(buf, "unsigned char") ||
        ct_eq(buf, "_Bool")       ||
        ct_eq(buf, "bool"))
        return 1;

    /* v1: int / long / unsigned / typedef'd handles / etc. all 8 bytes. */
    return 8;
}

/*
 * Size of what a pointer/array element is, for pointer-arithmetic scaling.
 * Strip ONE trailing '*' (or a trailing "[]"/"[N]") then cc_sizeof_type the
 * remainder. If the type is neither a pointer nor an array, there is nothing
 * to scale by -> return 1.
 */
int cc_elem_size(const char* type_str)
{
    char buf[CC_NAME];
    int  n;

    if (!type_str) return 1;

    /* Trim outer whitespace into a working buffer. */
    ct_copy_trim(buf, (int)sizeof(buf), type_str);
    n = ct_len(buf);
    if (n == 0) return 1;

    /* Case 1: trailing array brackets "...[..]" -> strip from the '['. */
    if (buf[n - 1] == ']') {
        int i = n - 1;
        while (i > 0 && buf[i] != '[')
            i--;
        if (buf[i] == '[') {
            buf[i] = '\0';
            /* element is the remainder (e.g. "int [4]" -> "int"). */
            return cc_sizeof_type(buf);
        }
        /* malformed; fall through */
    }

    /* Case 2: strip ONE trailing '*' (the rightmost). */
    {
        int i = n - 1;
        /* find rightmost non-space */
        while (i >= 0 && (buf[i] == ' ' || buf[i] == '\t'))
            i--;
        if (i >= 0 && buf[i] == '*') {
            buf[i] = '\0';               /* drop that one star */
            return cc_sizeof_type(buf);  /* size of remainder  */
        }
    }

    /* Not a pointer/array: no scaling. */
    return 1;
}

/* ---------------------------------------------------------------------- *
 *  strip helpers used by inference (operate in-place on a buffer)
 * ---------------------------------------------------------------------- */

/* Remove ONE trailing '*' from buf (ignoring trailing spaces); 1 if removed. */
static int ct_strip_one_star(char* buf)
{
    int i = ct_len(buf) - 1;
    while (i >= 0 && (buf[i] == ' ' || buf[i] == '\t'))
        buf[i--] = '\0';            /* trim trailing space while at it */
    if (i >= 0 && buf[i] == '*') {
        buf[i] = '\0';
        /* trim any space now exposed at the tail */
        i--;
        while (i >= 0 && (buf[i] == ' ' || buf[i] == '\t'))
            buf[i--] = '\0';
        return 1;
    }
    return 0;
}

/* Remove ONE trailing '*' OR a trailing "[...]" (element type). */
static void ct_strip_one_indir(char* buf)
{
    int n = ct_len(buf);
    if (n == 0) return;
    if (buf[n - 1] == ']') {
        int i = n - 1;
        while (i > 0 && buf[i] != '[')
            i--;
        if (buf[i] == '[') {
            buf[i] = '\0';
            /* trim trailing space */
            i--;
            while (i >= 0 && (buf[i] == ' ' || buf[i] == '\t'))
                buf[i--] = '\0';
            return;
        }
    }
    ct_strip_one_star(buf);
}

/* ---------------------------------------------------------------------- *
 *  public: rough best-effort expression type inference
 * ---------------------------------------------------------------------- */

#define CT_MAX_DEPTH 16

static void ct_infer(Cg* g, const AstNode* e, char* out, int cap, int depth);

void cc_infer_type(Cg* g, const AstNode* e, char* out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = '\0';
    ct_infer(g, e, out, cap, 0);
}

static void ct_infer(Cg* g, const AstNode* e, char* out, int cap, int depth)
{
    if (cap <= 0) return;
    out[0] = '\0';

    /* Bail out safely on missing node or runaway recursion. */
    if (!e || depth >= CT_MAX_DEPTH) {
        ct_copy(out, cap, "int");
        return;
    }

    switch (e->kind) {

    case AST_IDENT: {
        CcLocal*  loc = cc_find_local(g, e->name);
        CcGlobal* glb;
        if (loc) {
            /* CcLocal stores no type string. Recover the real declared type by
             * locating this name's PARAM/VAR_DECL in the AST -- needed so that
             * struct lvalues (`s.field`) and pointers (`p->field`) keep their
             * tag for member-offset lookup. Fall back to the coarse size
             * bucket (1 => char, else int) when no decl is found. */
            const AstNode* decl = ct_find_decl(ast_root(), e->name, 0);
            if (decl && decl->type_str[0]) {
                ct_copy(out, cap, decl->type_str);
                return;
            }
            ct_copy(out, cap, (loc->size == 1) ? "char" : "int");
            return;
        }
        glb = cc_find_global(g, e->name);
        if (glb) {
            ct_copy(out, cap, glb->type);
            if (out[0] == '\0')
                ct_copy(out, cap, "int");
            return;
        }
        /* Not a known local/global: still try the AST (e.g. inner scopes). */
        {
            const AstNode* decl = ct_find_decl(ast_root(), e->name, 0);
            if (decl && decl->type_str[0]) {
                ct_copy(out, cap, decl->type_str);
                return;
            }
        }
        ct_copy(out, cap, "int");
        return;
    }

    case AST_LITERAL: {
        const char* t = e->name;
        if (t && t[0] == '"')        ct_copy(out, cap, "char *");
        else if (t && t[0] == '\'')  ct_copy(out, cap, "char");
        else                         ct_copy(out, cap, "int");
        return;
    }

    case AST_UNARY: {
        const char* op = e->name;
        if (op && op[0] == '*' && op[1] == '\0') {
            /* dereference: child type minus one level of indirection. */
            ct_infer(g, e->first_child, out, cap, depth + 1);
            ct_strip_one_indir(out);
            if (out[0] == '\0') ct_copy(out, cap, "int");
            return;
        }
        if (op && op[0] == '&' && op[1] == '\0') {
            /* address-of: child type plus one '*'. */
            ct_infer(g, e->first_child, out, cap, depth + 1);
            if (out[0] == '\0') ct_copy(out, cap, "int");
            ct_append_star(out, cap);
            return;
        }
        /* other unary (-, !, ~, ++/--): result type ~ operand (int-ish). */
        ct_infer(g, e->first_child, out, cap, depth + 1);
        if (out[0] == '\0') ct_copy(out, cap, "int");
        return;
    }

    case AST_INDEX: {
        /* a[b] : element type of a's (pointer/array) type. */
        ct_infer(g, e->first_child, out, cap, depth + 1);
        ct_strip_one_indir(out);
        if (out[0] == '\0') ct_copy(out, cap, "int");
        return;
    }

    case AST_BINARY: {
        /* If either operand is a pointer, the binary result is that pointer
         * type (pointer + integer arithmetic). Otherwise int. */
        char lhs[CC_NAME];
        char rhs[CC_NAME];
        ct_infer(g, e->first_child, lhs, (int)sizeof(lhs), depth + 1);
        if (cc_is_pointer(lhs)) {
            ct_copy(out, cap, lhs);
            return;
        }
        if (e->first_child) {
            const AstNode* rc = e->first_child->next;
            ct_infer(g, rc, rhs, (int)sizeof(rhs), depth + 1);
            if (cc_is_pointer(rhs)) {
                ct_copy(out, cap, rhs);
                return;
            }
        }
        ct_copy(out, cap, "int");
        return;
    }

    case AST_MEMBER: {
        /* type of `base.member` / `base->member` = the field's declared type.
         * Recover the base's struct type, look the field up in the registry,
         * and report the field's type so loads/stores pick byte vs qword. */
        const AstNode* base = e->first_child;
        char bty[CC_NAME];
        char tag[CC_NAME];
        CtStruct* s;
        int i;

        ct_infer(g, base, bty, (int)sizeof(bty), depth + 1);
        ct_strip_one_star(bty);                  /* `->` : peel the pointer */
        ct_struct_tag(bty, tag, (int)sizeof(tag));

        if (!ct_built) cc_build_struct_registry();
        s = ct_find_struct(tag);
        if (s) {
            for (i = 0; i < s->nfields; i++) {
                if (ct_eq(s->fields[i].name, e->name)) {
                    ct_copy(out, cap, (s->fields[i].size == 1) ? "char" : "int");
                    return;
                }
            }
        }
        ct_copy(out, cap, "int");
        return;
    }

    case AST_CALL:
    default:
        /* good enough for v1 */
        ct_copy(out, cap, "int");
        return;
    }
}

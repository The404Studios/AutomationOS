/*
 * ide_parse.c -- AST-based semantic model builder for the IDE.
 *
 * Implements model_parse(): tokenize one C source buffer, parse it into a real
 * C AST (ide_pcore/pdecl/pstmt/pexpr.c), then WALK that tree to fill the Model:
 *   - top-level AST_VAR_DECL  -> Global
 *   - top-level AST_FUNC_DEF  -> Func (+ params, calls, global reads/writes)
 *
 * This replaces the old regex/token-heuristic model_parse with an accurate
 * tree walk. Freestanding: no libc, no malloc, no stdio. Static buffers only.
 * All helpers are static and prefixed mp_.
 */
#include "ide_model.h"
#include "ide_ast.h"
#include "ide_parser.h"
#include "ide_lex.h"
#include "ide_sys.h"

/* ---- static, bounded parse state (no malloc) ---- */
static Tok    toks[PARSE_MAX_TOKS];
static Parser P;

/* recursion guard so a pathological / corrupt tree can't blow the stack */
#define MP_MAX_DEPTH 256

/* ----------------------------------------------------------------------- */
/* per-function "seen" sets: ensures a Global's reader/writer fan-in count  */
/* is bumped at most ONCE per function.                                     */
/* ----------------------------------------------------------------------- */
typedef struct {
    char r[M_MAXREFS][M_NAME]; int nr;
    char w[M_MAXREFS][M_NAME]; int nw;
} SeenSet;

/* ----------------------------------------------------------------------- */
/* small list helpers                                                      */
/* ----------------------------------------------------------------------- */

/* does an M_NAME-name list already contain name?  (dedupe) */
static int mp_list_has(char list[][M_NAME], int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (ide_streq(list[i], name)) return 1;
    return 0;
}

/* append name to a capped M_NAME list if absent; returns 1 if newly added */
static int mp_list_add(char list[][M_NAME], int* n, int cap, const char* name) {
    if (!name || !name[0]) return 0;
    if (mp_list_has(list, *n, name)) return 0;
    if (*n >= cap) return 0;
    ide_strlcpy(list[*n], name, M_NAME);
    (*n)++;
    return 1;
}

static int mp_global_index(Model* m, const char* name) {
    for (int i = 0; i < m->nglobals; i++)
        if (ide_streq(m->globals[i].name, name)) return i;
    return -1;
}

/* ----------------------------------------------------------------------- */
/* the read/write classifier: a recursive AST walk that marks IDENT nodes   */
/* as global reads or writes depending on lvalue context.                   */
/* ----------------------------------------------------------------------- */
static void mp_classify(Model* m, Func* f, SeenSet* seen,
                        AstNode* n, int writing, int depth) {
    if (!n || depth > MP_MAX_DEPTH) return;

    switch (n->kind) {
    case AST_ASSIGN: {
        /* a = b / a += b : first child is the lvalue (written), rest read. */
        AstNode* c = n->first_child;
        if (c) {
            mp_classify(m, f, seen, c, 1, depth + 1);
            for (AstNode* k = c->next; k; k = k->next)
                mp_classify(m, f, seen, k, 0, depth + 1);
        }
        return;
    }
    case AST_UNARY: {
        /* ++/-- (pre or post) write their operand; everything else reads. */
        int isinc = ide_streq(n->name, "++")     || ide_streq(n->name, "--") ||
                    ide_streq(n->name, "post++") || ide_streq(n->name, "post--");
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, isinc ? writing : 0, depth + 1);
        return;
    }
    case AST_INDEX:
    case AST_MEMBER: {
        /* base keeps the writing context; any index/remaining are reads.   */
        AstNode* c = n->first_child;
        if (c) {
            mp_classify(m, f, seen, c, writing, depth + 1);
            for (AstNode* k = c->next; k; k = k->next)
                mp_classify(m, f, seen, k, 0, depth + 1);
        }
        return;
    }
    case AST_IDENT: {
        /* a known global reference: record read or write on the func, and
         * bump the global's fan-in at most once per function. */
        const char* nm = n->name;
        int gx = mp_global_index(m, nm);
        if (gx >= 0) {
            if (writing) {
                mp_list_add(f->writes, &f->nwrites, M_MAXREFS, nm);
                if (mp_list_add(seen->w, &seen->nw, M_MAXREFS, nm))
                    m->globals[gx].nwriters++;
            } else {
                mp_list_add(f->reads, &f->nreads, M_MAXREFS, nm);
                if (mp_list_add(seen->r, &seen->nr, M_MAXREFS, nm))
                    m->globals[gx].nreaders++;
            }
        }
        /* an identifier may still have children (rare); treat as reads. */
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, 0, depth + 1);
        return;
    }
    default:
        /* generic node: descend, no write context propagated. */
        for (AstNode* c = n->first_child; c; c = c->next)
            mp_classify(m, f, seen, c, 0, depth + 1);
        return;
    }
}

/* ----------------------------------------------------------------------- */
/* deep walk of a function body to collect CALLS (the classifier above      */
/* handles reads/writes separately so the two concerns stay clean).         */
/* ----------------------------------------------------------------------- */
static void mp_collect_calls(Func* f, AstNode* n, int depth) {
    if (!n || depth > MP_MAX_DEPTH) return;

    if (n->kind == AST_CALL) {
        const char* nm = n->name;
        /* fall back to a simple callee identifier if name is empty */
        if ((!nm || !nm[0]) && n->first_child &&
            n->first_child->kind == AST_IDENT)
            nm = n->first_child->name;
        if (nm && nm[0])
            mp_list_add(f->calls, &f->ncalls, M_MAXCALLS, nm);
    }

    for (AstNode* c = n->first_child; c; c = c->next)
        mp_collect_calls(f, c, depth + 1);
}

/* ----------------------------------------------------------------------- */
/* PASS 0: pre-parse scan of preprocessor tokens for #include and #define  */
/* (called before parser_init strips them).                                */
/* ----------------------------------------------------------------------- */
static int mp_startswith(const char* s, int len, const char* prefix) {
    int i = 0;
    if (!s || !prefix) return 0;
    while (prefix[i]) {
        if (i >= len) return 0;
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static void mp_pass_preproc(Model* m, const char* src, int src_len) {
    /* Scan each line looking for #include and #define directives. */
    int pos = 0, line = 1;
    while (pos < src_len) {
        /* skip leading whitespace on the line */
        while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t')) pos++;

        if (pos < src_len && src[pos] == '#') {
            pos++; /* skip '#' */
            /* skip spaces after '#' */
            while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t')) pos++;

            int remaining = src_len - pos;

            if (mp_startswith(src + pos, remaining, "include")) {
                /* #include directive */
                pos += 7; /* skip "include" */
                while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
                if (m->nincludes < M_MAXINCLUDES) {
                    Include* inc = &m->includes[m->nincludes];
                    inc->line = line;
                    inc->path[0] = '\0';
                    /* extract the path: "foo.h" or <foo.h> */
                    if (pos < src_len && (src[pos] == '"' || src[pos] == '<')) {
                        char close = (src[pos] == '"') ? '"' : '>';
                        int pstart = pos;
                        pos++;
                        while (pos < src_len && src[pos] != close && src[pos] != '\n') pos++;
                        if (pos < src_len && src[pos] == close) pos++;
                        int plen = pos - pstart;
                        if (plen > M_NAME - 1) plen = M_NAME - 1;
                        for (int i = 0; i < plen; i++) inc->path[i] = src[pstart + i];
                        inc->path[plen] = '\0';
                    }
                    m->nincludes++;
                }
            } else if (mp_startswith(src + pos, remaining, "define")) {
                /* #define directive */
                pos += 6; /* skip "define" */
                while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
                if (m->nmacros < M_MAXMACROS) {
                    Macro* mac = &m->macros[m->nmacros];
                    mac->line = line;
                    mac->name[0] = '\0';
                    /* extract macro name (identifier chars) */
                    int nstart = pos;
                    while (pos < src_len && ((src[pos] >= 'a' && src[pos] <= 'z') ||
                           (src[pos] >= 'A' && src[pos] <= 'Z') ||
                           (src[pos] >= '0' && src[pos] <= '9') ||
                           src[pos] == '_')) pos++;
                    int nlen = pos - nstart;
                    if (nlen > 0) {
                        if (nlen > M_NAME - 1) nlen = M_NAME - 1;
                        for (int i = 0; i < nlen; i++) mac->name[i] = src[nstart + i];
                        mac->name[nlen] = '\0';
                        m->nmacros++;
                    }
                }
            }
        }

        /* advance to end of line */
        while (pos < src_len && src[pos] != '\n') pos++;
        if (pos < src_len) pos++; /* skip '\n' */
        line++;
    }
}

/* ----------------------------------------------------------------------- */
/* PASS 1: top-level globals                                               */
/* ----------------------------------------------------------------------- */
static void mp_pass_globals(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind != AST_VAR_DECL) continue;
        if (m->nglobals >= M_MAXGLOBALS) break;
        if (!c->name[0]) continue;
        if (mp_global_index(m, c->name) >= 0) continue;   /* dedupe by name */

        Global* g = &m->globals[m->nglobals];
        ide_strlcpy(g->name, c->name, M_NAME);
        ide_strlcpy(g->type, c->type_str, M_TYPE);
        ide_strlcpy(g->file, m->cur_file, M_NAME);
        g->nreaders = 0;
        g->nwriters = 0;
        m->nglobals++;
    }
}

/* ----------------------------------------------------------------------- */
/* PASS 1b: top-level records (struct/union/enum) and typedefs             */
/* ----------------------------------------------------------------------- */
static void mp_pass_records(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind == AST_RECORD) {
            if (m->nrecords >= M_MAXRECORDS) continue;
            Record* r = &m->records[m->nrecords];
            ide_strlcpy(r->name, c->name, M_NAME);
            /* figure out the kind tag from the type_str or by context */
            r->kind_tag[0] = '\0';
            if (c->type_str[0])
                ide_strlcpy(r->kind_tag, c->type_str, 16);
            else
                ide_strlcpy(r->kind_tag, "struct", 16);
            r->line = c->span.start_line;
            r->nfields = c->nchildren;
            m->nrecords++;
        } else if (c->kind == AST_TYPEDEF) {
            if (m->nrecords >= M_MAXRECORDS) continue;
            Record* r = &m->records[m->nrecords];
            ide_strlcpy(r->name, c->name, M_NAME);
            ide_strlcpy(r->kind_tag, "typedef", 16);
            r->line = c->span.start_line;
            r->nfields = 0;
            m->nrecords++;
        }
    }
}

/* ----------------------------------------------------------------------- */
/* PASS 1c: top-level function prototypes                                  */
/* ----------------------------------------------------------------------- */
static void mp_pass_protos(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind != AST_FUNC_PROTO) continue;
        if (m->nprotos >= M_MAXPROTOS) break;
        if (!c->name[0]) continue;

        Proto* pr = &m->protos[m->nprotos];
        ide_strlcpy(pr->name, c->name, M_NAME);
        ide_strlcpy(pr->ret,  c->type_str, M_TYPE);
        pr->line = c->span.start_line;
        m->nprotos++;
    }
}

/* ----------------------------------------------------------------------- */
/* PASS 2: top-level function definitions                                  */
/* ----------------------------------------------------------------------- */
static void mp_pass_funcs(Model* m, AstNode* root) {
    for (AstNode* c = root->first_child; c; c = c->next) {
        if (c->kind != AST_FUNC_DEF) continue;
        if (m->nfuncs >= M_MAXFUNCS) break;

        Func* f = &m->funcs[m->nfuncs];

        /* header */
        ide_strlcpy(f->name, c->name, M_NAME);
        ide_strlcpy(f->ret,  c->type_str, M_TYPE);
        ide_strlcpy(f->file, m->cur_file, M_NAME);
        f->line_start = c->span.start_line;
        f->line_end   = c->span.end_line;

        f->nparams = 0;
        f->ncalls  = 0;
        f->nreads  = 0;
        f->nwrites = 0;
        f->nports  = 0;
        f->closed  = 0;

        /* params + locate the body among the children */
        AstNode* body = 0;
        for (AstNode* k = c->first_child; k; k = k->next) {
            if (k->kind == AST_PARAM) {
                if (f->nparams < M_MAXPARAMS) {
                    Param* p = &f->params[f->nparams];
                    ide_strlcpy(p->type, k->type_str, M_TYPE);
                    ide_strlcpy(p->name, k->name, M_NAME);
                    f->nparams++;
                }
            } else if (k->kind == AST_COMPOUND) {
                body = k;            /* the function body */
            }
        }

        /* deep walk the body: calls, then reads/writes of globals */
        if (body) {
            SeenSet seen;
            seen.nr = 0;
            seen.nw = 0;
            mp_collect_calls(f, body, 0);
            mp_classify(m, f, &seen, body, 0, 0);
        }

        m->nfuncs++;
    }
}

/* ======================================================================= */
/* ASM model builder: assembly files ALSO populate the Semantic LEGO Map.  */
/* Each code label (name:) becomes a node ("function"); `call`/`jmp`/jcc to */
/* a label is an outgoing edge -> the map renders the control-flow graph.   */
/* `global`/`extern` directives become protos; data labels (db/dd/...)      */
/* become globals. Supports both NASM (global foo) and GAS (.globl foo,     */
/* labels like .L1) syntax. Freestanding line scanner; no AST.              */
/* ======================================================================= */
static char mp_lower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int  mp_asm_ident(char c){
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='$'||c=='@';
}
static int mp_is_reg(const char* s){
    static const char* const regs[] = {
        "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
        "r8","r9","r10","r11","r12","r13","r14","r15",
        "eax","ebx","ecx","edx","esi","edi","ebp","esp",
        "ax","bx","cx","dx","al","bl","cl","dl","ah","bh","ch","dh", 0 };
    for (int i = 0; regs[i]; i++) if (ide_streq(s, regs[i])) return 1;
    return 0;
}
/* filename ends with .asm / .s / .S / .inc / .nasm (case-insensitive)? */
static int mp_filename_is_asm(const char* fn){
    if (!fn) return 0;
    int n = 0; while (fn[n]) n++;
    int dot = -1;
    for (int i = n - 1; i >= 0 && fn[i] != '/'; i--) if (fn[i] == '.') { dot = i; break; }
    if (dot < 0) return 0;
    char e[8]; int el = 0;
    for (int i = dot + 1; i < n && el < 7; i++) e[el++] = mp_lower(fn[i]);
    e[el] = 0;
    return ide_streq(e,"asm") || ide_streq(e,"s") || ide_streq(e,"inc") || ide_streq(e,"nasm");
}

static void mp_parse_asm(Model* m, const char* src, int len){
    int pos = 0, line = 1;
    Func* cur = 0;
    while (pos < len) {
        int le = pos; while (le < len && src[le] != '\n') le++;     /* line = [pos, le) */
        int i = pos; while (i < le && (src[i] == ' ' || src[i] == '\t')) i++;
        int lineend = le;
        for (int k = i; k < le; k++) if (src[k] == ';') { lineend = k; break; }  /* strip ; comment */

        /* label?  ident ':'  */
        if (i < lineend) {
            int j = i; while (j < lineend && mp_asm_ident(src[j])) j++;
            if (j > i && j < lineend && src[j] == ':') {
                char name[M_NAME]; int nl = j - i; if (nl > M_NAME - 1) nl = M_NAME - 1;
                for (int x = 0; x < nl; x++) name[x] = src[i + x]; name[nl] = 0;
                /* data label? next token is a data/reserve directive */
                int r = j + 1; while (r < lineend && (src[r] == ' ' || src[r] == '\t')) r++;
                char d[8]; int dl = 0;
                for (int rr = r; rr < lineend && mp_asm_ident(src[rr]) && dl < 7; rr++) d[dl++] = mp_lower(src[rr]);
                d[dl] = 0;
                int is_data = ide_streq(d,"db")||ide_streq(d,"dw")||ide_streq(d,"dd")||ide_streq(d,"dq")||
                              ide_streq(d,"resb")||ide_streq(d,"resw")||ide_streq(d,"resd")||ide_streq(d,"resq")||
                              ide_streq(d,"equ")||ide_streq(d,"times");
                if (cur) cur->line_end = (line > 1 ? line - 1 : line);
                if (is_data) {
                    if (m->nglobals < M_MAXGLOBALS && mp_global_index(m, name) < 0) {
                        Global* g = &m->globals[m->nglobals++];
                        ide_strlcpy(g->name, name, M_NAME); ide_strlcpy(g->type, "data", M_TYPE);
                        ide_strlcpy(g->file, m->cur_file, M_NAME); g->nreaders = 0; g->nwriters = 0;
                    }
                    cur = 0;
                    pos = (le < len) ? le + 1 : le; line++; continue;
                }
                if (m->nfuncs < M_MAXFUNCS) {
                    cur = &m->funcs[m->nfuncs++];
                    ide_strlcpy(cur->name, name, M_NAME); ide_strlcpy(cur->ret, "label", M_TYPE);
                    ide_strlcpy(cur->file, m->cur_file, M_NAME);
                    cur->line_start = line; cur->line_end = line;
                    cur->nparams = 0; cur->ncalls = 0; cur->nreads = 0; cur->nwrites = 0;
                    cur->nports = 0; cur->closed = 0;
                } else cur = 0;
                i = j + 1;   /* scan the rest of the line for an instruction (e.g. "foo: call bar") */
            }
        }

        /* instruction / directive on the rest of the line */
        while (i < lineend && (src[i] == ' ' || src[i] == '\t')) i++;
        if (i < lineend) {
            int j = i; while (j < lineend && mp_asm_ident(src[j])) j++;
            char mn[16]; int ml = j - i; if (ml > 15) ml = 15;
            for (int x = 0; x < ml; x++) mn[x] = mp_lower(src[i + x]); mn[ml] = 0;
            const char* mnn = (mn[0] == '.') ? mn + 1 : mn;   /* normalize GAS .globl etc. */
            int o = j; while (o < lineend && (src[o] == ' ' || src[o] == '\t')) o++;
            int oe = o; while (oe < lineend && mp_asm_ident(src[oe])) oe++;
            char op[M_NAME]; int ol = oe - o; if (ol > M_NAME - 1) ol = M_NAME - 1;
            for (int x = 0; x < ol; x++) op[x] = src[o + x]; op[ol] = 0;

            if (ide_streq(mnn,"global") || ide_streq(mnn,"globl") || ide_streq(mnn,"extern")) {
                if (op[0] && !mp_is_reg(op) && m->nprotos < M_MAXPROTOS) {
                    Proto* pr = &m->protos[m->nprotos++];
                    ide_strlcpy(pr->name, op, M_NAME);
                    ide_strlcpy(pr->ret, ide_streq(mnn,"extern") ? "extern" : "global", M_TYPE);
                    pr->line = line;
                }
            } else if (cur && op[0] && !mp_is_reg(op)) {
                int isctl = ide_streq(mnn,"call") || (mnn[0] == 'j' && ide_strlen(mnn) <= 4);  /* call / jmp / jcc */
                if (isctl) mp_list_add(cur->calls, &cur->ncalls, M_MAXCALLS, op);
            }
        }
        pos = (le < len) ? le + 1 : le; line++;
    }
    if (cur) cur->line_end = (line > 1 ? line - 1 : line);
}

/* ----------------------------------------------------------------------- */
/* entry point                                                             */
/* ----------------------------------------------------------------------- */
/* IDE-XFILE-0: the reset half of the old fused reset+parse. Callers that
 * build a MULTI-FILE model reset once, then model_parse_append() per file. */
void model_reset(Model* m) {
    if (!m) return;
    m->nfuncs    = 0;
    m->nglobals  = 0;
    m->nincludes = 0;
    m->nmacros   = 0;
    m->nrecords  = 0;
    m->nprotos   = 0;
    m->nconns    = 0;
    m->nrisks    = 0;
    m->nactions  = 0;
    m->nflow     = 0;
    m->focus     = -1;
    m->coherence = 0;
    m->analyzed  = 0;
    m->lexed     = 0;
    m->parsed    = 0;
    m->total_lines = 0;
}

/* IDE-XFILE-0: parse ONE file's source INTO m without clearing prior files.
 * The transient parse state (token buffer + AST arena) is reset per call by
 * parser_init/parse_translation_unit, so only the Model accumulates. Sets
 * cur_file=basename and total_lines for THIS file -- last caller wins, so a
 * multi-file driver parses the OPEN file last. Body = the old model_parse
 * minus the reset; NO new parser features. */
void model_parse_append(Model* m, const char* src, int len, const char* filename) {
    if (!m) return;

    /* line count = number of '\n' + 1 (empty buffer -> 1 line) */
    int lines = 1;
    if (src && len > 0) {
        for (int i = 0; i < len; i++)
            if (src[i] == '\n') lines++;
    }
    m->total_lines = lines;

    /* basename of filename after the last '/' */
    const char* base = filename ? filename : "";
    if (filename) {
        for (const char* p = filename; *p; p++)
            if (*p == '/') base = p + 1;
    }
    ide_strlcpy(m->cur_file, base, M_NAME);

    /* nothing to parse: still produce a consistent (empty) model */
    if (!src || len <= 0) {
        m->lexed  = 1;
        m->parsed = 1;
        return;
    }

    /* Assembly files: build the model from labels + call/jmp edges (a real
     * control-flow graph) instead of the C parser, so .asm/.s ALSO render in
     * the Semantic LEGO Map. */
    if (mp_filename_is_asm(filename)) {
        mp_parse_asm(m, src, len);
        m->lexed  = 1;
        m->parsed = 1;
        return;
    }

    /* 2a. pre-parse scan for #include / #define (before parser_init strips them) */
    mp_pass_preproc(m, src, len);

    /* 2b. parse to an AST */
    parser_init(&P, src, len, toks, PARSE_MAX_TOKS);
    m->lexed = 1;
    AstNode* root = parse_translation_unit(&P);

    /* 3 & 4: walk the tree (robust if root is NULL or the arena overflowed) */
    if (root) {
        mp_pass_globals(m, root);   /* PASS 1: globals first (so func walk
                                       can recognise global references)     */
        mp_pass_records(m, root);   /* PASS 1b: structs/typedefs/enums      */
        mp_pass_protos(m, root);    /* PASS 1c: function prototypes         */
        mp_pass_funcs(m, root);     /* PASS 2: functions + bodies           */
    }

    /* 5. status */
    m->parsed = 1;
    /* NOTE: model_analyze() is called by the caller next; do not call here.
     * focus was reset by model_reset(); the caller sets it afterward. */
}

/* The legacy fused entry point: single-file reset+parse, byte-for-byte the
 * old behavior (every existing caller keeps working unchanged). */
void model_parse(Model* m, const char* src, int len, const char* filename) {
    model_reset(m);
    model_parse_append(m, src, len, filename);
}

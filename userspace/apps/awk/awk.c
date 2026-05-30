/*
 * awk.c -- small AWK for a from-scratch x86_64 OS (freestanding, ring 3).
 * =========================================================================
 *
 * A tiny single-pass AWK interpreter. NO libc: pure inline syscalls plus
 * hand-rolled helpers. Operates on FILE ARGUMENTS only -- this OS has no
 * pipes/stdin, so there is nothing to read from fd 0.
 *
 * Usage model (the program text + input file are passed to run_awk()):
 *     awk 'PROGRAM' INFILE          (conceptual CLI; see argv note below)
 *
 * Supported PROGRAM subset (a real, if small, awk -- recursive-descent
 * expression evaluator + statement interpreter):
 *
 *   RULES (a program is a sequence of rules, separated by newlines or by
 *   the end of one action `}` and the start of the next):
 *       BEGIN { ACTION }          run once before any input
 *       END   { ACTION }          run once after all input
 *       /PAT/ { ACTION }          run ACTION when the line contains PAT
 *       EXPR  { ACTION }          run ACTION when EXPR is true (e.g. $2>5)
 *       { ACTION }                run ACTION on every line
 *       /PAT/                     bare pattern -> default action {print $0}
 *       EXPR                      bare expr    -> default action {print $0}
 *
 *   ACTION = STMT { ';' STMT }
 *       print EXPR [, EXPR ...]   print expr list, space-joined, '\n' end
 *       print                     print $0
 *       VAR = EXPR                assignment
 *       VAR += EXPR               compound add-assign
 *       if (COND) STMT [else STMT]
 *       { STMT ; STMT ... }       statement block
 *
 *   EXPR (lowest..highest precedence):
 *       ||                        logical or
 *       &&                        logical and
 *       == != < <= > >=           comparisons
 *       (concat)                  adjacent terms -> string concatenation
 *       + -                       additive
 *       * / %                     multiplicative
 *       unary - ! +
 *       primary: NUMBER, "STRING", $N, $(EXPR), NR, NF, VAR, ( EXPR )
 *
 *   -F X      set the field separator to the single char X
 *             (default: any run of spaces/tabs)
 *
 * Values are dynamically numeric-or-string (awk-like): a string used in
 * arithmetic is parsed as an integer prefix; a number used as text is
 * formatted decimal. Arithmetic is integer (this OS has no FP in ring 3).
 *
 * ---------------------------------------------------------------------------
 * ARGV AVAILABILITY (IMPORTANT)
 * ---------------------------------------------------------------------------
 * Spawned programs in this OS receive NO argv/argc. sys_spawn (handlers.c)
 * takes only a path and ignores args; elf_load_and_exec (kernel/fs/exec.c)
 * builds the initial user stack itself (RSP = top-16, nothing pushed) and
 * never installs an argc/argv frame -- the `elf_setup_stack` argv path in
 * elf_loader.c is an unused TODO. Therefore a spawned `_start` cannot read a
 * command line. So `_start` runs the SELF-TEST unconditionally, and the real
 * awk logic lives in the callable run_awk(program, infile, sep) function.
 *
 * Syscall numbers verified against kernel/include/syscall.h:
 *     SYS_EXIT=0  SYS_READ=2  SYS_WRITE=3  SYS_OPEN=4  SYS_CLOSE=5
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector gets dropped and the program faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/awk/awk.c -o awk.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       awk.o -o build/awk
 *   objdump -d build/awk | grep fs:0x28   # MUST be empty
 *
 * Serial selftest marker (fd 1): "AWK SELFTEST: PASS" or "AWK SELFTEST: FAIL".
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified: kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

/* O_* flags (kernel/include/vfs.h, mirrored from terminal_m3.c). */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_CREAT    0x0040
#define O_TRUNC    0x0200

typedef unsigned long size_t;

/* -----------------------------------------------------------------------
 * 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9).
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers
 * ===================================================================== */

static size_t a_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int a_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* copy NUL-terminated src into dst (cap chars incl. NUL); truncates safely */
static void a_strcpy_cap(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* substring test: does hay[0..hlen) contain NUL-terminated needle? */
static int contains_sub(const char *hay, long hlen, const char *needle) {
    long nlen = (long)a_strlen(needle);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (long i = 0; i + nlen <= hlen; i++) {
        long j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

/* Parse a leading integer (optional sign) from a string; awk-style coercion. */
static long str_to_long(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/*
 * Is the WHOLE string a (signed) integer with only surrounding blanks?
 * Used for awk's "numeric string" comparison rule: a field that looks like a
 * number compares numerically rather than lexically.
 */
static int looks_numeric(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') s++;
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9') s++;
    while (*s == ' ' || *s == '\t') s++;
    return *s == '\0';
}

/* ---- fd 1 output ---- */
static void out(const char *s)            { sc(SYS_WRITE, 1, (long)s, (long)a_strlen(s), 0, 0, 0); }
static void outn(const char *s, long n)   { if (n > 0) sc(SYS_WRITE, 1, (long)s, n, 0, 0, 0); }
static void outc(char c)                  { sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0); }

/* Format a signed long into buf (>=24 bytes). Returns length. */
static int fmt_long(char *buf, long v) {
    int i = 0;
    unsigned long n;
    int neg = 0;
    if (v < 0) { neg = 1; n = (unsigned long)(-(v + 1)) + 1UL; } else { n = (unsigned long)v; }
    char tmp[24]; int t = 0;
    do { tmp[t++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    if (neg) buf[i++] = '-';
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = '\0';
    return i;
}

/* =======================================================================
 *  Whole-file read into a static buffer (cap 64KB).
 * ===================================================================== */
#define FILE_BUF_MAX 65536
static char g_filebuf[FILE_BUF_MAX] __attribute__((aligned(16)));

/* Returns bytes read (>=0), -1 on open failure, -2 if the file overflowed. */
static long slurp_file(const char *path) {
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_BUF_MAX - total;
        if (room <= 0) {
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1, 0, 0, 0);
            sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            if (n > 0) return -2;
            return total;
        }
        long n = sc(SYS_READ, fd, (long)(g_filebuf + total), room, 0, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return total;
}

/* =======================================================================
 *  Field splitting
 *
 *  Splits one line [line, line+len) into fields. Field pointers are stored
 *  in a static field table; field text is referenced in-place (we NUL out
 *  separators in a private scratch copy so $N is a valid C string).
 * ===================================================================== */
#define MAX_FIELDS 256
#define LINE_SCRATCH 8192
static char  g_linebuf[LINE_SCRATCH];     /* NUL-terminated copy of the line */
static char  g_rawline[LINE_SCRATCH];     /* NUL-terminated $0 copy          */
static int   g_rawlen;                     /* length of $0                    */
static char *g_fields[MAX_FIELDS];        /* $1..$NF -> g_fields[0..NF-1]    */
static int   g_nf;                        /* NF                              */
static char  g_field_sep;                 /* 0 = whitespace, else literal    */

/*
 * Populate g_linebuf/g_fields/g_nf from the record [line, line+len).
 *   sep == 0  -> split on runs of spaces/tabs (leading/trailing trimmed)
 *   sep != 0  -> split on each `sep` char (empty fields preserved)
 */
static void split_fields(const char *line, long len) {
    if (len > LINE_SCRATCH - 1) len = LINE_SCRATCH - 1;
    for (long i = 0; i < len; i++) { g_linebuf[i] = line[i]; g_rawline[i] = line[i]; }
    g_linebuf[len] = '\0';
    g_rawline[len] = '\0';
    g_rawlen = (int)len;
    g_nf = 0;

    char *p = g_linebuf;
    if (g_field_sep == 0) {
        /* whitespace mode: collapse runs of ' '/'\t' */
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (g_nf < MAX_FIELDS) g_fields[g_nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    } else {
        /* explicit single-char separator: every occurrence splits */
        if (*p == '\0') return;          /* empty line -> 0 fields */
        if (g_nf < MAX_FIELDS) g_fields[g_nf++] = p;
        while (*p) {
            if (*p == g_field_sep) {
                *p = '\0';
                p++;
                if (g_nf < MAX_FIELDS) g_fields[g_nf++] = p;
            } else {
                p++;
            }
        }
    }
}

/* =======================================================================
 *  Values (numeric OR string), and the scalar variable symbol table.
 *
 *  A value carries both representations lazily: `is_num` says which is
 *  authoritative. Coercion happens on demand (val_num / val_str).
 * ===================================================================== */
#define VAL_STR_MAX 256

typedef struct {
    int  is_num;            /* 1 -> num authoritative, 0 -> str authoritative */
    long num;
    char str[VAL_STR_MAX];
} value_t;

static value_t mk_num(long n) {
    value_t v;
    v.is_num = 1;
    v.num = n;
    v.str[0] = '\0';
    return v;
}

static value_t mk_str(const char *s) {
    value_t v;
    v.is_num = 0;
    v.num = 0;
    a_strcpy_cap(v.str, s, VAL_STR_MAX);
    return v;
}

static long val_num(const value_t *v) {
    if (v->is_num) return v->num;
    return str_to_long(v->str);
}

/* Materialize a value as text into `buf` (>= VAL_STR_MAX). */
static void val_str(const value_t *v, char *buf) {
    if (v->is_num) { fmt_long(buf, v->num); return; }
    a_strcpy_cap(buf, v->str, VAL_STR_MAX);
}

/* Truthiness: numeric != 0, or non-empty string that isn't "0"-ish. */
static int val_true(const value_t *v) {
    if (v->is_num) return v->num != 0;
    /* awk: a string is true if non-empty (string context). Our exprs that
     * produce comparison results are numeric, so this only hits literals/
     * fields used directly as conditions -> treat non-empty as true, but a
     * purely numeric-looking string uses its numeric value. */
    if (v->str[0] == '\0') return 0;
    return 1;
}

#define MAX_VARS 64
typedef struct {
    char    name[32];
    value_t val;
} var_t;
static var_t g_vars[MAX_VARS];
static int   g_nvars;

/* Find (or create) a variable slot by name. Returns NULL if table full. */
static var_t *var_lookup(const char *name, int create) {
    for (int i = 0; i < g_nvars; i++) {
        if (a_streq(g_vars[i].name, name)) return &g_vars[i];
    }
    if (!create) return (var_t *)0;
    if (g_nvars >= MAX_VARS) return (var_t *)0;
    var_t *v = &g_vars[g_nvars++];
    a_strcpy_cap(v->name, name, (int)sizeof(v->name));
    v->val = mk_num(0);                  /* uninitialized vars default to 0/"" */
    return v;
}

static void vars_reset(void) { g_nvars = 0; }

/* =======================================================================
 *  Tokenizer
 *
 *  We tokenize the program text once. Tokens reference the program buffer
 *  for identifiers/strings (copied into small fixed buffers).
 * ===================================================================== */
enum {
    T_EOF = 0,
    T_NUM, T_STR, T_IDENT, T_FIELD,   /* $ token */
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN,
    T_SEMI, T_COMMA, T_NEWLINE,
    T_ASSIGN, T_PLUSEQ,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT,
    T_REGEX,                          /* /pat/ literal */
    T_KW_PRINT, T_KW_IF, T_KW_ELSE, T_KW_BEGIN, T_KW_END
};

#define MAX_TOKENS 1024
#define TOK_TEXT_MAX 256

typedef struct {
    int  type;
    long num;                  /* T_NUM */
    char text[TOK_TEXT_MAX];   /* T_STR / T_IDENT / T_REGEX */
} token_t;

static token_t g_toks[MAX_TOKENS];
static int     g_ntok;

/* Parse error flag (sticky). */
static int g_parse_err;

/*
 * Whether a '/' at the current position starts a regex (vs. division).
 * Heuristic: regex follows the start, '{', ';', '(', ',', a newline, or any
 * operator/keyword -- i.e. anywhere an expression/pattern may begin.
 */
static int regex_allowed(int prev_type) {
    switch (prev_type) {
        case T_EOF: case T_LBRACE: case T_RBRACE: case T_SEMI: case T_LPAREN:
        case T_COMMA: case T_NEWLINE: case T_ASSIGN: case T_PLUSEQ:
        case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
        case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
        case T_AND: case T_OR: case T_NOT:
        case T_KW_PRINT: case T_KW_IF: case T_KW_ELSE:
            return 1;
        default:
            return 0;
    }
}

static int kw_type(const char *s) {
    if (a_streq(s, "print")) return T_KW_PRINT;
    if (a_streq(s, "if"))    return T_KW_IF;
    if (a_streq(s, "else"))  return T_KW_ELSE;
    if (a_streq(s, "BEGIN")) return T_KW_BEGIN;
    if (a_streq(s, "END"))   return T_KW_END;
    return T_IDENT;
}

static void push_tok(int type) {
    if (g_ntok >= MAX_TOKENS) { g_parse_err = 1; return; }
    token_t *t = &g_toks[g_ntok++];
    t->type = type;
    t->num = 0;
    t->text[0] = '\0';
}

/* Tokenize the whole program. Returns 0 on success, -1 on error. */
static int tokenize(const char *prog) {
    g_ntok = 0;
    g_parse_err = 0;
    const char *p = prog;

    while (*p) {
        char c = *p;
        int prev = (g_ntok > 0) ? g_toks[g_ntok - 1].type : T_EOF;

        if (c == ' ' || c == '\t') { p++; continue; }
        if (c == '\n' || c == '\r') {
            /* collapse to a single NEWLINE separator */
            if (prev != T_NEWLINE && prev != T_EOF && g_ntok > 0)
                push_tok(T_NEWLINE);
            p++;
            continue;
        }
        if (c == '#') { while (*p && *p != '\n') p++; continue; }  /* comment */

        /* numbers */
        if (c >= '0' && c <= '9') {
            long v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (g_ntok >= MAX_TOKENS) { g_parse_err = 1; return -1; }
            token_t *t = &g_toks[g_ntok++];
            t->type = T_NUM; t->num = v; t->text[0] = '\0';
            continue;
        }

        /* identifiers / keywords */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            char name[TOK_TEXT_MAX];
            int i = 0;
            while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_') && i < TOK_TEXT_MAX - 1) {
                name[i++] = *p++;
            }
            name[i] = '\0';
            int kt = kw_type(name);
            if (g_ntok >= MAX_TOKENS) { g_parse_err = 1; return -1; }
            token_t *t = &g_toks[g_ntok++];
            t->type = kt; t->num = 0;
            a_strcpy_cap(t->text, name, TOK_TEXT_MAX);
            continue;
        }

        /* string literal */
        if (c == '"') {
            p++;
            char buf[TOK_TEXT_MAX];
            int i = 0;
            while (*p && *p != '"' && i < TOK_TEXT_MAX - 1) {
                if (*p == '\\' && p[1]) {
                    p++;
                    char e = *p;
                    buf[i++] = (e == 'n') ? '\n' : (e == 't') ? '\t' :
                               (e == 'r') ? '\r' : (e == '\\') ? '\\' :
                               (e == '"') ? '"' : e;
                    p++;
                } else {
                    buf[i++] = *p++;
                }
            }
            buf[i] = '\0';
            if (*p != '"') { g_parse_err = 1; return -1; }
            p++;
            if (g_ntok >= MAX_TOKENS) { g_parse_err = 1; return -1; }
            token_t *t = &g_toks[g_ntok++];
            t->type = T_STR; t->num = 0;
            a_strcpy_cap(t->text, buf, TOK_TEXT_MAX);
            continue;
        }

        /* '/' -> regex literal or division */
        if (c == '/') {
            if (regex_allowed(prev)) {
                p++;
                char buf[TOK_TEXT_MAX];
                int i = 0;
                while (*p && *p != '/' && i < TOK_TEXT_MAX - 1) {
                    if (*p == '\\' && p[1]) { buf[i++] = p[1]; p += 2; }
                    else buf[i++] = *p++;
                }
                buf[i] = '\0';
                if (*p != '/') { g_parse_err = 1; return -1; }
                p++;
                if (g_ntok >= MAX_TOKENS) { g_parse_err = 1; return -1; }
                token_t *t = &g_toks[g_ntok++];
                t->type = T_REGEX; t->num = 0;
                a_strcpy_cap(t->text, buf, TOK_TEXT_MAX);
                continue;
            }
            push_tok(T_SLASH); p++; continue;
        }

        /* field reference '$' */
        if (c == '$') { push_tok(T_FIELD); p++; continue; }

        /* multi-char operators */
        if (c == '=' && p[1] == '=') { push_tok(T_EQ); p += 2; continue; }
        if (c == '!' && p[1] == '=') { push_tok(T_NE); p += 2; continue; }
        if (c == '<' && p[1] == '=') { push_tok(T_LE); p += 2; continue; }
        if (c == '>' && p[1] == '=') { push_tok(T_GE); p += 2; continue; }
        if (c == '&' && p[1] == '&') { push_tok(T_AND); p += 2; continue; }
        if (c == '|' && p[1] == '|') { push_tok(T_OR); p += 2; continue; }
        if (c == '+' && p[1] == '=') { push_tok(T_PLUSEQ); p += 2; continue; }

        /* single-char tokens */
        switch (c) {
            case '{': push_tok(T_LBRACE);  p++; continue;
            case '}': push_tok(T_RBRACE);  p++; continue;
            case '(': push_tok(T_LPAREN);  p++; continue;
            case ')': push_tok(T_RPAREN);  p++; continue;
            case ';': push_tok(T_SEMI);    p++; continue;
            case ',': push_tok(T_COMMA);   p++; continue;
            case '=': push_tok(T_ASSIGN);  p++; continue;
            case '+': push_tok(T_PLUS);    p++; continue;
            case '-': push_tok(T_MINUS);   p++; continue;
            case '*': push_tok(T_STAR);    p++; continue;
            case '%': push_tok(T_PERCENT); p++; continue;
            case '<': push_tok(T_LT);      p++; continue;
            case '>': push_tok(T_GT);      p++; continue;
            case '!': push_tok(T_NOT);     p++; continue;
            default:  g_parse_err = 1;     return -1;   /* unknown char */
        }
    }
    push_tok(T_EOF);
    return g_parse_err ? -1 : 0;
}

/* =======================================================================
 *  Parser / interpreter shared cursor.
 *
 *  We interpret directly from the token stream (no AST): the parser and the
 *  evaluator are fused. To run a rule's action twice (or skip it) we record
 *  the token index where each action begins and re-seek the cursor.
 * ===================================================================== */
static int g_cur;                   /* current token index */
static unsigned long g_nr;          /* NR for the current record */
static int g_exec;                  /* 1 = execute side effects, 0 = parse-only */

static token_t *cur(void)  { return &g_toks[g_cur]; }
static int      curtype(void) { return g_toks[g_cur].type; }
static void     advance(void) { if (g_toks[g_cur].type != T_EOF) g_cur++; }

static int accept(int type) {
    if (curtype() == type) { advance(); return 1; }
    return 0;
}
static int expect(int type) {
    if (accept(type)) return 1;
    g_parse_err = 1;
    return 0;
}

/* Field accessor: n==0 -> $0 (raw line); else $n (or "" if out of range). */
static value_t get_field(long n) {
    if (n <= 0) return mk_str(g_rawline);
    if (n <= (long)g_nf) return mk_str(g_fields[n - 1]);
    return mk_str("");
}

/* Forward decls (mutual recursion). */
static value_t eval_expr(void);
static value_t eval_or(void);

/* ---- primary ---- */
static value_t eval_primary(void) {
    token_t *t = cur();
    switch (t->type) {
        case T_NUM:   advance(); return mk_num(t->num);
        case T_STR:   advance(); return mk_str(t->text);
        case T_KW_BEGIN: case T_KW_END:
            /* BEGIN/END only valid as rule patterns; here treat as ident-ish */
            advance(); return mk_num(0);
        case T_IDENT: {
            char name[32];
            a_strcpy_cap(name, t->text, (int)sizeof(name));
            if (a_streq(name, "NR")) { advance(); return mk_num((long)g_nr); }
            if (a_streq(name, "NF")) { advance(); return mk_num((long)g_nf); }
            advance();
            var_t *v = var_lookup(name, 1);
            if (!v) { g_parse_err = 1; return mk_num(0); }
            return v->val;
        }
        case T_FIELD: {
            advance();
            if (curtype() == T_LPAREN) {
                advance();
                value_t idx = eval_expr();
                expect(T_RPAREN);
                return get_field(val_num(&idx));
            }
            /* $NUM or $NR/$NF or $VAR */
            value_t idx = eval_primary();
            return get_field(val_num(&idx));
        }
        case T_LPAREN: {
            advance();
            value_t v = eval_expr();
            expect(T_RPAREN);
            return v;
        }
        case T_MINUS: { advance(); value_t v = eval_primary(); return mk_num(-val_num(&v)); }
        case T_PLUS:  { advance(); value_t v = eval_primary(); return mk_num(val_num(&v)); }
        case T_NOT:   { advance(); value_t v = eval_primary(); return mk_num(!val_true(&v)); }
        default:
            g_parse_err = 1;
            advance();
            return mk_num(0);
    }
}

/* ---- multiplicative: * / % ---- */
static value_t eval_mul(void) {
    value_t l = eval_primary();
    for (;;) {
        int op = curtype();
        if (op != T_STAR && op != T_SLASH && op != T_PERCENT) break;
        advance();
        value_t r = eval_primary();
        long a = val_num(&l), b = val_num(&r);
        if (op == T_STAR) l = mk_num(a * b);
        else if (op == T_SLASH) l = mk_num(b != 0 ? a / b : 0);
        else l = mk_num(b != 0 ? a % b : 0);
    }
    return l;
}

/* ---- additive: + - ---- */
static value_t eval_add(void) {
    value_t l = eval_mul();
    for (;;) {
        int op = curtype();
        if (op != T_PLUS && op != T_MINUS) break;
        advance();
        value_t r = eval_mul();
        long a = val_num(&l), b = val_num(&r);
        l = mk_num(op == T_PLUS ? a + b : a - b);
    }
    return l;
}

/* Can a token start a concatenation operand (a "term")? */
static int starts_concat(int type) {
    switch (type) {
        case T_NUM: case T_STR: case T_IDENT: case T_FIELD:
        case T_LPAREN:
            return 1;
        default:
            return 0;
    }
}

/* ---- string concatenation: adjacent additive terms ---- */
static value_t eval_concat(void) {
    value_t l = eval_add();
    if (!starts_concat(curtype())) return l;

    /* Build a string by appending each following term's text. */
    char acc[VAL_STR_MAX];
    val_str(&l, acc);
    int len = (int)a_strlen(acc);
    while (starts_concat(curtype())) {
        value_t r = eval_add();
        char rb[VAL_STR_MAX];
        val_str(&r, rb);
        int j = 0;
        while (rb[j] && len < VAL_STR_MAX - 1) acc[len++] = rb[j++];
        acc[len] = '\0';
    }
    return mk_str(acc);
}

/*
 * awk comparison rule: compare numerically when both operands are numbers or
 * numeric-looking strings; otherwise compare as strings. (A bare string
 * constant like "x" forces a string comparison even against a number.)
 */
static int cmp_numeric(const value_t *a, const value_t *b) {
    int an = a->is_num || looks_numeric(a->str);
    int bn = b->is_num || looks_numeric(b->str);
    return an && bn;
}

/* ---- comparisons: == != < <= > >= ---- */
static value_t eval_cmp(void) {
    value_t l = eval_concat();
    int op = curtype();
    if (op == T_EQ || op == T_NE || op == T_LT ||
        op == T_LE || op == T_GT || op == T_GE) {
        advance();
        value_t r = eval_concat();
        int res = 0;
        if (cmp_numeric(&l, &r)) {
            long a = val_num(&l), b = val_num(&r);
            switch (op) {
                case T_EQ: res = (a == b); break;
                case T_NE: res = (a != b); break;
                case T_LT: res = (a <  b); break;
                case T_LE: res = (a <= b); break;
                case T_GT: res = (a >  b); break;
                case T_GE: res = (a >= b); break;
            }
        } else {
            /* string comparison */
            char as[VAL_STR_MAX], bs[VAL_STR_MAX];
            val_str(&l, as); val_str(&r, bs);
            int c = 0;
            const char *x = as, *y = bs;
            while (*x && *y && *x == *y) { x++; y++; }
            c = (int)(unsigned char)*x - (int)(unsigned char)*y;
            switch (op) {
                case T_EQ: res = (c == 0); break;
                case T_NE: res = (c != 0); break;
                case T_LT: res = (c <  0); break;
                case T_LE: res = (c <= 0); break;
                case T_GT: res = (c >  0); break;
                case T_GE: res = (c >= 0); break;
            }
        }
        return mk_num(res);
    }
    return l;
}

/* ---- logical and ---- */
static value_t eval_and(void) {
    value_t l = eval_cmp();
    while (curtype() == T_AND) {
        advance();
        value_t r = eval_cmp();
        l = mk_num(val_true(&l) && val_true(&r));
    }
    return l;
}

/* ---- logical or ---- */
static value_t eval_or(void) {
    value_t l = eval_and();
    while (curtype() == T_OR) {
        advance();
        value_t r = eval_and();
        l = mk_num(val_true(&l) || val_true(&r));
    }
    return l;
}

/* ---- top-level expression (handles assignment as an expr too) ---- */
static value_t eval_expr(void) {
    /* lookahead: IDENT '=' / IDENT '+=' is an assignment */
    if (curtype() == T_IDENT &&
        (g_toks[g_cur + 1].type == T_ASSIGN || g_toks[g_cur + 1].type == T_PLUSEQ)) {
        char name[32];
        a_strcpy_cap(name, cur()->text, (int)sizeof(name));
        int op = g_toks[g_cur + 1].type;
        advance(); advance();                 /* consume IDENT and =/+= */
        value_t rhs = eval_expr();
        var_t *v = var_lookup(name, 1);
        if (!v) { g_parse_err = 1; return mk_num(0); }
        /* g_exec gates the mutation so a skipped branch leaves vars untouched,
         * but we still evaluate RHS above to keep the cursor advancing. */
        if (g_exec) {
            if (op == T_PLUSEQ) {
                v->val = mk_num(val_num(&v->val) + val_num(&rhs));
            } else {
                v->val = rhs;
            }
        }
        return v->val;
    }
    return eval_or();
}

/* =======================================================================
 *  Statement interpreter.
 *
 *  `g_exec` gates side effects: when 0 we still walk tokens (so the cursor
 *  ends up past the statement) but suppress print/assignment. This lets us
 *  parse-skip an action whose pattern did not match.
 * ===================================================================== */
static void run_stmt(void);

/* print EXPR-list */
static void run_print(void) {
    advance();  /* consume 'print' */

    /* `print` with no args / end-of-action -> $0 */
    if (curtype() == T_SEMI || curtype() == T_RBRACE ||
        curtype() == T_NEWLINE || curtype() == T_EOF) {
        if (g_exec) { outn(g_rawline, g_rawlen); outc('\n'); }
        return;
    }

    int first = 1;
    for (;;) {
        value_t v = eval_expr();
        if (g_exec) {
            if (!first) outc(' ');
            char buf[VAL_STR_MAX];
            val_str(&v, buf);
            out(buf);
        }
        first = 0;
        if (accept(T_COMMA)) continue;
        break;
    }
    if (g_exec) outc('\n');
}

/* A single statement: print | if | block | expr/assignment | empty */
static void run_stmt(void) {
    switch (curtype()) {
        case T_KW_PRINT:
            run_print();
            return;

        case T_KW_IF: {
            advance();
            expect(T_LPAREN);
            value_t c = eval_expr();
            expect(T_RPAREN);
            int taken = val_true(&c);

            /* then-branch */
            int saved = g_exec;
            if (!taken) g_exec = 0;
            run_stmt();
            g_exec = saved;

            /* optional `; else` or `else` */
            int probe = g_cur;
            if (curtype() == T_SEMI) probe++;   /* allow `if(..) stmt; else` */
            if (g_toks[probe].type == T_KW_ELSE) {
                g_cur = probe;
                advance();                       /* consume else */
                int saved2 = g_exec;
                if (taken) g_exec = 0;
                run_stmt();
                g_exec = saved2;
            }
            return;
        }

        case T_LBRACE: {
            advance();
            while (curtype() != T_RBRACE && curtype() != T_EOF) {
                run_stmt();
                if (curtype() == T_SEMI || curtype() == T_NEWLINE) advance();
                else break;
            }
            expect(T_RBRACE);
            return;
        }

        case T_SEMI: case T_NEWLINE:
            return;                              /* empty statement */

        default: {
            /* expression statement (e.g. assignment `s += $1`) */
            eval_expr();
            return;
        }
    }
}

/* Run an action block { STMT ; STMT ... }. Cursor must be at '{'. */
static void run_action_block(void) {
    expect(T_LBRACE);
    while (curtype() != T_RBRACE && curtype() != T_EOF) {
        run_stmt();
        if (curtype() == T_SEMI || curtype() == T_NEWLINE) { advance(); continue; }
        break;
    }
    expect(T_RBRACE);
}

/* =======================================================================
 *  Rule table
 *
 *  We pre-scan the token stream once to record each rule: its pattern kind
 *  and the token index of its '{' action (or -1 for the default action).
 * ===================================================================== */
enum { PK_ALWAYS, PK_BEGIN, PK_END, PK_REGEX, PK_EXPR };

typedef struct {
    int  kind;          /* PK_* */
    int  pat_tok;       /* token index where the pattern expr begins (PK_EXPR) */
    char regex[TOK_TEXT_MAX]; /* PK_REGEX literal substring */
    int  action_tok;    /* token index of '{', or -1 => default {print $0} */
    int  has_action;    /* 1 if an explicit { } block follows */
} rule_t;

#define MAX_RULES 64
static rule_t g_rules[MAX_RULES];
static int    g_nrules;

/* Skip a balanced { ... } starting at token index `i`; returns index past }. */
static int scan_skip_block(int i) {
    /* assumes g_toks[i] == T_LBRACE */
    int depth = 0;
    while (g_toks[i].type != T_EOF) {
        if (g_toks[i].type == T_LBRACE) depth++;
        else if (g_toks[i].type == T_RBRACE) { depth--; i++; if (depth == 0) return i; continue; }
        i++;
    }
    return i;
}

/*
 * Scan the token stream into the rule table. Returns 0 on success.
 * Rules are separated by NEWLINE/SEMI or simply by adjacency of `}` and the
 * next pattern.
 */
static int scan_rules(void) {
    g_nrules = 0;
    int i = 0;

    while (g_toks[i].type != T_EOF) {
        /* skip separators between rules */
        if (g_toks[i].type == T_NEWLINE || g_toks[i].type == T_SEMI) { i++; continue; }

        if (g_nrules >= MAX_RULES) { g_parse_err = 1; return -1; }
        rule_t *r = &g_rules[g_nrules];
        r->kind = PK_ALWAYS;
        r->pat_tok = -1;
        r->regex[0] = '\0';
        r->action_tok = -1;
        r->has_action = 0;

        int tt = g_toks[i].type;

        if (tt == T_KW_BEGIN) {
            r->kind = PK_BEGIN;
            i++;
            if (g_toks[i].type != T_LBRACE) { g_parse_err = 1; return -1; }
            r->action_tok = i;
            r->has_action = 1;
            i = scan_skip_block(i);
        } else if (tt == T_KW_END) {
            r->kind = PK_END;
            i++;
            if (g_toks[i].type != T_LBRACE) { g_parse_err = 1; return -1; }
            r->action_tok = i;
            r->has_action = 1;
            i = scan_skip_block(i);
        } else if (tt == T_REGEX) {
            r->kind = PK_REGEX;
            a_strcpy_cap(r->regex, g_toks[i].text, TOK_TEXT_MAX);
            i++;
            if (g_toks[i].type == T_LBRACE) {
                r->action_tok = i;
                r->has_action = 1;
                i = scan_skip_block(i);
            }
        } else if (tt == T_LBRACE) {
            /* bare action, always-run */
            r->kind = PK_ALWAYS;
            r->action_tok = i;
            r->has_action = 1;
            i = scan_skip_block(i);
        } else {
            /* expression pattern: spans tokens until '{' or a separator */
            r->kind = PK_EXPR;
            r->pat_tok = i;
            /* advance past the expression: stop at top-level { ; newline EOF */
            int depth = 0;
            while (g_toks[i].type != T_EOF) {
                int x = g_toks[i].type;
                if (x == T_LPAREN) depth++;
                else if (x == T_RPAREN) { if (depth > 0) depth--; }
                else if (depth == 0 && (x == T_LBRACE || x == T_SEMI || x == T_NEWLINE))
                    break;
                i++;
            }
            if (g_toks[i].type == T_LBRACE) {
                r->action_tok = i;
                r->has_action = 1;
                i = scan_skip_block(i);
            }
        }

        g_nrules++;
    }
    return 0;
}

/* =======================================================================
 *  Rule execution helpers
 * ===================================================================== */

/* Evaluate a rule's pattern (PK_EXPR/PK_REGEX) for the current record. */
static int rule_matches(rule_t *r, const char *line, long llen) {
    if (r->kind == PK_ALWAYS) return 1;
    if (r->kind == PK_REGEX)  return contains_sub(line, llen, r->regex);
    if (r->kind == PK_EXPR) {
        g_cur = r->pat_tok;
        int saved = g_exec;
        g_exec = 0;                  /* pattern eval has no side effects */
        value_t v = eval_expr();
        g_exec = saved;
        return val_true(&v);
    }
    return 0;
}

/* Execute a rule's action for the current record (or default {print $0}). */
static void run_rule_action(rule_t *r) {
    if (!r->has_action) {
        if (g_exec) { outn(g_rawline, g_rawlen); outc('\n'); }
        return;
    }
    g_cur = r->action_tok;
    run_action_block();
}

/* =======================================================================
 *  run_awk -- the callable awk core.
 *
 *  @param program  the AWK program text
 *  @param infile   path to the input file
 *  @param sep      field separator: 0 => whitespace, else a literal char
 *  @return 0 on success, negative on error.
 * ===================================================================== */
static int run_awk(const char *program, const char *infile, char sep) {
    g_field_sep = sep;
    vars_reset();

    if (tokenize(program) != 0 || scan_rules() != 0 || g_parse_err) {
        out("awk: syntax error in program\n");
        return -1;
    }

    /* BEGIN rules first (no record loaded). */
    g_nr = 0; g_nf = 0; g_rawline[0] = '\0'; g_rawlen = 0;
    g_exec = 1;
    for (int ri = 0; ri < g_nrules; ri++) {
        if (g_rules[ri].kind == PK_BEGIN) run_rule_action(&g_rules[ri]);
    }
    if (g_parse_err) { out("awk: runtime error\n"); return -1; }

    /* Decide whether we even need to read input (any non-BEGIN rules?). */
    int need_input = 0;
    for (int ri = 0; ri < g_nrules; ri++) {
        if (g_rules[ri].kind != PK_BEGIN && g_rules[ri].kind != PK_END) { need_input = 1; break; }
    }
    int have_end = 0;
    for (int ri = 0; ri < g_nrules; ri++)
        if (g_rules[ri].kind == PK_END) { have_end = 1; break; }

    if (need_input || have_end) {
        long n = slurp_file(infile);
        if (n == -1) { out("awk: cannot open '"); out(infile); out("'\n"); return -2; }
        if (n == -2) { out("awk: input file too large\n"); return -3; }

        long start = 0;
        for (long i = 0; i <= n; i++) {
            if (i == n || g_filebuf[i] == '\n') {
                long llen = i - start;
                if (llen > 0 && g_filebuf[start + llen - 1] == '\r') llen--;
                /* a trailing empty segment after the final '\n' is not a record */
                if (i == n && llen == 0 && start == i && start > 0) { start = i + 1; continue; }
                if (i == n && i == start && start == 0 && n == 0) { start = i + 1; continue; }

                g_nr++;
                const char *line = g_filebuf + start;
                split_fields(line, llen);

                g_exec = 1;
                for (int ri = 0; ri < g_nrules; ri++) {
                    rule_t *r = &g_rules[ri];
                    if (r->kind == PK_BEGIN || r->kind == PK_END) continue;
                    if (rule_matches(r, line, llen)) {
                        g_exec = 1;
                        run_rule_action(r);
                    }
                }
                start = i + 1;
            }
        }
    }

    /* END rules last (NR/NF retain last-record values, fields still loaded). */
    g_exec = 1;
    for (int ri = 0; ri < g_nrules; ri++) {
        if (g_rules[ri].kind == PK_END) run_rule_action(&g_rules[ri]);
    }

    return g_parse_err ? -1 : 0;
}

/* =======================================================================
 *  Self-test
 *
 *  Writes /tmp/awk_in.txt with known columns, then exercises several real
 *  features of the interpreter (sum via +=, conditional pattern, BEGIN/END,
 *  field comparisons) and verifies the captured results.
 *
 *  Output capture: instead of intercepting writes, we replicate run_awk()'s
 *  evaluation in-process where needed and compare the computed values.
 * ===================================================================== */

/* Write text to a path (truncating). Returns 0 on success. */
static int write_file(const char *path, const char *text, long len) {
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        long w = sc(SYS_WRITE, fd, (long)(text + off), len - off, 0, 0, 0);
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return -2; }
        off += w;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 0;
}

/*
 * Verify field extraction `{print $2}` over the known input by exercising
 * the same tokenize/scan/split path run_awk() uses. Returns 1 PASS, 0 FAIL.
 */
static int selftest_fields(void) {
    static const char *expected[] = { "banana", "green", "777" };
    const int rows = 3;

    g_field_sep = 0;
    vars_reset();
    if (tokenize("{print $2}") != 0 || scan_rules() != 0) return 0;

    long n = slurp_file("/tmp/awk_in.txt");
    if (n < 0) return 0;

    int row = 0;
    long start = 0;
    for (long i = 0; i <= n; i++) {
        if (i == n || g_filebuf[i] == '\n') {
            long llen = i - start;
            if (llen > 0 && g_filebuf[start + llen - 1] == '\r') llen--;
            if (i == n && llen == 0) { start = i + 1; continue; }
            split_fields(g_filebuf + start, llen);
            if (row >= rows) return 0;
            if (g_nf < 2) return 0;
            if (!a_streq(g_fields[1], expected[row])) return 0;
            row++;
            start = i + 1;
        }
    }
    return (row == rows);
}

/*
 * Verify the SUM program `{s+=$1} END{print s}` by running it through the
 * interpreter and checking the resulting variable `s` (column 1 = 1,2,3 in
 * the numeric self-test input -> sum should be 6).
 */
static int selftest_sum(void) {
    g_field_sep = 0;
    if (run_awk("{s+=$1} END{print s}", "/tmp/awk_num.txt", 0) != 0) return 0;
    var_t *s = var_lookup("s", 0);
    if (!s) return 0;
    return val_num(&s->val) == 6;     /* 1 + 2 + 3 */
}

/*
 * Verify a conditional/relational program. For input rows with column 2 of
 * {3, 8, 1, 9}, `$2>5{c+=1}` should fire on rows 2 and 4 -> c == 2. Also
 * checks `if/else` by accumulating into `lo` for the non-matching rows.
 */
static int selftest_cond(void) {
    g_field_sep = 0;
    if (run_awk("{ if ($2 > 5) hi += 1; else lo += 1 }", "/tmp/awk_num.txt", 0) != 0)
        return 0;
    var_t *hi = var_lookup("hi", 0);
    var_t *lo = var_lookup("lo", 0);
    if (!hi || !lo) return 0;
    /* awk_num.txt column 2 = {3, 8, 1} (3 rows) -> hi=1 (8>5), lo=2 */
    return val_num(&hi->val) == 1 && val_num(&lo->val) == 2;
}

/*
 * Verify BEGIN/END with arithmetic + concat: BEGIN sets t=10, each record
 * does t += $1 (col1 = 1,2,3 -> +6), END leaves t == 16.
 */
static int selftest_beginend(void) {
    g_field_sep = 0;
    if (run_awk("BEGIN{ t = 10 } { t += $1 } END{ print \"total=\" t }",
                "/tmp/awk_num.txt", 0) != 0) return 0;
    var_t *t = var_lookup("t", 0);
    if (!t) return 0;
    return val_num(&t->val) == 16;    /* 10 + 1 + 2 + 3 */
}

static void run_selftest(void) {
    static const char input[] =
        "apple banana cherry\n"
        "red green yellow\n"
        "111 777 333\n";
    /* numeric fixture: col1 = 1,2,3 ; col2 = 3,8,1 */
    static const char numinput[] =
        "1 3 x\n"
        "2 8 y\n"
        "3 1 z\n";

    out("[AWK] selftest: writing /tmp/awk_in.txt and /tmp/awk_num.txt\n");
    if (write_file("/tmp/awk_in.txt", input, (long)(sizeof(input) - 1)) != 0 ||
        write_file("/tmp/awk_num.txt", numinput, (long)(sizeof(numinput) - 1)) != 0) {
        out("AWK SELFTEST: FAIL (could not write input files)\n");
        return;
    }

    /* Live demonstrations for the operator. */
    out("[AWK] selftest: {print $2} over /tmp/awk_in.txt:\n");
    run_awk("{print $2}", "/tmp/awk_in.txt", 0);

    out("[AWK] selftest: /red/{print NR, $1, $3} over /tmp/awk_in.txt:\n");
    run_awk("/red/{print NR, $1, $3}", "/tmp/awk_in.txt", 0);

    out("[AWK] selftest: {s+=$1} END{print s} over /tmp/awk_num.txt:\n");
    run_awk("{s+=$1} END{print s}", "/tmp/awk_num.txt", 0);

    out("[AWK] selftest: $2>5{print $1} over /tmp/awk_num.txt:\n");
    run_awk("$2>5{print $1}", "/tmp/awk_num.txt", 0);

    out("[AWK] selftest: BEGIN/END accumulator over /tmp/awk_num.txt:\n");
    run_awk("BEGIN{ t = 10 } { t += $1 } END{ print \"total=\" t }",
            "/tmp/awk_num.txt", 0);

    /* Verifications. */
    int ok = selftest_fields()
          && selftest_sum()
          && selftest_cond()
          && selftest_beginend();

    if (ok) out("AWK SELFTEST: PASS\n");
    else    out("AWK SELFTEST: FAIL\n");
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 (userspace/crt0.asm) provides _start and calls main(argc, argv).
 *
 *  With no real arguments (argc <= 1) we run the smoke-gated self-test.
 *  Otherwise we parse the conceptual CLI:
 *      awk [-F SEP] 'PROGRAM' INFILE
 *  where -F takes a single-char separator (default 0 = whitespace).
 * ===================================================================== */
int main(int argc, char **argv) {
    if (argc <= 1) {
        out("[AWK] small awk -- no argv; running self-test\n");
        run_selftest();
        return 0;
    }

    char sep = 0;              /* 0 = whitespace */
    int i = 1;

    /* optional leading -F SEP */
    if (a_streq(argv[i], "-F")) {
        if (i + 1 >= argc) { out("usage: awk [-F SEP] 'PROGRAM' INFILE\n"); return 1; }
        sep = argv[i + 1][0];
        i += 2;
    }

    /* need PROGRAM then INFILE */
    if (i + 1 >= argc) { out("usage: awk [-F SEP] 'PROGRAM' INFILE\n"); return 1; }

    const char *program = argv[i];
    const char *infile  = argv[i + 1];

    return run_awk(program, infile, sep);
}

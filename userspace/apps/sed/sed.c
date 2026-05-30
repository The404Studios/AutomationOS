/*
 * sed.c -- minimal freestanding stream editor for a from-scratch x86_64 OS.
 * =========================================================================
 *
 * A tiny `sed` clone. Ring 3, NO libc: pure inline syscalls + hand-rolled
 * string helpers. Single self-contained file, no headers beyond this one.
 *
 * --------------------------------------------------------------------------
 * USAGE (when argv becomes available -- see the IMPORTANT note below):
 *
 *   sed 's/OLD/NEW/[g]' INFILE [OUTFILE]
 *       Substitute BRE OLD with NEW on every line. Trailing 'g' replaces
 *       ALL occurrences per line; without it only the FIRST per line.
 *       No OUTFILE  -> result written to stdout (fd 1, SYS_WRITE).
 *       -i (before the script) -> edit INFILE in place.
 *
 *   sed -n 's/.../.../p' INFILE          (only print substituted... )    [*]
 *   sed -n '/PAT/p' INFILE               print only lines matching PAT
 *   sed '/PAT/d'     INFILE              delete lines matching PAT
 *   sed 'Nd'         INFILE              delete line number N (1-based)
 *
 *   sed selftest                         run the built-in self-test
 *
 *   [*] -n suppresses auto-print; a `p` flag/command then prints explicitly.
 *
 * Match is a BASIC REGULAR EXPRESSION (BRE): . * ^ $ [..] [^..] \( \) \1
 * and \-escapes; a pattern with no metacharacters behaves like a literal
 * substring. In the replacement, & = whole match and \1..\9 = captures.
 * 'g' = global per line.
 *
 * --------------------------------------------------------------------------
 * IMPORTANT -- argv availability on THIS kernel:
 *
 *   I checked kernel/fs/exec.c (elf_load_and_exec) and
 *   kernel/core/syscall/handlers.c (sys_spawn). SYS_SPAWN takes ONLY a path;
 *   args 2..6 are explicitly ignored. elf_load_and_exec() builds the new
 *   process with a BARE stack -- it does NOT push argc/argv/envp. Every
 *   spawnable userspace app uses `void _start(void)` with no parameters, and
 *   there is no agreed-upon convention for finding arguments on the stack.
 *
 *   => ARGV IS NOT AVAILABLE TO SPAWNED APPS.
 *
 *   Therefore _start() runs the SELF-TEST unconditionally so the program is
 *   still verifiable end-to-end. All real sed logic lives in sed_run(argc,
 *   argv) and the pieces it calls, so the day the kernel starts passing an
 *   argv (push it on the new stack and have _start read it), wiring this up
 *   is a one-line change in _start(): parse the real argv and call sed_run().
 *
 * --------------------------------------------------------------------------
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/sed/sed.c -o sed.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       sed.o -o build/sed
 *   objdump -d build/sed | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers -- verified against kernel/include/syscall.h.
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_UNLINK 34

/* open() flags (kernel/include/vfs.h). */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

/* The kernel copies MAX_PATH_LEN (4096) bytes out of a path pointer in
 * sys_open(); any path we hand it must have that many readable bytes behind
 * it, so paths always live in statically sized buffers (never bare short
 * literals). KPATH_MAX matches that copy length. */
#define KPATH_MAX 4096

typedef unsigned long size_t;

/* -----------------------------------------------------------------------
 * Inline syscall (args rdi/rsi/rdx). Three args cover everything we need.
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding string / memory helpers (our own libc).
 * ======================================================================= */

static size_t s_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void s_memcpy(char *dst, const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

static int s_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Copy src into dst (cap includes the NUL). Returns length written. */
static size_t s_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* =======================================================================
 *  Output helpers (everything -- output AND diagnostics -- goes to fd 1).
 * ======================================================================= */

static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n); }
static void out(const char *s)             { out_n(s, s_strlen(s)); }

/* =======================================================================
 *  Buffers. The kernel gives each app a tiny (64 KB) stack, so anything
 *  large MUST be static.  We cap input at 64 KB as specified.
 * ======================================================================= */

#define INBUF_MAX  (64 * 1024)
#define OUTBUF_MAX (128 * 1024)   /* a substitution can grow the text */

static char g_in[INBUF_MAX]   __attribute__((aligned(16)));
static char g_out[OUTBUF_MAX] __attribute__((aligned(16)));
static size_t g_out_len;

static char g_path[KPATH_MAX] __attribute__((aligned(16)));  /* safe path buffer */

/* Append into g_out, dropping silently once full (bounded; never overflows). */
static void ob_putc(char c)
{
    if (g_out_len < OUTBUF_MAX) g_out[g_out_len++] = c;
}
static void ob_put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) ob_putc(s[i]);
}

/* =======================================================================
 *  File I/O (mirrors terminal_m3.c's slurp_file / write patterns).
 * ======================================================================= */

/*
 * Read the whole file at `path` into g_in (up to INBUF_MAX).
 * Returns bytes read (>=0), -1 if it could not be opened, -2 if too large.
 * `path` is copied through g_path first so copy_from_user has 4096 readable
 * bytes behind the pointer.
 */
static long slurp(const char *path)
{
    s_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) return -1;

    long total = 0;
    for (;;) {
        long room = INBUF_MAX - total;
        if (room <= 0) {
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1);
            sc(SYS_CLOSE, fd, 0, 0);
            return (n > 0) ? -2 : total;   /* still more bytes => too large */
        }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Write g_out[0..g_out_len) to `path` (truncate/create). Returns 0 / -1. */
static int spill(const char *path)
{
    s_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < (long)g_out_len) {
        long w = sc(SYS_WRITE, fd, (long)(g_out + off), (long)g_out_len - off);
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* =======================================================================
 *  Substring search: index of NUL-terminated `needle` in hay[0..hlen),
 *  starting at `from`. Returns -1 if not found. Empty needle => `from`.
 * ======================================================================= */
static __attribute__((unused)) long find_sub(const char *hay, long hlen, long from, const char *needle)
{
    long nlen = (long)s_strlen(needle);
    if (nlen == 0) return from;
    for (long i = from; i + nlen <= hlen; i++) {
        long j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return i;
    }
    return -1;
}

/* Does line[0..llen) contain `needle`? */
static __attribute__((unused)) int line_has(const char *line, long llen, const char *needle)
{
    return find_sub(line, llen, 0, needle) >= 0;
}

/* =======================================================================
 *  BASIC REGULAR EXPRESSION (BRE) engine.
 *
 *  Self-contained, freestanding, bounded. A BRE pattern string is compiled
 *  into a fixed-size array of tokens, then matched with a bounded recursive
 *  backtracker. No allocation, no libc.
 *
 *  Supported metacharacters:
 *    .            any single char
 *    *            zero-or-more of the preceding atom (.* , c* , [..]* )
 *    ^            anchor: start of line (only special as the first atom)
 *    $            anchor: end of line   (only special as the last atom)
 *    [abc] [a-z]  character class; [^...] negated class
 *    \( \)        capture group open/close (BRE-style, backslash-quoted)
 *    \1 .. \9     backreference to a captured group
 *    \. \* \[ ... escape: the following metachar is taken literally
 *    other chars  literal
 *
 *  A pattern with no metacharacters therefore behaves exactly like a literal
 *  substring match (matched anywhere in the line, leftmost), preserving the
 *  old behaviour for plain text.
 *
 *  In the replacement text, `&` expands to the whole match and `\1`..`\9`
 *  expand to the corresponding capture (handled in process(), not here).
 * ======================================================================= */

#define RE_MAX_TOK   256   /* compiled tokens per pattern (bounded)        */
#define RE_MAX_GROUP 10    /* capture groups 1..9 (index 0 unused)         */

/* Token kinds. */
enum {
    RT_CHAR,    /* match the literal byte in .c                            */
    RT_ANY,     /* match any single byte (.)                               */
    RT_CLASS,   /* match a char class (bitmap in .cls / .neg)              */
    RT_BOL,     /* ^ anchor (matches at start of line, consumes nothing)   */
    RT_EOL,     /* $ anchor (matches at end of line, consumes nothing)     */
    RT_GOPEN,   /* \( open capture group .grp                              */
    RT_GCLOSE,  /* \) close capture group .grp                             */
    RT_BACKREF  /* \N backreference to group .grp                          */
};

typedef struct {
    unsigned char kind;
    unsigned char star;     /* 1 if this atom is followed by '*'           */
    char          c;        /* literal byte (RT_CHAR)                       */
    int           grp;      /* group index (RT_GOPEN/GCLOSE/BACKREF)        */
    unsigned char cls[32];  /* 256-bit membership bitmap (RT_CLASS)         */
    unsigned char neg;      /* negated class (RT_CLASS)                     */
} re_tok;

/* class-bitmap accessors (256 bits packed into 32 bytes) */
static void recls_set(re_tok *t, int c) { t->cls[(c >> 3) & 31] |= (unsigned char)(1u << (c & 7)); }
static int  recls_get(const re_tok *t, int c) { return (t->cls[(c >> 3) & 31] >> (c & 7)) & 1; }

typedef struct {
    re_tok tok[RE_MAX_TOK];
    int    ntok;
    int    ngroup;          /* number of capture groups seen (1-based)     */
    int    ok;              /* compiled cleanly                            */
} re_prog;

/* Capture spans, filled during a successful match. -1 = unset. */
typedef struct {
    long start[RE_MAX_GROUP];
    long end[RE_MAX_GROUP];
} re_caps;

/* Compile BRE `pat` (NUL-terminated) into `pg`. Always returns; sets pg->ok. */
static void re_compile(const char *pat, re_prog *pg)
{
    pg->ntok   = 0;
    pg->ngroup = 0;
    pg->ok     = 1;

    int gstack[RE_MAX_GROUP];   /* open-group index stack                  */
    int gtop = 0;

    const char *p = pat;
    int first = 1;              /* is the next atom the first in the RE?   */

    while (*p) {
        re_tok t;
        /* zero the token (no memset) */
        { char *z = (char *)&t; for (size_t k = 0; k < sizeof(t); k++) z[k] = 0; }

        if (*p == '^' && first) {
            t.kind = RT_BOL; p++;
        } else if (*p == '$' && (p[1] == '\0')) {
            t.kind = RT_EOL; p++;
        } else if (*p == '.') {
            t.kind = RT_ANY; p++;
        } else if (*p == '[') {
            /* character class */
            t.kind = RT_CLASS;
            p++;
            if (*p == '^') { t.neg = 1; p++; }
            /* a ']' immediately after [ or [^ is a literal ']' */
            int firstcls = 1;
            while (*p && (*p != ']' || firstcls)) {
                unsigned char lo = (unsigned char)*p;
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    unsigned char hi = (unsigned char)p[2];
                    if (hi >= lo) for (int c = lo; c <= hi; c++) recls_set(&t, c);
                    else          recls_set(&t, lo);
                    p += 3;
                } else {
                    recls_set(&t, lo);
                    p++;
                }
                firstcls = 0;
            }
            if (*p != ']') { pg->ok = 0; return; }   /* unterminated class */
            p++;
        } else if (*p == '\\' && p[1] == '(') {
            if (gtop >= RE_MAX_GROUP - 1) { pg->ok = 0; return; }
            pg->ngroup++;
            t.kind = RT_GOPEN; t.grp = pg->ngroup;
            gstack[gtop++] = pg->ngroup;
            p += 2;
            /* group-open is not a "*"-able atom; stays first if RE began here */
            if (pg->ntok < RE_MAX_TOK) pg->tok[pg->ntok++] = t;
            continue;
        } else if (*p == '\\' && p[1] == ')') {
            if (gtop <= 0) { pg->ok = 0; return; }
            t.kind = RT_GCLOSE; t.grp = gstack[--gtop];
            p += 2;
            if (pg->ntok < RE_MAX_TOK) pg->tok[pg->ntok++] = t;
            continue;
        } else if (*p == '\\' && p[1] >= '1' && p[1] <= '9') {
            t.kind = RT_BACKREF; t.grp = p[1] - '0';
            p += 2;
        } else if (*p == '\\' && p[1]) {
            /* escaped literal */
            t.kind = RT_CHAR; t.c = p[1];
            p += 2;
        } else {
            t.kind = RT_CHAR; t.c = *p;
            p++;
        }

        /* postfix '*' binds to the atom just parsed (not anchors/groups) */
        if (*p == '*' && (t.kind == RT_CHAR || t.kind == RT_ANY ||
                          t.kind == RT_CLASS)) {
            t.star = 1;
            p++;
        }

        if (pg->ntok >= RE_MAX_TOK) { pg->ok = 0; return; }
        pg->tok[pg->ntok++] = t;
        first = 0;
    }

    if (gtop != 0) pg->ok = 0;   /* unbalanced \( \) */
}

/* Does the single (non-star, non-anchor) token `t` match byte at s[i]?
 * Caller guarantees i < slen. */
static int re_atom_match(const re_tok *t, const char *s, long i)
{
    switch (t->kind) {
    case RT_CHAR:  return s[i] == t->c;
    case RT_ANY:   return 1;
    case RT_CLASS: {
        unsigned char c = (unsigned char)s[i];
        int in = recls_get(t, c);
        return t->neg ? !in : in;
    }
    default: return 0;
    }
}

/* Bounded recursive matcher. Try to match prog->tok[ti..] against s[i..slen).
 * `caps` is updated in place as groups open/close. Returns the end position
 * of the overall match on success (>= i), or -1 on failure. */
static long re_match_here(const re_prog *pg, int ti, const char *s,
                          long slen, long i, re_caps *caps, int depth)
{
    if (depth > 20000) return -1;          /* runaway guard (bounded)      */

    if (ti >= pg->ntok) return i;          /* whole pattern consumed       */

    const re_tok *t = &pg->tok[ti];

    if (t->kind == RT_BOL) {
        /* ^ matched in the matcher entry (we only reach here mid-pattern if
         * ^ appears not-at-start; treat as always-true zero-width). */
        return re_match_here(pg, ti + 1, s, slen, i, caps, depth + 1);
    }
    if (t->kind == RT_EOL) {
        if (i == slen) return re_match_here(pg, ti + 1, s, slen, i, caps, depth + 1);
        return -1;
    }
    if (t->kind == RT_GOPEN) {
        long save = caps->start[t->grp];
        caps->start[t->grp] = i;
        long r = re_match_here(pg, ti + 1, s, slen, i, caps, depth + 1);
        if (r < 0) caps->start[t->grp] = save;
        return r;
    }
    if (t->kind == RT_GCLOSE) {
        long save = caps->end[t->grp];
        caps->end[t->grp] = i;
        long r = re_match_here(pg, ti + 1, s, slen, i, caps, depth + 1);
        if (r < 0) caps->end[t->grp] = save;
        return r;
    }
    if (t->kind == RT_BACKREF) {
        int g = t->grp;
        if (g >= RE_MAX_GROUP || caps->start[g] < 0 || caps->end[g] < 0)
            return -1;
        long blen = caps->end[g] - caps->start[g];
        if (blen < 0 || i + blen > slen) return -1;
        for (long k = 0; k < blen; k++)
            if (s[i + k] != s[caps->start[g] + k]) return -1;
        return re_match_here(pg, ti + 1, s, slen, i + blen, caps, depth + 1);
    }

    if (t->star) {
        /* greedy: match as many as possible, then backtrack. */
        long count = 0;
        while (i + count < slen && re_atom_match(t, s, i + count)) count++;
        for (long k = count; k >= 0; k--) {
            long r = re_match_here(pg, ti + 1, s, slen, i + k, caps, depth + 1);
            if (r >= 0) return r;
        }
        return -1;
    }

    /* single mandatory atom */
    if (i < slen && re_atom_match(t, s, i))
        return re_match_here(pg, ti + 1, s, slen, i + 1, caps, depth + 1);
    return -1;
}

/* Attempt a match of `pg` anywhere in s[0..slen), at or after `from`.
 * On success returns the start offset and fills *mend with the match end and
 * *caps with capture spans; returns -1 if no match. Honours a leading ^ by
 * only trying position 0 (or `from` if it is 0). */
static long re_search(const re_prog *pg, const char *s, long slen, long from,
                      long *mend, re_caps *caps)
{
    int anchored_bol = (pg->ntok > 0 && pg->tok[0].kind == RT_BOL);

    for (long start = from; start <= slen; start++) {
        if (anchored_bol && start != 0) break;   /* ^ only at line start  */

        /* reset captures for this attempt */
        for (int g = 0; g < RE_MAX_GROUP; g++) { caps->start[g] = -1; caps->end[g] = -1; }

        int ti = anchored_bol ? 1 : 0;            /* ^ already satisfied   */
        long r = re_match_here(pg, ti, s, slen, start, caps, 0);
        if (r >= 0) {
            caps->start[0] = start;               /* group 0 = whole match */
            caps->end[0]   = r;
            *mend = r;
            return start;
        }
        if (anchored_bol) break;
    }
    return -1;
}

/* Does line[0..llen) match BRE `pat` anywhere? (used for /pat/ addresses) */
static int line_re_match(const char *line, long llen, const char *pat)
{
    static re_prog pg;   /* static: too large for the 64KB stack */
    re_compile(pat, &pg);
    if (!pg.ok) return 0;
    re_caps caps;
    long mend;
    return re_search(&pg, line, llen, 0, &mend, &caps) >= 0;
}

/* =======================================================================
 *  Script representation.
 *
 *  We support exactly the documented subset. The parser fills one of these.
 * ======================================================================= */

typedef enum { OP_SUBST, OP_DELETE, OP_PRINT } sed_op;

typedef struct {
    sed_op op;
    /* substitution */
    char   pat[256];        /* OLD / search text                    */
    char   rep[256];        /* NEW / replacement text               */
    int    global;          /* s///g : all matches per line         */
    /* addressing for d / p */
    long   line_no;         /* >0 : address a specific 1-based line  */
    char   addr_pat[256];   /* non-empty : address by /pattern/      */
    int    has_addr_pat;
    /* global flag from the -n option */
    int    suppress;        /* -n : suppress auto-print              */
    int    p_flag;          /* substitution 'p' flag (with -n)       */
} sed_script;

/* Parse a `s/OLD/NEW/flags` command. `delim` is taken from script[1] (we
 * only document '/', but honour whatever char follows the 's'). Returns 0
 * on success, -1 on a malformed script. */
static int parse_subst(const char *s, sed_script *sp)
{
    sp->op = OP_SUBST;
    sp->global = 0;
    sp->p_flag = 0;
    char delim = s[1];
    if (delim == '\0') return -1;
    const char *p = s + 2;

    /* OLD up to next unescaped delim */
    int i = 0;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1] == delim) p++;      /* allow \<delim> literal */
        if (i < (int)sizeof(sp->pat) - 1) sp->pat[i++] = *p;
        p++;
    }
    sp->pat[i] = '\0';
    if (*p != delim) return -1;
    p++;

    /* NEW up to next unescaped delim */
    i = 0;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1] == delim) p++;
        if (i < (int)sizeof(sp->rep) - 1) sp->rep[i++] = *p;
        p++;
    }
    sp->rep[i] = '\0';
    if (*p != delim) return -1;
    p++;

    /* flags */
    while (*p) {
        if (*p == 'g') sp->global = 1;
        else if (*p == 'p') sp->p_flag = 1;
        p++;
    }
    return 0;
}

/* Parse the address-form scripts: `Nd`, `/PAT/d`, `/PAT/p`. Returns 0/-1. */
static int parse_addr_cmd(const char *s, sed_script *sp)
{
    sp->line_no = 0;
    sp->has_addr_pat = 0;
    sp->addr_pat[0] = '\0';

    if (*s == '/') {
        /* /PAT/<cmd> */
        const char *p = s + 1;
        int i = 0;
        while (*p && *p != '/') {
            if (i < (int)sizeof(sp->addr_pat) - 1) sp->addr_pat[i++] = *p;
            p++;
        }
        sp->addr_pat[i] = '\0';
        if (*p != '/') return -1;
        p++;
        sp->has_addr_pat = 1;
        if (*p == 'd') { sp->op = OP_DELETE; return 0; }
        if (*p == 'p') { sp->op = OP_PRINT;  return 0; }
        return -1;
    }

    /* Nd : leading digits then 'd' */
    if (*s >= '0' && *s <= '9') {
        long n = 0;
        const char *p = s;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (*p == 'd') { sp->op = OP_DELETE; sp->line_no = n; return 0; }
        return -1;
    }
    return -1;
}

/* Dispatch a raw script string into sp. Returns 0/-1. */
static int parse_script(const char *s, sed_script *sp)
{
    if (s[0] == 's') return parse_subst(s, sp);
    return parse_addr_cmd(s, sp);
}

/* =======================================================================
 *  Core processing: run `sp` over g_in[0..in_len) into g_out.
 *
 *  Lines are delimited by '\n'. The trailing newline (if any) is preserved
 *  per line. A final line without a newline is emitted without one.
 * ======================================================================= */
/* Expand a substitution replacement template into g_out. `&` -> whole match,
 * `\1`..`\9` -> captured group, `\&` -> literal '&', `\\` -> literal '\'.
 * `line` is the current line text; `caps` holds match spans for this match. */
static void emit_replacement(const char *rep, const char *line,
                             const re_caps *caps)
{
    for (const char *r = rep; *r; r++) {
        if (*r == '&') {
            long s = caps->start[0], e = caps->end[0];
            if (s >= 0 && e >= s) ob_put(line + s, e - s);
        } else if (*r == '\\' && r[1] >= '1' && r[1] <= '9') {
            int g = r[1] - '0';
            r++;
            if (g < RE_MAX_GROUP && caps->start[g] >= 0 && caps->end[g] >= caps->start[g])
                ob_put(line + caps->start[g], caps->end[g] - caps->start[g]);
        } else if (*r == '\\' && r[1]) {
            ob_putc(r[1]);   /* \&  \\  etc. -> literal next char */
            r++;
        } else {
            ob_putc(*r);
        }
    }
}

static void process(const sed_script *sp, long in_len)
{
    g_out_len = 0;
    long start = 0;
    long lineno = 0;

    /* Pre-compile the substitution / address regex once. Kept static: the
     * app is single-threaded and re_prog is too large for the 64KB stack. */
    static re_prog subst_pg;
    int subst_compiled = 0;
    if (sp->op == OP_SUBST && s_strlen(sp->pat) > 0) {
        re_compile(sp->pat, &subst_pg);
        subst_compiled = subst_pg.ok;
    }

    while (start <= in_len) {
        /* find end of this line (exclusive of '\n') */
        long e = start;
        while (e < in_len && g_in[e] != '\n') e++;
        int had_nl = (e < in_len);          /* line ended with '\n' */
        long llen = e - start;
        const char *line = g_in + start;
        lineno++;

        /* If we've consumed everything and there was no dangling content,
         * stop. (start == in_len with had_nl==0 and llen==0 means we already
         * emitted the last newline-terminated line.) */
        if (start == in_len && in_len > 0 && g_in[in_len - 1] == '\n') break;

        int emit = 1;          /* auto-print this line?                */
        int forced_print = 0;  /* p flag/command forces a print        */

        switch (sp->op) {
        case OP_SUBST: {
            /* Build the (possibly) substituted line into g_out directly,
             * using the BRE engine for the search side and `&`/`\N` expansion
             * for the replacement side. */
            long pos = 0;
            int did = 0;
            long line_out_start = g_out_len;
            if (!subst_compiled) {
                /* empty / unparseable pattern: no-op copy */
                ob_put(line, llen);
            } else {
                while (pos <= llen) {
                    re_caps caps;
                    long mend;
                    long m = re_search(&subst_pg, line, llen, pos, &mend, &caps);
                    if (m < 0) { ob_put(line + pos, llen - pos); break; }
                    ob_put(line + pos, m - pos);              /* prefix      */
                    emit_replacement(sp->rep, line, &caps);   /* replacement */
                    did = 1;
                    if (!sp->global) {
                        ob_put(line + mend, llen - mend);     /* rest        */
                        break;
                    }
                    if (mend == m) {
                        /* empty match: emit one byte to make progress */
                        if (m < llen) ob_putc(line[m]);
                        pos = m + 1;
                    } else {
                        pos = mend;
                    }
                    if (pos > llen) break;
                }
            }
            /* newline handling */
            if (had_nl) ob_putc('\n');
            /* with -n, suppress the auto-print we just did unless 'p' set;
             * since we already wrote into g_out, handle -n by rewinding. */
            if (sp->suppress) {
                if (did && sp->p_flag) {
                    /* keep it (it was the substituted line) */
                } else {
                    g_out_len = line_out_start;   /* discard this line */
                }
            }
            emit = 0;   /* already emitted (or discarded) above */
            break;
        }
        case OP_DELETE: {
            int del = 0;
            if (sp->line_no > 0) del = (lineno == sp->line_no);
            else if (sp->has_addr_pat) del = line_re_match(line, llen, sp->addr_pat);
            emit = !del;
            break;
        }
        case OP_PRINT: {
            /* /PAT/p : with -n, print only matching lines. Without -n, sed
             * prints matching lines an EXTRA time (auto-print + p). We model
             * the common `-n .../p` use: matching => print, else nothing. */
            int match = sp->has_addr_pat ? line_re_match(line, llen, sp->addr_pat) : 1;
            if (sp->suppress) {
                emit = 0;
                forced_print = match;
            } else {
                emit = 1;
                forced_print = match;   /* duplicate matching line */
            }
            break;
        }
        }

        if (emit) {
            ob_put(line, llen);
            if (had_nl) ob_putc('\n');
        }
        if (forced_print) {
            ob_put(line, llen);
            if (had_nl) ob_putc('\n');
        }

        if (!had_nl) break;        /* last line had no newline => done */
        start = e + 1;
    }
}

/* =======================================================================
 *  sed_run -- the real entry point a future argv hookup would call.
 *
 *  argv layout (sed-style):
 *    argv[0] = "sed"
 *    optional "-n" and/or "-i" flags (any order before the script)
 *    next non-flag arg = SCRIPT  ('s/.../.../' | 'Nd' | '/pat/d' | '/pat/p')
 *    next arg          = INFILE
 *    optional next arg = OUTFILE
 *
 *  Returns a process exit code (0 = success).
 * ======================================================================= */
static int sed_run(int argc, char **argv)
{
    sed_script sp;
    /* zero it (no memset in freestanding without our own; do it inline) */
    {
        char *z = (char *)&sp;
        for (size_t k = 0; k < sizeof(sp); k++) z[k] = 0;
    }

    int in_place = 0;
    sp.suppress = 0;

    int ai = 1;
    /* leading flags */
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (s_streq(argv[ai], "-n"))      sp.suppress = 1;
        else if (s_streq(argv[ai], "-i")) in_place = 1;
        else { out("sed: unknown option: "); out(argv[ai]); out("\n"); return 1; }
        ai++;
    }

    if (ai >= argc || !argv[ai]) {
        out("usage: sed 's/OLD/NEW/[g]' INFILE [OUTFILE]\n");
        return 1;
    }
    const char *script = argv[ai++];

    if (parse_script(script, &sp) != 0) {
        out("sed: bad script: "); out(script); out("\n");
        return 1;
    }

    if (ai >= argc || !argv[ai]) { out("sed: no input file\n"); return 1; }
    const char *infile  = argv[ai++];
    const char *outfile = (ai < argc && argv[ai]) ? argv[ai] : 0;

    long n = slurp(infile);
    if (n == -1) { out("sed: cannot open '"); out(infile); out("'\n"); return 1; }
    if (n == -2) { out("sed: input too large (>64KB)\n"); return 1; }

    process(&sp, n);

    if (in_place)       return spill(infile)  ? 1 : 0;
    else if (outfile)   return spill(outfile) ? 1 : 0;
    else { out_n(g_out, g_out_len); return 0; }
}

/* =======================================================================
 *  SELF-TEST
 *
 *  Creates /tmp/sed_in.txt with known content, runs `s/foo/bar/g` over it,
 *  and verifies g_out byte-for-byte against the expectation. Prints
 *  "SED SELFTEST: PASS" or "SED SELFTEST: FAIL".
 * ======================================================================= */

#define TMP_IN "/tmp/sed_in.txt"

static int bytes_eq(const char *a, long alen, const char *b, long blen)
{
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Run a single-line `script` over `input` (in memory, no files) and compare
 * the produced g_out against `expect`. Returns 1 on pass, 0 on fail and
 * prints a diagnostic. `nflag` sets the -n (suppress) flag. */
static int re_case(const char *script, const char *input,
                   const char *expect, int nflag)
{
    long ilen = (long)s_strlen(input);
    if (ilen >= INBUF_MAX) return 0;
    s_memcpy(g_in, input, (size_t)ilen);

    sed_script sp;
    { char *z = (char *)&sp; for (size_t k = 0; k < sizeof(sp); k++) z[k] = 0; }
    sp.suppress = nflag;
    if (parse_script(script, &sp) != 0) {
        out("  re_case PARSE-FAIL: "); out(script); out("\n");
        return 0;
    }
    process(&sp, ilen);

    long elen = (long)s_strlen(expect);
    if (bytes_eq(g_out, (long)g_out_len, expect, elen)) return 1;

    out("  re_case FAIL script="); out(script); out("\n");
    out("    got:  "); out_n(g_out, g_out_len); out("\n");
    out("    want: "); out(expect);             out("\n");
    return 0;
}

static int selftest(void)
{
    out("SED SELFTEST: begin\n");

    /* 1) Write a known input file via the same syscalls sed uses. */
    const char *content =
        "foo bar foo\n"      /* two foo -> with /g both replaced       */
        "no match here\n"    /* unchanged                              */
        "foofoo end\n";      /* adjacent foos                          */
    s_strlcpy(g_path, TMP_IN, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { out("SED SELFTEST: FAIL (cannot create temp file)\n"); return 1; }
    {
        long len = (long)s_strlen(content), off = 0;
        while (off < len) {
            long w = sc(SYS_WRITE, fd, (long)(content + off), len - off);
            if (w <= 0) break;
            off += w;
        }
    }
    sc(SYS_CLOSE, fd, 0, 0);

    /* 2) Build argv and run sed: s/foo/bar/g over the temp file (to g_out). */
    char a0[] = "sed";
    char a1[] = "s/foo/bar/g";
    char a2[KPATH_MAX];
    s_strlcpy(a2, TMP_IN, sizeof(a2));
    char *argv[] = { a0, a1, a2, 0 };

    /* sed_run with no OUTFILE writes to fd1; for the test we want the bytes,
     * so we replicate its core path: slurp + process, then compare g_out. */
    long n = slurp(TMP_IN);
    if (n < 0) { out("SED SELFTEST: FAIL (cannot read temp file)\n"); return 1; }

    sed_script sp;
    { char *z = (char *)&sp; for (size_t k = 0; k < sizeof(sp); k++) z[k] = 0; }
    sp.suppress = 0;
    if (parse_script(a1, &sp) != 0) { out("SED SELFTEST: FAIL (parse)\n"); return 1; }
    process(&sp, n);

    const char *expect =
        "bar bar bar\n"
        "no match here\n"
        "barbar end\n";
    long elen = (long)s_strlen(expect);

    int ok = bytes_eq(g_out, (long)g_out_len, expect, elen);

    /* 2b) BRE engine cases (in-memory; verify regex metacharacters). These
     * gate the smoke test, so the regex engine is exercised on every boot. */

    /* '[0-9]*' (zero-or-more): non-global, matches empty run at line start. */
    ok &= re_case("s/[0-9]*/N/", "abc123def\n", "Nabc123def\n", 0);
    /* ^...$ anchors with .* : whole line a...z becomes MATCH. */
    ok &= re_case("s/^a.*z$/MATCH/", "abcz\n", "MATCH\n", 0);
    /* anchored pattern that should NOT match (no trailing z) -> unchanged. */
    ok &= re_case("s/^a.*z$/MATCH/", "abcd\n", "abcd\n", 0);
    /* negated class: replace each non-vowel-run... here single char class. */
    ok &= re_case("s/[aeiou]/_/g", "regex\n", "r_g_x\n", 0);
    /* escaped metachar: literal dot only, not "any char". */
    ok &= re_case("s/a\\.b/X/", "a.b axb\n", "X axb\n", 0);
    /* '&' in replacement = whole match (wrap matched digits in <>). */
    ok &= re_case("s/[0-9][0-9]*/<&>/", "id42x\n", "id<42>x\n", 0);
    /* capture + backref \1 in replacement (NICE-TO-HAVE): swap two words. */
    ok &= re_case("s/\\(a*\\)\\(b*\\)/\\2\\1/", "aabbb\n", "bbbaa\n", 0);
    /* plain literal pattern (no metachars) still behaves like before. */
    ok &= re_case("s/foo/bar/g", "foofoo\n", "barbar\n", 0);

    /* 3) Also exercise the full sed_run() path end-to-end (writes to fd1). */
    out("SED SELFTEST: sed_run output ->\n");
    (void)sed_run(3, argv);   /* prints transformed text to stdout */
    out("\n");

    /* 4) Clean up the temp file (best effort). */
    s_strlcpy(g_path, TMP_IN, KPATH_MAX);
    sc(SYS_UNLINK, (long)g_path, 0, 0);

    if (ok) { out("SED SELFTEST: PASS\n"); return 0; }
    out("SED SELFTEST: FAIL (output mismatch)\n");
    out("  got: ");      out_n(g_out, g_out_len); out("\n");
    out("  want: ");     out(expect);             out("\n");
    return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0.asm (userspace/crt0.asm) provides _start, reads argc/argv off the
 *  kernel-prepared stack, calls main(argc, argv), and feeds main's return
 *  value to SYS_EXIT. We therefore define main() here (NOT _start).
 *
 *  With real arguments (argc > 1) we dispatch to the actual sed logic via
 *  sed_run(argc, argv) -- so `sed 's/old/new/' INFILE [OUTFILE]` works, and
 *  sed_run itself handles any leading -n/-i flags. With no arguments
 *  (argc <= 1) we fall back to the built-in self-test so the program stays
 *  verifiable end-to-end (it prints "SED SELFTEST: PASS/FAIL").
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) {
        return sed_run(argc, argv);
    }
    (void)selftest();
    return 0;
}

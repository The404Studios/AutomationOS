/*
 * make.c -- a minimal `make` for this from-scratch x86_64 OS.
 * ==========================================================================
 *
 * FREESTANDING userspace ELF: no libc, pure inline syscalls, `void _start(void)`.
 * Build EXACTLY like the other apps (flags DIRECTLY on the command line so
 * -fno-stack-protector is never dropped, else the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/make/make.c -o make.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       make.o -o build/make
 *   objdump -d build/make | grep fs:0x28      # MUST be empty
 *
 * --------------------------------------------------------------------------
 * WHAT IT DOES
 * --------------------------------------------------------------------------
 * Reads a Makefile (default "/Makefile", falling back to "Makefile") fully
 * into a static buffer and parses a MINIMAL make subset:
 *
 *   - Rules:        `target: dep1 dep2 ...`
 *                   followed by recipe lines indented with a literal TAB.
 *   - Variables:    `VAR = value`  (also `VAR := value`)  and `$(VAR)` /
 *                   `${VAR}` expansion in targets, deps and recipes.
 *   - .PHONY:       `.PHONY: name ...`  marks targets as always-out-of-date.
 *   - Default goal: the FIRST real (non-".") target in the file.
 *   - Comments:     lines whose first non-blank char is '#'; and trailing
 *                   ` # ...` on variable/rule lines.
 *
 * --------------------------------------------------------------------------
 * RECIPE EXECUTION (the "spawn model")
 * --------------------------------------------------------------------------
 * This OS has no shell-as-a-library and SYS_SPAWN passes NO argv (see the
 * "ARGV" note below). So each recipe line is executed by:
 *   1. echoing the (expanded) line to fd 1, exactly like real make;
 *   2. taking the FIRST whitespace word as the program;
 *   3. spawning it via SYS_SPAWN of "/bin/<word>" -- unless the word already
 *      contains a '/', in which case it is spawned as that literal path;
 *   4. waiting for it (SYS_WAITPID loop) so recipe lines run in order.
 * Because SYS_SPAWN cannot deliver arguments, only argument-less commands can
 * actually run today; the echo still shows the full intended command line.
 * If a spawn fails, make prints an error and STOPS (real make's behaviour).
 *
 * --------------------------------------------------------------------------
 * DEPENDENCY HANDLING
 * --------------------------------------------------------------------------
 * Deps are built recursively (depth-first) before their target. There is NO
 * timestamp / mtime check: this OS's SYS_STAT has no reliable mtime, so make
 * ALWAYS runs a target's recipe once its deps are done (each target is built
 * at most once per run, tracked by a `built` flag, which also breaks cycles).
 *
 * --------------------------------------------------------------------------
 * SELF-TEST
 * --------------------------------------------------------------------------
 * SYS_SPAWN delivers no argv, so `make` can never see the word "selftest" on a
 * command line. We therefore run the PARSER + DEP-GRAPH self-test
 * UNCONDITIONALLY at startup (it depends on no other programs existing). It
 * builds an in-memory Makefile, parses it, expands a variable, computes the
 * build order for the default goal, and checks both against expected values,
 * printing "MAKE SELFTEST: PASS" or "MAKE SELFTEST: FAIL ...". After the
 * self-test it goes on to build a real "/Makefile" if one is present.
 */

/* ===========================================================================
 *  Syscall numbers  (verified against kernel/include/syscall.h)
 * ========================================================================= */
#define SYS_EXIT     0
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_WAITPID  6
#define SYS_SPAWN    16
#define SYS_YIELD    15

/* O_RDONLY (kernel/include/vfs.h) */
#define O_RDONLY     0x0000

typedef unsigned long      usize;
typedef unsigned char      u8;

/* ---------------------------------------------------------------------------
 * Inline syscall helper (3 args is enough for everything make needs).
 * Same register convention every other freestanding app here uses.
 * ------------------------------------------------------------------------- */
static inline long sc3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ===========================================================================
 *  Tiny freestanding string helpers (self-contained)
 * ========================================================================= */
static usize m_strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }

static int m_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* compare a [start,len) slice of `buf` against NUL-terminated `b` */
static int m_slice_eq(const char *buf, int len, const char *b) {
    int i = 0;
    for (; i < len; i++) { if (b[i] == 0 || buf[i] != b[i]) return 0; }
    return b[i] == 0;
}

/* copy NUL-terminated src into dst (cap incl NUL); returns length written. */
static int m_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static int is_space(char c)  { return c == ' ' || c == '\t'; }
static int is_blank(char c)  { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* ===========================================================================
 *  Output helpers (fd 1)
 * ========================================================================= */
static void out(const char *s)             { sc3(SYS_WRITE, 1, (long)s, (long)m_strlen(s)); }
static void outn(const char *s, long n)     { sc3(SYS_WRITE, 1, (long)s, n); }
static void outc(char c)                     { sc3(SYS_WRITE, 1, (long)&c, 1); }
static void out_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { outc('-'); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) outc(b[--i]);
}

/* ===========================================================================
 *  Data model
 * ========================================================================= */
#define MK_BUF_MAX     (64 * 1024)   /* max Makefile size */
#define MAX_TARGETS    64
#define MAX_DEPS       16
#define MAX_RECIPES    16
#define MAX_VARS       64
#define NAME_MAX_      128
#define VALUE_MAX_     512
#define RECIPE_MAX_    512

typedef struct {
    char name[NAME_MAX_];
    char value[VALUE_MAX_];
} mk_var_t;

typedef struct {
    char  name[NAME_MAX_];
    char  deps[MAX_DEPS][NAME_MAX_];
    int   ndeps;
    char  recipes[MAX_RECIPES][RECIPE_MAX_];   /* RAW (pre-expansion) */
    int   nrecipes;
    int   phony;
    int   built;       /* set once its recipe has run (also breaks cycles) */
    int   building;    /* on the current recursion stack (cycle guard) */
} mk_target_t;

typedef struct {
    char         src[MK_BUF_MAX];   /* the Makefile text */
    long         srclen;
    mk_var_t     vars[MAX_VARS];
    int          nvars;
    mk_target_t  tgt[MAX_TARGETS];
    int          ntgt;
    int          first_real;        /* index of default goal, -1 if none */
    char         expand_scratch[VALUE_MAX_ + RECIPE_MAX_];
} mk_ctx_t;

/* Two contexts: one for the live build, one for the self-test, kept static
 * (they are large; the user stack is only 64 KB). */
static mk_ctx_t g_ctx;
static mk_ctx_t g_test;

/* ===========================================================================
 *  Variable store
 * ========================================================================= */
static int var_find(mk_ctx_t *c, const char *name) {
    for (int i = 0; i < c->nvars; i++)
        if (m_streq(c->vars[i].name, name)) return i;
    return -1;
}

static void var_set(mk_ctx_t *c, const char *name, const char *value) {
    if (!name[0]) return;
    int i = var_find(c, name);
    if (i < 0) {
        if (c->nvars >= MAX_VARS) return;
        i = c->nvars++;
        m_strlcpy(c->vars[i].name, name, NAME_MAX_);
    }
    m_strlcpy(c->vars[i].value, value, VALUE_MAX_);
}

static const char *var_get(mk_ctx_t *c, const char *name) {
    int i = var_find(c, name);
    return (i >= 0) ? c->vars[i].value : "";
}

/*
 * Expand $(VAR) and ${VAR} references in `in` into `out` (cap bytes incl NUL).
 * Unknown variables expand to empty (like make). A single, non-recursive pass
 * is performed (sufficient for the simple var usage this make supports).
 */
static void expand(mk_ctx_t *c, const char *in, char *out, int cap) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        if (in[i] == '$' && (in[i+1] == '(' || in[i+1] == '{')) {
            char close = (in[i+1] == '(') ? ')' : '}';
            i += 2;
            char vname[NAME_MAX_];
            int vn = 0;
            while (in[i] && in[i] != close && vn < NAME_MAX_ - 1) vname[vn++] = in[i++];
            vname[vn] = '\0';
            if (in[i] == close) i++;
            const char *val = var_get(c, vname);
            for (int k = 0; val[k] && o < cap - 1; k++) out[o++] = val[k];
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* ===========================================================================
 *  Target store
 * ========================================================================= */
static int tgt_find(mk_ctx_t *c, const char *name) {
    for (int i = 0; i < c->ntgt; i++)
        if (m_streq(c->tgt[i].name, name)) return i;
    return -1;
}

/* get-or-create a target by name; returns index, or -1 if the table is full. */
static int tgt_get(mk_ctx_t *c, const char *name) {
    int i = tgt_find(c, name);
    if (i >= 0) return i;
    if (c->ntgt >= MAX_TARGETS) return -1;
    i = c->ntgt++;
    mk_target_t *t = &c->tgt[i];
    m_strlcpy(t->name, name, NAME_MAX_);
    t->ndeps = 0; t->nrecipes = 0;
    t->phony = 0; t->built = 0; t->building = 0;
    return i;
}

/* ===========================================================================
 *  Parser
 *
 *  Line-oriented. State machine:
 *    - A line whose first char is TAB is a recipe line for the CURRENT target.
 *    - Otherwise it is a "logical" line: blank/comment, a variable assignment
 *      (contains '=' before any ':'), or a rule (contains ':').
 *  ".PHONY: a b" sets the phony flag on each named target (creating them).
 * ========================================================================= */

/* Trim trailing blanks of a [start, *len) span by decrementing *len. */
static void rtrim(const char *s, int *len) {
    while (*len > 0 && is_blank(s[*len - 1])) (*len)--;
}

/* Skip a comment: returns the effective length of the line before a ' #'/'#'.
 * We only strip '#' that begins a token (preceded by start or whitespace), to
 * avoid mangling things like URLs -- though this simple make rarely sees them. */
static int strip_comment(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (s[i] == '#' && (i == 0 || is_space(s[i-1]))) return i;
    }
    return len;
}

static void parse(mk_ctx_t *c) {
    c->nvars = 0;
    c->ntgt = 0;
    c->first_real = -1;

    const char *p = c->src;
    long n = c->srclen;
    long i = 0;
    int cur_tgt = -1;

    while (i < n) {
        /* isolate one physical line [line, line+ll) (no newline) */
        long start = i;
        while (i < n && c->src[i] != '\n') i++;
        const char *line = c->src + start;
        int ll = (int)(i - start);
        if (i < n) i++;                 /* consume '\n' */
        if (ll > 0 && line[ll - 1] == '\r') ll--;  /* strip CR */

        /* --- recipe line: starts with a literal TAB --- */
        if (ll > 0 && line[0] == '\t') {
            int rl = ll - 1;            /* skip the leading TAB */
            const char *r = line + 1;
            /* a recipe that is only whitespace is ignored */
            int allblank = 1;
            for (int k = 0; k < rl; k++) if (!is_blank(r[k])) { allblank = 0; break; }
            if (allblank) continue;
            if (cur_tgt >= 0) {
                mk_target_t *t = &c->tgt[cur_tgt];
                if (t->nrecipes < MAX_RECIPES) {
                    int w = m_strlcpy(t->recipes[t->nrecipes], r,
                                      rl + 1 < RECIPE_MAX_ ? rl + 1 : RECIPE_MAX_);
                    (void)w;
                    t->nrecipes++;
                }
            }
            continue;
        }

        /* --- logical line: trim, drop comments / blanks --- */
        int el = strip_comment(line, ll);
        rtrim(line, &el);
        /* skip leading blanks for inspection */
        int s0 = 0;
        while (s0 < el && is_space(line[s0])) s0++;
        if (s0 >= el) { cur_tgt = -1; continue; }   /* blank line ends recipe scope */
        if (line[s0] == '#') { continue; }          /* full-line comment */

        /* find first ':' and first '=' in the trimmed content */
        int colon = -1, eq = -1;
        for (int k = s0; k < el; k++) {
            if (line[k] == ':' && colon < 0) colon = k;
            if (line[k] == '=' && eq < 0)    eq = k;
        }

        /* Variable assignment: '=' appears, and either there's no ':' or the
         * '=' comes before it (so "a = b" is a var, "a: b" is a rule). Also
         * accept ":=" by treating a ':' immediately before '=' as part of it. */
        int is_assign = (eq >= 0) && (colon < 0 || eq < colon || (colon == eq - 1));
        if (is_assign) {
            /* name = trimmed token left of '='; allow trailing ':' for ":=" */
            int nameend = eq;
            if (nameend > s0 && line[nameend - 1] == ':') nameend--;   /* ":=" */
            int ns = s0, ne = nameend;
            while (ns < ne && is_space(line[ns]))     ns++;
            while (ne > ns && is_space(line[ne - 1])) ne--;
            char name[NAME_MAX_];
            int nn = ne - ns; if (nn > NAME_MAX_ - 1) nn = NAME_MAX_ - 1;
            for (int k = 0; k < nn; k++) name[k] = line[ns + k];
            name[nn] = '\0';

            /* value = trimmed text right of '=' (raw; expanded at use sites) */
            int vs = eq + 1;
            while (vs < el && is_space(line[vs])) vs++;
            char value[VALUE_MAX_];
            int vn = el - vs; if (vn > VALUE_MAX_ - 1) vn = VALUE_MAX_ - 1;
            for (int k = 0; k < vn; k++) value[k] = line[vs + k];
            value[vn] = '\0';

            var_set(c, name, value);
            cur_tgt = -1;
            continue;
        }

        /* Rule line: "targets : deps". (No '=' before ':'.) */
        if (colon >= 0) {
            /* split target side [s0, colon) and dep side (colon, el) */
            /* Expand both sides so $(VAR) in targets/deps works. */
            char tside_raw[VALUE_MAX_], dside_raw[VALUE_MAX_];
            int tn = colon - s0; if (tn > VALUE_MAX_ - 1) tn = VALUE_MAX_ - 1;
            for (int k = 0; k < tn; k++) tside_raw[k] = line[s0 + k];
            tside_raw[tn] = '\0';
            int dn = el - (colon + 1); if (dn > VALUE_MAX_ - 1) dn = VALUE_MAX_ - 1;
            for (int k = 0; k < dn; k++) dside_raw[k] = line[colon + 1 + k];
            dside_raw[dn] = '\0';

            char tside[VALUE_MAX_], dside[VALUE_MAX_];
            expand(c, tside_raw, tside, sizeof(tside));
            expand(c, dside_raw, dside, sizeof(dside));

            /* ".PHONY: t1 t2" -- mark each named target phony */
            {
                int q = 0; while (tside[q] && is_space(tside[q])) q++;
                if (m_streq(tside + q, ".PHONY")) {
                    const char *d = dside;
                    while (*d) {
                        while (*d && is_blank(*d)) d++;
                        if (!*d) break;
                        char tok[NAME_MAX_]; int tk = 0;
                        while (*d && !is_blank(*d) && tk < NAME_MAX_ - 1) tok[tk++] = *d++;
                        tok[tk] = '\0';
                        int ti = tgt_get(c, tok);
                        if (ti >= 0) c->tgt[ti].phony = 1;
                    }
                    cur_tgt = -1;
                    continue;
                }
            }

            /* Normal rule: one or more space-separated targets (we honour the
             * first as the recipe owner; extra targets share the deps). */
            int last_tgt = -1;
            const char *t = tside;
            while (*t) {
                while (*t && is_blank(*t)) t++;
                if (!*t) break;
                char tok[NAME_MAX_]; int tk = 0;
                while (*t && !is_blank(*t) && tk < NAME_MAX_ - 1) tok[tk++] = *t++;
                tok[tk] = '\0';
                int ti = tgt_get(c, tok);
                if (ti < 0) continue;
                last_tgt = ti;
                /* record first real (non-".") target as the default goal */
                if (c->first_real < 0 && tok[0] != '.') c->first_real = ti;

                /* attach deps */
                const char *d = dside;
                while (*d) {
                    while (*d && is_blank(*d)) d++;
                    if (!*d) break;
                    char dep[NAME_MAX_]; int dk = 0;
                    while (*d && !is_blank(*d) && dk < NAME_MAX_ - 1) dep[dk++] = *d++;
                    dep[dk] = '\0';
                    mk_target_t *tt = &c->tgt[ti];
                    if (tt->ndeps < MAX_DEPS) m_strlcpy(tt->deps[tt->ndeps++], dep, NAME_MAX_);
                }
            }
            cur_tgt = last_tgt;   /* recipe lines that follow belong here */
            continue;
        }

        /* anything else: ignore, and end recipe scope */
        cur_tgt = -1;
    }
}

/* ===========================================================================
 *  Build (recipe execution via SYS_SPAWN)
 * ========================================================================= */

/*
 * run_recipe_line -- echo the (expanded) recipe line, then spawn its first
 * word. Returns the spawned PID on success (>0), or <=0 on spawn failure.
 * Path: "/bin/<word>" unless <word> already contains '/', in which case it is
 * spawned literally. SYS_SPAWN copies 127 bytes, so the path buffer is 128.
 */
static char g_spawn_path[128];

static long run_recipe_line(mk_ctx_t *c, const char *raw) {
    char expanded[RECIPE_MAX_];
    expand(c, raw, expanded, sizeof(expanded));

    /* echo, exactly like real make */
    out(expanded);
    outc('\n');

    /* first word */
    const char *p = expanded;
    while (*p && is_space(*p)) p++;
    char word[128]; int wl = 0;
    int has_slash = 0;
    while (*p && !is_space(*p) && wl < (int)sizeof(word) - 1) {
        if (*p == '/') has_slash = 1;
        word[wl++] = *p++;
    }
    word[wl] = '\0';
    if (wl == 0) return 1;   /* empty recipe -> treat as success */

    for (int k = 0; k < (int)sizeof(g_spawn_path); k++) g_spawn_path[k] = '\0';
    if (has_slash) {
        m_strlcpy(g_spawn_path, word, sizeof(g_spawn_path));
    } else {
        m_strlcpy(g_spawn_path, "/bin/", sizeof(g_spawn_path));
        m_strlcpy(g_spawn_path + 5, word, (int)sizeof(g_spawn_path) - 5);
    }

    long pid = sc3(SYS_SPAWN, (long)g_spawn_path, 0, 0);
    return pid;
}

/*
 * build(): recursively build all deps, then run this target's recipes.
 * No mtime check (this OS's SYS_STAT lacks reliable mtime) -- each target is
 * built at most once per run (the `built` flag), in dependency order.
 * Returns 0 on success, -1 on error (and the caller stops, like make).
 */
static int build(mk_ctx_t *c, int ti) {
    mk_target_t *t = &c->tgt[ti];
    if (t->built) return 0;
    if (t->building) {
        out("make: circular dependency dropped for '"); out(t->name); out("'\n");
        return 0;   /* break the cycle, don't fail the build */
    }
    t->building = 1;

    /* build deps first (depth-first, left-to-right) */
    for (int d = 0; d < t->ndeps; d++) {
        int di = tgt_find(c, t->deps[d]);
        if (di >= 0) {
            if (build(c, di) != 0) { t->building = 0; return -1; }
        }
        /* a dep with no rule is treated as an existing prerequisite (file),
         * so we simply proceed -- matching make's behaviour for source files. */
    }

    /* run this target's recipes */
    for (int r = 0; r < t->nrecipes; r++) {
        long pid = run_recipe_line(c, t->recipes[r]);
        if (pid <= 0) {
            out("make: *** spawn failed for recipe of target '");
            out(t->name);
            out("' (err "); out_num(pid); out(").  Stop.\n");
            t->building = 0;
            return -1;
        }
        /* wait for the recipe command to finish so lines run in order */
        int status = 0;
        for (long tries = 0; tries < 100000000L; tries++) {
            long w = sc3(SYS_WAITPID, pid, (long)&status, 0);
            if (w == pid) break;
            if (w < 0) break;                 /* no such child / error */
            sc3(SYS_YIELD, 0, 0, 0);
        }
    }

    t->built = 1;
    t->building = 0;
    return 0;
}

/* ===========================================================================
 *  Makefile loading
 * ========================================================================= */
static long load_file(const char *path, char *buf, long cap) {
    long fd = sc3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap) {
        long n = sc3(SYS_READ, fd, (long)(buf + total), cap - total);
        if (n <= 0) break;
        total += n;
    }
    sc3(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* ===========================================================================
 *  SELF-TEST  (parser + dep-graph; needs no other programs to exist)
 *
 *  In-memory Makefile:
 *      CC = gcc
 *      all: prog
 *      prog: main.o util.o
 *      main.o: main.c
 *      util.o: util.c
 *
 *  Checks:
 *   1) $(CC) expands to "gcc".
 *   2) default goal is "all".
 *   3) "all" depends on "prog"; "prog" depends on "main.o","util.o".
 *   4) the recursive build order (post-order) for the default goal is exactly
 *      [main.o, util.o, prog, all].
 * ========================================================================= */

/* Post-order recorder used only by the self-test (mirrors build()'s order,
 * without spawning anything). Records each target name into `order`. */
static const char *g_order[MAX_TARGETS];
static int         g_norder;

static void record_order(mk_ctx_t *c, int ti) {
    mk_target_t *t = &c->tgt[ti];
    if (t->built || t->building) return;
    t->building = 1;
    for (int d = 0; d < t->ndeps; d++) {
        int di = tgt_find(c, t->deps[d]);
        if (di >= 0) record_order(c, di);
    }
    if (g_norder < MAX_TARGETS) g_order[g_norder++] = t->name;
    t->built = 1;
    t->building = 0;
}

static int selftest(void) {
    static const char *MF =
        "CC = gcc\n"
        "all: prog\n"
        "prog: main.o util.o\n"
        "main.o: main.c\n"
        "util.o: util.c\n";

    /* load the in-memory Makefile into the test context */
    int len = (int)m_strlen(MF);
    for (int i = 0; i < len; i++) g_test.src[i] = MF[i];
    g_test.srclen = len;
    parse(&g_test);

    int ok = 1;
    const char *why = "";

    /* 1) variable expansion */
    {
        char exp[64];
        expand(&g_test, "$(CC)", exp, sizeof(exp));
        if (!m_streq(exp, "gcc")) { ok = 0; why = "var-expand"; }
    }

    /* 2) default goal == "all" */
    if (ok) {
        if (g_test.first_real < 0 ||
            !m_streq(g_test.tgt[g_test.first_real].name, "all")) {
            ok = 0; why = "default-goal";
        }
    }

    /* 3) dependency edges */
    if (ok) {
        int ia = tgt_find(&g_test, "all");
        int ip = tgt_find(&g_test, "prog");
        if (ia < 0 || ip < 0) { ok = 0; why = "missing-target"; }
        else {
            mk_target_t *ta = &g_test.tgt[ia];
            mk_target_t *tp = &g_test.tgt[ip];
            if (ta->ndeps != 1 || !m_streq(ta->deps[0], "prog")) { ok = 0; why = "all-deps"; }
            else if (tp->ndeps != 2 ||
                     !m_streq(tp->deps[0], "main.o") ||
                     !m_streq(tp->deps[1], "util.o")) { ok = 0; why = "prog-deps"; }
        }
    }

    /* 4) build order (post-order) for the default goal */
    if (ok) {
        for (int i = 0; i < g_test.ntgt; i++) { g_test.tgt[i].built = 0; g_test.tgt[i].building = 0; }
        g_norder = 0;
        record_order(&g_test, g_test.first_real);

        static const char *expect[] = { "main.o", "util.o", "prog", "all" };
        if (g_norder != 4) { ok = 0; why = "order-count"; }
        else {
            for (int i = 0; i < 4; i++)
                if (!m_streq(g_order[i], expect[i])) { ok = 0; why = "order-seq"; }
        }
    }

    if (ok) {
        out("MAKE SELFTEST: PASS\n");
        return 0;
    } else {
        out("MAKE SELFTEST: FAIL ("); out(why); out(")\n");
        return 1;
    }
}

/* ===========================================================================
 *  Entry point
 *
 *  crt0 (userspace/crt0.asm) provides _start and calls
 *  `int main(int argc, char** argv)`.  With no arguments (argc <= 1) we run the
 *  parser self-test, exactly as before (a spawned `make` still self-tests since
 *  SYS_SPAWN delivers no argv).  With arguments, argv[1..] are targets to build
 *  out of the real Makefile.  The exit code is main's return value.
 * ========================================================================= */

/* Load /Makefile (then ./Makefile) into g_ctx and parse it.
 * Returns 0 on success; nonzero (and prints the reason) on error. */
static int load_and_parse_makefile(void) {
    long len = load_file("/Makefile", g_ctx.src, MK_BUF_MAX);
    if (len < 0) len = load_file("Makefile", g_ctx.src, MK_BUF_MAX);
    if (len < 0) {
        out("make: *** No Makefile found (tried /Makefile and Makefile).  Stop.\n");
        return 2;
    }
    g_ctx.srclen = len;
    parse(&g_ctx);
    if (g_ctx.ntgt == 0) {
        out("make: *** No targets.  Stop.\n");
        return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    out("make: minimal make for this OS\n");

    /* No target arguments: run the parser self-test (smoke-gated) and exit. */
    if (argc <= 1) {
        return selftest();
    }

    /* Targets given on the command line: load + parse the real Makefile, then
     * build each requested target via the existing dep-resolution path. */
    int rc = load_and_parse_makefile();
    if (rc != 0) return rc;

    out("make: note -- no mtime check (SYS_STAT lacks mtime); "
        "recipes always run in dependency order.\n");

    for (int a = 1; a < argc; a++) {
        const char *name = argv[a];
        int ti = tgt_find(&g_ctx, name);
        if (ti < 0) {
            out("make: *** No rule to make target '");
            out(name);
            out("'.  Stop.\n");
            return 2;
        }
        out("make: building target '");
        out(g_ctx.tgt[ti].name);
        out("'\n");
        if (build(&g_ctx, ti) != 0) return 2;
        out("make: '");
        out(g_ctx.tgt[ti].name);
        out("' is up to date.\n");
    }
    return 0;
}

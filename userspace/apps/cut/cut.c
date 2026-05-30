/*
 * cut.c -- minimal freestanding `cut` for the from-scratch x86_64 OS.
 * ===================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard headers.
 * Pure inline syscalls + fixed static buffers + hand-rolled helpers. All
 * output goes to fd 1 (SYS_WRITE).
 *
 * Selects fields or characters from each line of a file.
 *
 * Usage:
 *   cut -f LIST [-d DELIM] FILE   field mode. LIST selects 1-based fields
 *                                 split on DELIM (default TAB). Selected
 *                                 fields are re-joined with DELIM, in input
 *                                 order. A line with no DELIM is passed
 *                                 through whole (GNU default).
 *   cut -c LIST FILE              character mode. LIST selects 1-based byte
 *                                 columns of each line.
 *   cut                          (argc<=1) run the built-in self-test,
 *                                 printing "CUT SELFTEST: PASS"/"FAIL".
 *
 *   LIST grammar: comma-separated items, each "N" or "M-N" (1-based,
 *   inclusive). "M-" means M to end of line. Example: "1,3" or "2-4".
 *
 * Build (flags DIRECTLY on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/cut/cut.c -o /tmp/cut.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o /tmp/cut.o -o /tmp/cut.elf
 *   objdump -d /tmp/cut.elf | grep 'fs:0x28'   # MUST produce no output
 */

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

#define O_RDONLY  0x0000
#define KPATH_MAX 4096

typedef unsigned long size_t;

/* 6-arg inline syscall wrapper (rdi, rsi, rdx, r10, r8). */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* =======================================================================
 *  Freestanding helpers.
 * ======================================================================= */
static size_t c_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void c_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int c_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n, 0, 0); }
static void out(const char *s)           { out_n(s, (long)c_strlen(s)); }

/* =======================================================================
 *  Buffers.
 * ======================================================================= */
#define INBUF_MAX  (64 * 1024)
#define OUTBUF_MAX (96 * 1024)

static char g_in[INBUF_MAX]   __attribute__((aligned(16)));
static char g_out[OUTBUF_MAX] __attribute__((aligned(16)));
static long g_out_len;
static char g_path[KPATH_MAX] __attribute__((aligned(16)));

static void ob_putc(char c) { if (g_out_len < OUTBUF_MAX) g_out[g_out_len++] = c; }
static void ob_put(const char *s, long n) { for (long i = 0; i < n; i++) ob_putc(s[i]); }

/* =======================================================================
 *  Selection ranges. A compiled LIST is up to RANGE_MAX [lo,hi] pairs
 *  (1-based, inclusive). hi == 0 means "open-ended to end of line".
 * ======================================================================= */
#define RANGE_MAX 64
typedef struct { long lo; long hi; } range_t;
static range_t g_ranges[RANGE_MAX];
static int     g_nranges;

/* Parse LIST like "1,3" / "2-4" / "5-". Returns 0 on success, -1 on error. */
static int parse_list(const char *s) {
    g_nranges = 0;
    const char *p = s;
    long guard = 0;
    while (*p && guard < 100000) {
        guard++;
        long lo = 0, hi = 0;
        int sawlo = 0;
        while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p - '0'); p++; sawlo = 1; }
        if (*p == '-') {
            p++;
            int sawhi = 0;
            while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p - '0'); p++; sawhi = 1; }
            if (!sawhi) hi = 0;            /* "M-" open-ended */
            if (!sawlo) lo = 1;            /* "-N" from start */
        } else {
            hi = lo;                       /* single number */
        }
        if (!sawlo && hi == 0) return -1;  /* empty item */
        if (lo < 1) lo = 1;
        if (g_nranges >= RANGE_MAX) return -1;
        g_ranges[g_nranges].lo = lo;
        g_ranges[g_nranges].hi = hi;
        g_nranges++;
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;                          /* unexpected char */
    }
    return (g_nranges > 0) ? 0 : -1;
}

/* Is 1-based index n selected by any range? */
static int selected(long n) {
    for (int i = 0; i < g_nranges; i++) {
        long lo = g_ranges[i].lo, hi = g_ranges[i].hi;
        if (n >= lo && (hi == 0 || n <= hi)) return 1;
    }
    return 0;
}

/* =======================================================================
 *  Character-mode line processing: emit selected 1-based columns of
 *  line[0..llen) into g_out.
 * ======================================================================= */
static void cut_chars_line(const char *line, long llen) {
    for (long col = 0; col < llen; col++)
        if (selected(col + 1)) ob_putc(line[col]);
}

/* =======================================================================
 *  Field-mode line processing: split line on `delim`, emit selected fields
 *  re-joined with `delim`. If the line contains NO delimiter, emit it whole
 *  (matching GNU `cut` default behaviour).
 * ======================================================================= */
static void cut_fields_line(const char *line, long llen, char delim) {
    /* check for any delimiter */
    int has_delim = 0;
    for (long i = 0; i < llen; i++) if (line[i] == delim) { has_delim = 1; break; }
    if (!has_delim) { ob_put(line, llen); return; }

    long fstart = 0;
    long field = 1;
    int emitted = 0;
    long guard = 0;
    for (long i = 0; i <= llen && guard <= llen + 1; i++) {
        guard++;
        if (i == llen || line[i] == delim) {
            if (selected(field)) {
                if (emitted) ob_putc(delim);
                ob_put(line + fstart, i - fstart);
                emitted = 1;
            }
            field++;
            fstart = i + 1;
            if (i == llen) break;
        }
    }
}

/* =======================================================================
 *  Core: iterate lines of buf[0..len), apply selection, append to g_out.
 *  `mode_chars` selects char vs field mode. Each output line gets a '\n'
 *  if the source line had one.
 * ======================================================================= */
static void cut_core(const char *buf, long len, int mode_chars, char delim) {
    g_out_len = 0;
    long start = 0;
    long guard = 0;
    while (start < len && guard <= len) {
        guard++;
        long e = start;
        while (e < len && buf[e] != '\n') e++;
        int had_nl = (e < len);
        long llen = e - start;
        const char *line = buf + start;

        if (mode_chars) cut_chars_line(line, llen);
        else            cut_fields_line(line, llen, delim);

        if (had_nl) ob_putc('\n');
        if (!had_nl) break;
        start = e + 1;
    }
}

/* =======================================================================
 *  cut_run -- argv-driven entry.
 *    cut -f LIST [-d DELIM] FILE
 *    cut -c LIST FILE
 * ======================================================================= */
static int cut_run(int argc, char **argv) {
    int mode_chars = -1;     /* -1 unset, 0 fields, 1 chars */
    const char *list = 0;
    char delim = '\t';
    const char *file = 0;

    int ai = 1;
    while (ai < argc && argv[ai]) {
        const char *a = argv[ai];
        if (c_streq(a, "-f")) {
            mode_chars = 0;
            if (++ai >= argc || !argv[ai]) { out("cut: -f needs LIST\n"); return 1; }
            list = argv[ai];
        } else if (c_streq(a, "-c")) {
            mode_chars = 1;
            if (++ai >= argc || !argv[ai]) { out("cut: -c needs LIST\n"); return 1; }
            list = argv[ai];
        } else if (c_streq(a, "-d")) {
            if (++ai >= argc || !argv[ai]) { out("cut: -d needs DELIM\n"); return 1; }
            delim = argv[ai][0];
        } else if (a[0] == '-' && a[1]) {
            out("cut: unknown option: "); out(a); out("\n"); return 1;
        } else {
            file = a;            /* first non-flag = FILE */
        }
        ai++;
    }

    if (mode_chars < 0 || !list) { out("usage: cut -f LIST [-d DELIM] FILE | cut -c LIST FILE\n"); return 1; }
    if (!file) { out("cut: no input file\n"); return 1; }
    if (parse_list(list) != 0) { out("cut: invalid LIST: "); out(list); out("\n"); return 1; }

    c_strlcpy(g_path, file, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) { out("cut: cannot open '"); out(file); out("'\n"); return 1; }
    long total = 0;
    for (;;) {
        long room = INBUF_MAX - total;
        if (room <= 0) { out("cut: input too large (>64KB)\n"); sc(SYS_CLOSE, fd, 0, 0, 0, 0); return 1; }
        long n = sc(SYS_READ, fd, (long)(g_in + total), room, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);

    cut_core(g_in, total, mode_chars, delim);
    out_n(g_out, g_out_len);
    return 0;
}

/* =======================================================================
 *  SELF-TEST (in-memory).
 * ======================================================================= */
static int bytes_eq(const char *a, long alen, const char *b, long blen) {
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int t_case(const char *list, int mode_chars, char delim,
                  const char *in, const char *expect) {
    if (parse_list(list) != 0) { out("  cut case PARSE-FAIL\n"); return 0; }
    cut_core(in, (long)c_strlen(in), mode_chars, delim);
    long elen = (long)c_strlen(expect);
    if (bytes_eq(g_out, g_out_len, expect, elen)) return 1;
    out("  cut case FAIL\n    got:  "); out_n(g_out, g_out_len);
    out("\n    want: "); out(expect); out("\n");
    return 0;
}

static int selftest(void) {
    out("CUT: selftest begin\n");
    int ok = 1;
    /* field mode, delim ':' -- cut -f1 -d: of "x:y:z" -> "x" */
    ok &= t_case("1", 0, ':', "x:y:z", "x");
    /* fields 1,3 with ':' */
    ok &= t_case("1,3", 0, ':', "a:b:c:d\n", "a:c\n");
    /* range 2-4 with ':' */
    ok &= t_case("2-4", 0, ':', "a:b:c:d:e\n", "b:c:d\n");
    /* open-ended 2- */
    ok &= t_case("2-", 0, ':', "a:b:c\n", "b:c\n");
    /* char mode: chars 1,3 of "abcde" -> "ac" */
    ok &= t_case("1,3", 1, ':', "abcde\n", "ac\n");
    /* char range 2-4 of "abcde" -> "bcd" */
    ok &= t_case("2-4", 1, ':', "abcde", "bcd");
    /* line with no delimiter passes through whole in field mode */
    ok &= t_case("1", 0, ':', "nodelim\n", "nodelim\n");

    if (ok) { out("CUT SELFTEST: PASS\n"); return 0; }
    out("CUT SELFTEST: FAIL\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return cut_run(argc, argv);
    (void)selftest();
    return 0;
}

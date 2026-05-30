/*
 * less.c -- a pragmatic freestanding pager for the from-scratch x86_64 OS.
 * ========================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc / stdio / malloc / standard
 * headers. Pure inline syscalls + fixed static buffers + hand-rolled
 * helpers. All output goes to fd 1.
 *
 * `less FILE` shows a file one screenful (24 lines) at a time. Because
 * interactive stdin in the GUI terminal may not be wired up, this pager is
 * deliberately PRAGMATIC and never hangs:
 *
 *   - It splits the file into pages of LINES_PER_PAGE display lines (a
 *     display line ends at '\n' OR at COLS columns, whichever comes first,
 *     so very long lines wrap and still page correctly).
 *   - Between pages it probes stdin (fd 0) with a single 1-byte read:
 *        * if that read returns a byte: 'q' (or ESC) quits, '\n'/'\r'
 *          advances ONE line, anything else (e.g. space) advances a page.
 *        * if the read returns 0 or an error (stdin not available), it
 *          DEGRADES TO `cat`: it stops prompting and prints the rest of the
 *          file straight through. This guarantees the program terminates.
 *   - Everything is bounded: a fixed input cap and bounded loops.
 *
 * Usage:
 *   less FILE               page through FILE.
 *   less                    (argc <= 1) run the built-in self-test, printing
 *                           "LESS SELFTEST: PASS" or "LESS SELFTEST: FAIL".
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/less/less.c -o less.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o less.o -o build/less
 *   objdump -d build/less | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers -- verified against kernel/include/syscall.h.
 * --------------------------------------------------------------------- */
#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_YIELD  15

/* open() flags. */
#define O_RDONLY  0x0000

#define KPATH_MAX 4096

typedef unsigned long size_t;

/* Pager geometry. */
#define LINES_PER_PAGE 24
#define COLS           80

/* -----------------------------------------------------------------------
 * Inline 6-arg syscall wrapper (rdi, rsi, rdx, r10, r8).
 * --------------------------------------------------------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
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
static size_t l_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static size_t l_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}
static void l_memcpy(char *dst, const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

static void out_n(const char *s, size_t n) { sc(SYS_WRITE, 1, (long)s, (long)n, 0, 0); }
static void out(const char *s)             { out_n(s, l_strlen(s)); }

/* =======================================================================
 *  Buffers (static -- off the tiny user stack).
 * ======================================================================= */
#define FILEBUF_MAX (256 * 1024)               /* 256 KB input cap         */
static char g_buf[FILEBUF_MAX] __attribute__((aligned(16)));
static char g_path[KPATH_MAX]  __attribute__((aligned(16)));

/* =======================================================================
 *  Page-boundary computation -- the helper the self-test verifies.
 *
 *  count_display_lines(): count how many DISPLAY lines text[0..len) occupies
 *  when each '\n' ends a line and any line longer than `cols` wraps. An empty
 *  buffer counts as 0 lines; text that does not end in '\n' still counts its
 *  final partial line.
 *
 *  count_pages(): how many screenfuls of `lines_per_page` display lines that
 *  is (ceil division; at least 1 page if there is any content, 0 if empty).
 * ======================================================================= */
static long count_display_lines(const char *text, long len, int cols)
{
    if (len <= 0) return 0;
    long lines = 0;
    long col = 0;
    int saw_any = 0;
    for (long i = 0; i < len; i++) {
        saw_any = 1;
        if (text[i] == '\n') {
            lines++;
            col = 0;
        } else {
            col++;
            if (col >= cols) {        /* hard wrap at column boundary */
                lines++;
                col = 0;
            }
        }
    }
    /* a trailing partial line (no terminating '\n', col > 0) counts too */
    if (col > 0) lines++;
    if (!saw_any) return 0;
    return lines;
}

static long count_pages(const char *text, long len, int cols, int lines_per_page)
{
    if (lines_per_page <= 0) lines_per_page = 1;
    long dl = count_display_lines(text, len, cols);
    if (dl <= 0) return 0;
    return (dl + lines_per_page - 1) / lines_per_page;   /* ceil */
}

/* =======================================================================
 *  File read.
 * ======================================================================= */
static long slurp(const char *path, unsigned long cap)
{
    l_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;

    unsigned long total = 0;
    int guard = 0;
    while (total < cap) {
        long room = (long)(cap - total);
        long r = sc(SYS_READ, fd, (long)(g_buf + total), room, 0, 0);
        if (r < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0); return -1; }
        if (r == 0) break;                       /* EOF / short read => done */
        total += (unsigned long)r;
        if (++guard > 1000000) break;            /* runaway guard (bounded)  */
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0);
    return (long)total;
}

/* =======================================================================
 *  Paging.
 *
 *  We emit the buffer one display line at a time. After every
 *  LINES_PER_PAGE display lines we pause and probe stdin. If stdin is
 *  unavailable we set `interactive = 0` and never prompt again -- the rest
 *  prints straight through (cat fallback), so the pager NEVER hangs.
 *
 *  Returns the process exit code (0).
 * ======================================================================= */

/* Probe a single keypress on fd 0. Returns:
 *   >0  : the key byte (0..255)
 *    0  : stdin returned EOF (no data; treat as "not interactive")
 *   -1  : stdin read error / unavailable
 */
static int read_key(void)
{
    char c;
    long r = sc(SYS_READ, 0, (long)&c, 1, 0, 0);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return (unsigned char)c;
}

static int pager(long len)
{
    if (len <= 0) return 0;

    int interactive = 1;       /* until stdin proves otherwise            */
    long i = 0;                /* current byte offset                     */
    long lines_on_page = 0;    /* display lines emitted since last prompt  */
    long col = 0;              /* current column for wrap accounting       */

    while (i < len) {
        /* Emit exactly one display line: up to a '\n' or COLS columns. */
        long start = i;
        while (i < len) {
            char ch = g_buf[i];
            i++;
            if (ch == '\n') { col = 0; break; }
            col++;
            if (col >= COLS) { col = 0; break; }   /* wrap */
        }
        out_n(g_buf + start, (size_t)(i - start));
        /* If the chunk didn't end in '\n' (a wrap), add a newline so the GUI
         * terminal advances to the next row visually. */
        if (i > start && g_buf[i - 1] != '\n') out_n("\n", 1);
        lines_on_page++;

        if (i >= len) break;   /* whole file shown */

        if (interactive && lines_on_page >= LINES_PER_PAGE) {
            out_n("--More-- (space=page, enter=line, q=quit)", 41);
            int k = read_key();
            /* clear the prompt line best-effort with a CR + spaces + CR */
            out_n("\r                                         \r", 43);

            if (k <= 0) {
                /* stdin unavailable -> degrade to cat for the remainder */
                interactive = 0;
                lines_on_page = 0;
            } else if (k == 'q' || k == 'Q' || k == 27 /*ESC*/) {
                return 0;      /* quit */
            } else if (k == '\n' || k == '\r') {
                lines_on_page = LINES_PER_PAGE - 1;  /* advance ONE line */
            } else {
                lines_on_page = 0;                   /* advance a full page */
            }
        } else if (!interactive) {
            /* cat mode: yield occasionally so we play nice with the
             * cooperative scheduler, but otherwise stream straight through. */
            if ((lines_on_page++ & 0x3ff) == 0) sc(SYS_YIELD, 0, 0, 0, 0, 0);
        }
    }
    return 0;
}

/* =======================================================================
 *  less_run -- argv-driven entry.
 *
 *    less FILE
 * ======================================================================= */
static int less_run(int argc, char **argv)
{
    if (argc < 2 || !argv[1]) { out("usage: less FILE\n"); return 1; }
    const char *file = argv[1];

    long n = slurp(file, FILEBUF_MAX);
    if (n < 0) { out("less: cannot open '"); out(file); out("'\n"); return 1; }

    return pager(n);
}

/* =======================================================================
 *  SELF-TEST
 *
 *  Verifies the line-splitting / paging math on a known in-memory sample,
 *  with NO input required (so it never hangs). Prints
 *  "LESS SELFTEST: PASS" / "LESS SELFTEST: FAIL".
 * ======================================================================= */
static void out_num(unsigned long v)
{
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) { char c = b[--i]; sc(SYS_WRITE, 1, (long)&c, 1, 0, 0); }
}

static int selftest(void)
{
    out("LESS: selftest begin\n");
    int ok = 1;

    /* Case 1: 50 newline-terminated lines, no wrapping. With 24 lines/page
     * that is ceil(50/24) = 3 pages. */
    {
        /* build "L\n" x 50 in g_buf (100 bytes) */
        long len = 0;
        for (int n = 0; n < 50; n++) { g_buf[len++] = 'L'; g_buf[len++] = '\n'; }
        long dl    = count_display_lines(g_buf, len, COLS);
        long pages = count_pages(g_buf, len, COLS, LINES_PER_PAGE);
        out("LESS: 50 lines -> display="); out_num((unsigned long)dl);
        out(" pages="); out_num((unsigned long)pages); out("\n");
        if (dl != 50 || pages != 3) { out("LESS: case1 mismatch\n"); ok = 0; }
    }

    /* Case 2: empty buffer -> 0 display lines, 0 pages. */
    {
        long dl    = count_display_lines(g_buf, 0, COLS);
        long pages = count_pages(g_buf, 0, COLS, LINES_PER_PAGE);
        if (dl != 0 || pages != 0) { out("LESS: case2 mismatch\n"); ok = 0; }
    }

    /* Case 3: a single line of 200 chars wraps at 80 cols -> 3 display lines
     * (80 + 80 + 40), which is 1 page. No trailing newline. */
    {
        long len = 0;
        for (int n = 0; n < 200; n++) g_buf[len++] = 'x';
        long dl    = count_display_lines(g_buf, len, COLS);
        long pages = count_pages(g_buf, len, COLS, LINES_PER_PAGE);
        out("LESS: 200-char line -> display="); out_num((unsigned long)dl);
        out(" pages="); out_num((unsigned long)pages); out("\n");
        if (dl != 3 || pages != 1) { out("LESS: case3 mismatch\n"); ok = 0; }
    }

    /* Case 4: final partial line (no terminating '\n') still counts. Two
     * full lines + one partial = 3 display lines. */
    {
        const char *s = "aa\nbb\ncc";   /* 8 bytes, 3 display lines */
        long len = (long)l_strlen(s);
        l_memcpy(g_buf, s, (size_t)len);
        long dl = count_display_lines(g_buf, len, COLS);
        if (dl != 3) { out("LESS: case4 mismatch\n"); ok = 0; }
    }

    if (ok) { out("LESS SELFTEST: PASS\n"); return 0; }
    out("LESS SELFTEST: FAIL\n");
    return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 (userspace/crt0.asm) reads argc/argv off the kernel-prepared stack
 *  and calls main(argc, argv), turning the return value into SYS_EXIT. With
 *  a FILE argument we page it; with none we run the self-test (which needs no
 *  input and never hangs), and main returns 0 on PASS.
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) return less_run(argc, argv);
    (void)selftest();
    return 0;
}

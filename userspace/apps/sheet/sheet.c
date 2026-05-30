/*
 * sheet.c -- A tiny spreadsheet application (freestanding, ring 3).
 * ================================================================
 *
 * Opens a ~720x460 window titled "Sheet".  The window contains:
 *
 *   +--------------------------------------------------------------+
 *   |  A1: =SUM(A1:A5)                          (formula bar)      |
 *   +----+----------+----------+----------+ ... +-----------------+
 *   |    |    A     |    B     |    C     |     |       H         |  (col header)
 *   +----+----------+----------+----------+ ... +-----------------+
 *   |  1 |  123     |          |  =A1+B1  |     |                 |
 *   |  2 |          |          |          |     |                 |
 *   | .. |          |          |          |     |                 |
 *   | 15 |          |          |          |     |                 |
 *   +----+----------+----------+----------+ ... +-----------------+
 *
 * Grid: 8 columns (A..H) x 15 rows (1..15), with a header row and a
 * header column.  One cell is "selected" (highlighted).  Move the
 * selection with the arrow keys or by clicking a cell.
 *
 * Editing: typing characters appends to an edit buffer for the selected
 * cell (US-layout keycode -> ASCII map; lowercase letters, digits, and a
 * handful of operators '.', '-', '+', '*', '/', '=', ':', '(' ')' space).
 * Backspace deletes the last character.  Enter commits the edit buffer to
 * the cell and moves the selection down one row.  Clicking a different
 * cell also commits the current edit.
 *
 * Formulas: a cell whose text begins with '=' is a formula.  Supported:
 *   =A1+B2   =A1-B2   =A1*B2   =A1/B2     (single binary op on two refs/
 *                                          numbers; chained left-to-right)
 *   =SUM(A1:A5)                            (sum over a rectangular range)
 * Cell references resolve to the numeric value of the referenced cell
 * (recomputed every render).  Non-numeric cells evaluate to 0.  A depth
 * cap guards against cyclic / deeply nested references.
 *
 * Arithmetic is fixed-point with FP_SCALE = 1000 (3 fractional digits),
 * so e.g. =10/3 displays as 3.333.
 *
 * No libc: pure inline syscalls + tiny freestanding helpers + a small
 * recursive-descent-ish evaluator.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable,
 * or -fno-stack-protector gets dropped and CR2 faults at 0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/sheet/sheet.c -o /tmp/sheet.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/sheet.o /tmp/wlc.o /tmp/bf.o -o /tmp/sheet.elf
 *   objdump -d /tmp/sheet.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SHEET] starting
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

static inline long sc6(long n, long a1, long a2, long a3,
                       long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Minimal freestanding helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int u32;
typedef int          i32;

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc6(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Keycodes (mirrors kernel/include/input.h; copied to stay self-contained
 * within this single TU -- we are not allowed to add include paths).
 * --------------------------------------------------------------------- */
#define KC_BACKSPACE 14
#define KC_ENTER     28
#define KC_LEFT      105
#define KC_RIGHT     106
#define KC_UP        103
#define KC_DOWN      108
#define KC_SPACE     57

/* -----------------------------------------------------------------------
 * US-layout keycode -> ASCII (unshifted only).  Index by keycode.
 * Covers letters (lowercase), digits, and the operators we care about:
 *   '.' '-' '+' '*' '/' '=' plus ':' '(' ')' and space.
 * Unmapped keys yield 0.
 * --------------------------------------------------------------------- */
static char keymap_ascii(int kc)
{
    switch (kc) {
    /* digit row */
    case 2:  return '1';
    case 3:  return '2';
    case 4:  return '3';
    case 5:  return '4';
    case 6:  return '5';
    case 7:  return '6';
    case 8:  return '7';
    case 9:  return '8';
    case 10: return '9';
    case 11: return '0';
    case 12: return '-';  /* KEY_MINUS  */
    case 13: return '=';  /* KEY_EQUAL  */
    /* top letter row */
    case 16: return 'q';
    case 17: return 'w';
    case 18: return 'e';
    case 19: return 'r';
    case 20: return 't';
    case 21: return 'y';
    case 22: return 'u';
    case 23: return 'i';
    case 24: return 'o';
    case 25: return 'p';
    /* home letter row */
    case 30: return 'a';
    case 31: return 's';
    case 32: return 'd';
    case 33: return 'f';
    case 34: return 'g';
    case 35: return 'h';
    case 36: return 'j';
    case 37: return 'k';
    case 38: return 'l';
    case 39: return ';';  /* used as ':' surrogate not needed; keep ';' */
    /* bottom letter row */
    case 44: return 'z';
    case 45: return 'x';
    case 46: return 'c';
    case 47: return 'v';
    case 48: return 'b';
    case 49: return 'n';
    case 50: return 'm';
    case 51: return ',';
    case 52: return '.';  /* KEY_DOT   */
    case 53: return '/';  /* KEY_SLASH */
    /* keypad asterisk */
    case 55: return '*';  /* KEY_KPASTERISK */
    case 57: return ' ';  /* space */
    default: return 0;
    }
}

/*
 * Because a US keyboard without a shift map cannot directly produce
 * '+', ':', '(' and ')', and a spreadsheet's SUM range syntax needs
 * ':' '(' ')' while '+' is the addition operator, we provide convenient
 * unshifted aliases that ARE producible from this map:
 *
 *   '+'  ->  also accept via a dedicated handling: KEY_KPPLUS isn't in the
 *            kernel header subset, so we let users type '+' by reusing the
 *            ';' key? No -- instead we map a couple of seldom-needed letter
 *            keys to the SUM-syntax punctuation so formulas are typable:
 *
 *   We DO want '+', so map it to a key. The cleanest US-typable approach
 *   here is to treat these specially in the key handler (see on_key):
 *     - '=' from KEY_EQUAL (works)
 *     - '-' from KEY_MINUS (works)
 *     - '*' from KEY_KPASTERISK (works)
 *     - '/' from KEY_SLASH (works)
 *     - '+' : we additionally accept KEY_KPASTERISK? no. Instead we accept
 *            the SEMICOLON key (kc 39) as '+', and provide ':' '(' ')'
 *            through a tiny "syntax" key set below.
 *
 * To keep things simple and robust we override a few of the above in the
 * key handler: see remap_for_sheet().
 */
static char remap_for_sheet(int kc)
{
    /*
     * Provide spreadsheet punctuation that a plain unshifted US map cannot:
     *   ';'  (kc 39) -> '+'   (so addition is typable)
     *   '['  (kc 26) -> '('
     *   ']'  (kc 27) -> ')'
     *   '\'' (kc 40) -> ':'
     * Everything else falls through to keymap_ascii().
     */
    switch (kc) {
    case 39: return '+';   /* KEY_SEMICOLON   -> '+' */
    case 26: return '(';   /* KEY_LEFTBRACE   -> '(' */
    case 27: return ')';   /* KEY_RIGHTBRACE  -> ')' */
    case 40: return ':';   /* KEY_APOSTROPHE  -> ':' */
    default: return keymap_ascii(kc);
    }
}

/* -----------------------------------------------------------------------
 * Fixed-point arithmetic.
 *   value = real * FP_SCALE.  3 fractional digits.
 * --------------------------------------------------------------------- */
typedef long fp_t;
#define FP_SCALE 1000L

/* -----------------------------------------------------------------------
 * Grid geometry & storage.
 * --------------------------------------------------------------------- */
#define WIN_W       720
#define WIN_H       460

#define NCOLS       8    /* A..H */
#define NROWS       15   /* 1..15 */

#define CELL_MAX    16   /* max chars stored per cell (incl NUL)        */

/* Layout in pixels. */
#define FBAR_H      22   /* formula bar height                          */
#define HDR_H       18   /* column-header row height                    */
#define ROWHDR_W    36   /* row-header (1..15) column width             */
#define COL_W       84   /* data column width                           */
#define ROW_H       26   /* data row height                             */
#define GRID_X0     0
#define GRID_Y0     (FBAR_H)            /* top of column-header row      */
#define DATA_Y0     (GRID_Y0 + HDR_H)   /* top of first data row         */
#define DATA_X0     (GRID_X0 + ROWHDR_W)/* left of first data column     */

/* Colors (ARGB). */
#define COL_BG       0xFFF2F2F2u  /* window background      */
#define COL_FBAR     0xFFFFFFFFu  /* formula bar bg         */
#define COL_HDR      0xFFD8D8DCu  /* header cell bg         */
#define COL_HDR_SEL  0xFFB0C4DEu  /* header of selected col/row */
#define COL_CELL     0xFFFFFFFFu  /* cell bg                */
#define COL_SEL      0xFFCFE3FFu  /* selected cell bg       */
#define COL_GRID     0xFFB0B0B0u  /* grid lines             */
#define COL_TEXT     0xFF101010u  /* text                   */
#define COL_HDRTEXT  0xFF303030u  /* header text            */
#define COL_FORMULA  0xFF005000u  /* computed formula value */

/* Cell text store: row-major [row][col]. */
static char  g_cells[NROWS][NCOLS][CELL_MAX];

/* Selected cell. */
static int g_sel_col = 0;
static int g_sel_row = 0;

/* Edit buffer for the selected cell (live, uncommitted text). */
static char g_edit[CELL_MAX];
static int  g_editing = 0;  /* 1 once the user has started typing */

/* -----------------------------------------------------------------------
 * Tiny string helpers.
 * --------------------------------------------------------------------- */
static void k_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Reverse a buffer in place [0..n). */
static void rev(char *b, int n)
{
    int i = 0, j = n - 1;
    while (i < j) { char t = b[i]; b[i] = b[j]; b[j] = t; i++; j--; }
}

/*
 * Format a fixed-point value into out (cap bytes).  Shows up to 3 frac
 * digits, trimming trailing zeros (but always at least the integer part).
 * Examples: 3000 -> "3", 3333 -> "3.333", -1500 -> "-1.5".
 */
static void fp_to_str(fp_t v, char *out, int cap)
{
    if (cap <= 0) return;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }

    long ip = v / FP_SCALE;
    long fp = v % FP_SCALE;          /* 0..999 */

    char tmp[24];
    int n = 0;

    /* integer part (reversed) */
    if (ip == 0) {
        tmp[n++] = '0';
    } else {
        while (ip > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(ip % 10));
            ip /= 10;
        }
    }
    if (neg && n < (int)sizeof(tmp)) tmp[n++] = '-';
    rev(tmp, n);

    /* fractional part: 3 digits then trim trailing zeros */
    int fn = 0;
    char frac[4];
    if (fp > 0) {
        frac[0] = (char)('0' + (int)((fp / 100) % 10));
        frac[1] = (char)('0' + (int)((fp / 10) % 10));
        frac[2] = (char)('0' + (int)(fp % 10));
        fn = 3;
        while (fn > 0 && frac[fn - 1] == '0') fn--;
    }

    /* assemble into out */
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) out[o++] = tmp[i];
    if (fn > 0 && o < cap - 1) {
        out[o++] = '.';
        for (int i = 0; i < fn && o < cap - 1; i++) out[o++] = frac[i];
    }
    out[o] = '\0';
}

/*
 * Parse a numeric string into a fixed-point value.  Accepts an optional
 * leading sign, integer digits, an optional '.' and up to 3 fractional
 * digits.  *ok set to 1 if the ENTIRE (NUL-terminated) string parsed as a
 * number, 0 otherwise.  Leading/trailing spaces are tolerated.
 */
static fp_t str_to_fp(const char *s, int *ok)
{
    int i = 0;
    int any = 0;
    int neg = 0;
    *ok = 0;

    while (s[i] == ' ') i++;
    if (s[i] == '+') { i++; }
    else if (s[i] == '-') { neg = 1; i++; }

    long ip = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        ip = ip * 10 + (s[i] - '0');
        i++;
        any = 1;
    }

    long fp = 0;
    if (s[i] == '.') {
        i++;
        long mul = 100;
        while (s[i] >= '0' && s[i] <= '9') {
            if (mul >= 1) {
                fp += (long)(s[i] - '0') * mul;
                mul /= 10;
            }
            i++;
            any = 1;
        }
    }

    while (s[i] == ' ') i++;

    if (!any || s[i] != '\0') {
        *ok = 0;
        return 0;
    }

    fp_t val = ip * FP_SCALE + fp;
    if (neg) val = -val;
    *ok = 1;
    return val;
}

/* -----------------------------------------------------------------------
 * Formula evaluation.
 *
 * eval_cell(col,row,depth) returns the numeric (fixed-point) value of the
 * cell, recursively evaluating any references inside formulas.  A depth
 * cap (MAX_DEPTH) breaks cycles and runaway recursion -> returns 0.
 * --------------------------------------------------------------------- */
#define MAX_DEPTH 16

static fp_t eval_cell(int col, int row, int depth);

/* Parse a cell reference like "A1" / "h15" at s[*pi]; on success advance
 * *pi past it and set *pc/*pr (0-based).  Returns 1 on success. */
static int parse_ref(const char *s, int *pi, int *pc, int *pr)
{
    int i = *pi;
    char c = s[i];
    int col;

    if (c >= 'A' && c <= 'Z')      col = c - 'A';
    else if (c >= 'a' && c <= 'z') col = c - 'a';
    else return 0;

    i++;
    if (s[i] < '0' || s[i] > '9') return 0;

    int row = 0;
    int any = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        row = row * 10 + (s[i] - '0');
        i++;
        any = 1;
    }
    if (!any) return 0;

    row -= 1;  /* 1-based -> 0-based */
    if (col < 0 || col >= NCOLS || row < 0 || row >= NROWS) return 0;

    *pc = col;
    *pr = row;
    *pi = i;
    return 1;
}

/*
 * Parse one operand at s[*pi]: either a cell reference or a numeric
 * literal.  Returns its fixed-point value; sets *ok.  Advances *pi.
 */
static fp_t parse_operand(const char *s, int *pi, int depth, int *ok)
{
    int i = *pi;
    while (s[i] == ' ') i++;

    /* cell reference? */
    int c, r;
    int save = i;
    if (parse_ref(s, &i, &c, &r)) {
        *pi = i;
        *ok = 1;
        return eval_cell(c, r, depth + 1);
    }
    i = save;

    /* numeric literal: copy the token up to an operator / end, parse it. */
    char num[24];
    int n = 0;
    if (s[i] == '+' || s[i] == '-') {
        if (n < (int)sizeof(num) - 1) num[n++] = s[i];
        i++;
    }
    while ((s[i] >= '0' && s[i] <= '9') || s[i] == '.') {
        if (n < (int)sizeof(num) - 1) num[n++] = s[i];
        i++;
    }
    num[n] = '\0';

    if (n == 0) { *ok = 0; return 0; }

    int parsed;
    fp_t v = str_to_fp(num, &parsed);
    if (!parsed) { *ok = 0; return 0; }
    *pi = i;
    *ok = 1;
    return v;
}

/*
 * Evaluate a SUM(range) call.  s[*pi] points just past "SUM".  Expects
 * "(A1:A5)".  Sums eval_cell over the inclusive rectangular block spanned
 * by the two corner refs.  Returns sum; sets *ok.
 */
static fp_t eval_sum(const char *s, int *pi, int depth, int *ok)
{
    int i = *pi;
    *ok = 0;
    while (s[i] == ' ') i++;
    if (s[i] != '(') return 0;
    i++;

    int c1, r1, c2, r2;
    if (!parse_ref(s, &i, &c1, &r1)) return 0;
    while (s[i] == ' ') i++;
    if (s[i] != ':') return 0;
    i++;
    while (s[i] == ' ') i++;
    if (!parse_ref(s, &i, &c2, &r2)) return 0;
    while (s[i] == ' ') i++;
    if (s[i] != ')') return 0;
    i++;

    /* normalise corners */
    int cl = c1 < c2 ? c1 : c2;
    int ch = c1 > c2 ? c1 : c2;
    int rl = r1 < r2 ? r1 : r2;
    int rh = r1 > r2 ? r1 : r2;

    fp_t sum = 0;
    for (int rr = rl; rr <= rh; rr++)
        for (int cc = cl; cc <= ch; cc++)
            sum += eval_cell(cc, rr, depth + 1);

    *pi = i;
    *ok = 1;
    return sum;
}

/* Case-insensitive check that s[i..] starts with "SUM" (3 letters). */
static int starts_with_sum(const char *s, int i)
{
    char a = s[i], b = s[i + 1], c = s[i + 2];
    int A = (a == 'S' || a == 's');
    int B = (b == 'U' || b == 'u');
    int C = (c == 'M' || c == 'm');
    return A && B && C;
}

/*
 * Evaluate a formula string (WITHOUT the leading '=') into a fixed-point
 * value.  Supports:
 *   - SUM(range)
 *   - a chain of operands separated by + - * / evaluated left to right
 *     (no precedence; matches the task's simple binary-op requirement and
 *     extends gracefully to chains).
 * Sets *ok on success.
 */
static fp_t eval_expr(const char *s, int depth, int *ok)
{
    int i = 0;
    *ok = 0;

    while (s[i] == ' ') i++;

    /* SUM(...) special form. */
    if (starts_with_sum(s, i)) {
        i += 3;
        int sok;
        fp_t v = eval_sum(s, &i, depth, &sok);
        if (!sok) return 0;
        while (s[i] == ' ') i++;
        /* allow nothing else after SUM(...) for simplicity */
        if (s[i] != '\0') { *ok = 0; return 0; }
        *ok = 1;
        return v;
    }

    /* General: operand (op operand)* left-to-right. */
    int aok;
    fp_t acc = parse_operand(s, &i, depth, &aok);
    if (!aok) return 0;

    for (;;) {
        while (s[i] == ' ') i++;
        char op = s[i];
        if (op == '\0') break;
        if (op != '+' && op != '-' && op != '*' && op != '/') {
            /* unexpected token */
            *ok = 0;
            return 0;
        }
        i++;

        int bok;
        fp_t rhs = parse_operand(s, &i, depth, &bok);
        if (!bok) { *ok = 0; return 0; }

        switch (op) {
        case '+': acc = acc + rhs; break;
        case '-': acc = acc - rhs; break;
        case '*': acc = (acc * rhs) / FP_SCALE; break;     /* fixed-point */
        case '/':
            if (rhs == 0) { *ok = 0; return 0; }           /* div by zero */
            acc = (acc * FP_SCALE) / rhs;                  /* fixed-point */
            break;
        }
    }

    *ok = 1;
    return acc;
}

/*
 * Numeric value of a cell.  Formula cells (text[0]=='=') are evaluated;
 * plain numeric cells parse their text; everything else is 0.
 */
static fp_t eval_cell(int col, int row, int depth)
{
    if (depth > MAX_DEPTH) return 0;
    if (col < 0 || col >= NCOLS || row < 0 || row >= NROWS) return 0;

    const char *t = g_cells[row][col];
    if (t[0] == '\0') return 0;

    if (t[0] == '=') {
        int ok;
        fp_t v = eval_expr(t + 1, depth, &ok);
        return ok ? v : 0;
    }

    int ok;
    fp_t v = str_to_fp(t, &ok);
    return ok ? v : 0;
}

/*
 * Produce the DISPLAY text for a cell into out (cap bytes):
 *   - formula:  the computed value (or "#ERR" if it failed to parse)
 *   - other:    the raw text
 */
static void cell_display(int col, int row, char *out, int cap)
{
    const char *t = g_cells[row][col];
    if (t[0] == '=') {
        int ok;
        fp_t v = eval_expr(t + 1, 0, &ok);
        if (ok) fp_to_str(v, out, cap);
        else    k_strcpy(out, "#ERR", cap);
    } else {
        k_strcpy(out, t, cap);
    }
}

/* -----------------------------------------------------------------------
 * Rendering primitives.
 * --------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      int x, int y, int w, int h, u32 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)bw) w = (int)bw - x;
    if (y + h > (int)bh) h = (int)bh - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = 0; yy < h; yy++) {
        u32 *row = buf + (unsigned)(y + yy) * spx + x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

/* Horizontal / vertical 1px lines. */
static void hline(u32 *buf, u32 bw, u32 bh, u32 spx, int x, int y, int w, u32 c)
{
    fill_rect(buf, bw, bh, spx, x, y, w, 1, c);
}
static void vline(u32 *buf, u32 bw, u32 bh, u32 spx, int x, int y, int h, u32 c)
{
    fill_rect(buf, bw, bh, spx, x, y, 1, h, c);
}

/*
 * Draw a string clipped to a cell rectangle [x..x+w).  We render glyph by
 * glyph and stop once the next glyph would exceed the right edge.
 */
static void draw_text_clip(u32 *buf, u32 spx, u32 bw, u32 bh,
                           int x, int y, int maxw, const char *s, u32 color)
{
    int cx = x;
    int limit = x + maxw;
    for (int i = 0; s[i]; i++) {
        if (cx + FONT_W > limit) break;
        font_draw_char(buf, (int)spx, (int)bw, (int)bh, cx, y, s[i], color);
        cx += FONT_W;
    }
}

/* -----------------------------------------------------------------------
 * Hit testing: pixel -> cell.  Returns 1 and sets *col/*row if (mx,my)
 * lands inside the data grid; 0 otherwise (header / formula bar / outside).
 * --------------------------------------------------------------------- */
static int hit_cell(int mx, int my, int *col, int *row)
{
    if (mx < DATA_X0 || my < DATA_Y0) return 0;
    int c = (mx - DATA_X0) / COL_W;
    int r = (my - DATA_Y0) / ROW_H;
    if (c < 0 || c >= NCOLS || r < 0 || r >= NROWS) return 0;
    *col = c;
    *row = r;
    return 1;
}

/* -----------------------------------------------------------------------
 * Edit-buffer management.
 * --------------------------------------------------------------------- */

/* Begin editing the selected cell: seed edit buffer from its stored text. */
static void edit_begin(void)
{
    k_strcpy(g_edit, g_cells[g_sel_row][g_sel_col], CELL_MAX);
    g_editing = 1;
}

/* Commit the edit buffer into the selected cell. */
static void edit_commit(void)
{
    if (g_editing) {
        k_strcpy(g_cells[g_sel_row][g_sel_col], g_edit, CELL_MAX);
        g_editing = 0;
        g_edit[0] = '\0';
    }
}

/* Move the selection, committing any in-progress edit first. */
static void move_sel(int dc, int dr)
{
    edit_commit();
    int nc = g_sel_col + dc;
    int nr = g_sel_row + dr;
    if (nc < 0) nc = 0;
    if (nc >= NCOLS) nc = NCOLS - 1;
    if (nr < 0) nr = 0;
    if (nr >= NROWS) nr = NROWS - 1;
    g_sel_col = nc;
    g_sel_row = nr;
}

/* -----------------------------------------------------------------------
 * Frame render.
 * --------------------------------------------------------------------- */
static void render(wl_window *win)
{
    u32 *buf = win->pixels;
    u32  bw  = win->w;
    u32  bh  = win->h;
    u32  spx = win->stride / 4u;   /* font/draw use PIXEL stride */

    char tmp[CELL_MAX + 8];

    /* Background. */
    fill_rect(buf, bw, bh, spx, 0, 0, (int)bw, (int)bh, COL_BG);

    /* ---- Formula bar ---- */
    fill_rect(buf, bw, bh, spx, 0, 0, (int)bw, FBAR_H, COL_FBAR);
    hline(buf, bw, bh, spx, 0, FBAR_H - 1, (int)bw, COL_GRID);
    {
        /* "A1: <raw content or live edit>" */
        char label[8];
        label[0] = (char)('A' + g_sel_col);
        int li = 1;
        int rownum = g_sel_row + 1;
        if (rownum >= 10) { label[li++] = (char)('0' + rownum / 10); }
        label[li++] = (char)('0' + rownum % 10);
        label[li++] = ':';
        label[li++] = ' ';
        label[li] = '\0';

        int x = 4;
        draw_text_clip(buf, spx, bw, bh, x, 3, 80, label, COL_HDRTEXT);
        x += (int)k_strlen(label) * FONT_W;

        const char *content = g_editing
                                  ? g_edit
                                  : g_cells[g_sel_row][g_sel_col];
        draw_text_clip(buf, spx, bw, bh, x, 3,
                       (int)bw - x - 4, content, COL_TEXT);
    }

    /* ---- Column headers (A..H) ---- */
    fill_rect(buf, bw, bh, spx, GRID_X0, GRID_Y0, (int)bw, HDR_H, COL_HDR);
    /* corner box above row header */
    fill_rect(buf, bw, bh, spx, GRID_X0, GRID_Y0, ROWHDR_W, HDR_H, COL_HDR);
    for (int c = 0; c < NCOLS; c++) {
        int x = DATA_X0 + c * COL_W;
        u32 hc = (c == g_sel_col) ? COL_HDR_SEL : COL_HDR;
        fill_rect(buf, bw, bh, spx, x, GRID_Y0, COL_W, HDR_H, hc);
        char lbl[2] = { (char)('A' + c), '\0' };
        int tx = x + (COL_W - FONT_W) / 2;
        font_draw_char(buf, (int)spx, (int)bw, (int)bh, tx, GRID_Y0 + 1,
                       lbl[0], COL_HDRTEXT);
    }

    /* ---- Row headers (1..15) and cells ---- */
    for (int r = 0; r < NROWS; r++) {
        int y = DATA_Y0 + r * ROW_H;

        /* row header */
        u32 rhc = (r == g_sel_row) ? COL_HDR_SEL : COL_HDR;
        fill_rect(buf, bw, bh, spx, GRID_X0, y, ROWHDR_W, ROW_H, rhc);
        {
            int rn = r + 1;
            char num[3];
            int n = 0;
            if (rn >= 10) num[n++] = (char)('0' + rn / 10);
            num[n++] = (char)('0' + rn % 10);
            num[n] = '\0';
            int tw = n * FONT_W;
            int tx = GRID_X0 + (ROWHDR_W - tw) / 2;
            int ty = y + (ROW_H - FONT_H) / 2;
            draw_text_clip(buf, spx, bw, bh, tx, ty, ROWHDR_W, num,
                           COL_HDRTEXT);
        }

        /* data cells */
        for (int c = 0; c < NCOLS; c++) {
            int x = DATA_X0 + c * COL_W;
            int selected = (c == g_sel_col && r == g_sel_row);
            u32 bgc = selected ? COL_SEL : COL_CELL;
            fill_rect(buf, bw, bh, spx, x, y, COL_W, ROW_H, bgc);

            /* contents */
            const char *raw = g_cells[r][c];
            u32 txtc = COL_TEXT;
            const char *show;
            if (selected && g_editing) {
                show = g_edit;        /* live edit takes priority */
            } else if (raw[0] == '=') {
                cell_display(c, r, tmp, sizeof(tmp));
                show = tmp;
                txtc = COL_FORMULA;
            } else {
                show = raw;
            }
            int ty = y + (ROW_H - FONT_H) / 2;
            draw_text_clip(buf, spx, bw, bh, x + 3, ty, COL_W - 6, show, txtc);
        }
    }

    /* ---- Grid lines ---- */
    /* vertical lines */
    for (int c = 0; c <= NCOLS; c++) {
        int x = DATA_X0 + c * COL_W;
        vline(buf, bw, bh, spx, x, GRID_Y0, HDR_H + NROWS * ROW_H, COL_GRID);
    }
    /* left edge of row-header column */
    vline(buf, bw, bh, spx, GRID_X0, GRID_Y0, HDR_H + NROWS * ROW_H, COL_GRID);
    /* horizontal lines */
    for (int r = 0; r <= NROWS; r++) {
        int y = DATA_Y0 + r * ROW_H;
        hline(buf, bw, bh, spx, GRID_X0, y, ROWHDR_W + NCOLS * COL_W, COL_GRID);
    }
    /* top of header row and bottom of header row */
    hline(buf, bw, bh, spx, GRID_X0, GRID_Y0, ROWHDR_W + NCOLS * COL_W, COL_GRID);

    /* ---- Selection outline (2px) over the selected cell ---- */
    {
        int x = DATA_X0 + g_sel_col * COL_W;
        int y = DATA_Y0 + g_sel_row * ROW_H;
        u32 oc = 0xFF1A6FE0u;
        hline(buf, bw, bh, spx, x, y, COL_W, oc);
        hline(buf, bw, bh, spx, x, y + 1, COL_W, oc);
        hline(buf, bw, bh, spx, x, y + ROW_H - 1, COL_W, oc);
        hline(buf, bw, bh, spx, x, y + ROW_H - 2, COL_W, oc);
        vline(buf, bw, bh, spx, x, y, ROW_H, oc);
        vline(buf, bw, bh, spx, x + 1, y, ROW_H, oc);
        vline(buf, bw, bh, spx, x + COL_W - 1, y, ROW_H, oc);
        vline(buf, bw, bh, spx, x + COL_W - 2, y, ROW_H, oc);
    }
}

/* -----------------------------------------------------------------------
 * Key handling.
 * --------------------------------------------------------------------- */
static void on_key(int kc)
{
    /* Navigation keys (do not depend on the edit state). */
    switch (kc) {
    case KC_LEFT:  move_sel(-1, 0); return;
    case KC_RIGHT: move_sel(+1, 0); return;
    case KC_UP:    move_sel(0, -1); return;
    case KC_DOWN:  move_sel(0, +1); return;
    case KC_ENTER:
        edit_commit();
        /* move down one row (stay at last) */
        if (g_sel_row < NROWS - 1) g_sel_row++;
        return;
    case KC_BACKSPACE: {
        if (!g_editing) edit_begin();
        int n = (int)k_strlen(g_edit);
        if (n > 0) g_edit[n - 1] = '\0';
        return;
    }
    default:
        break;
    }

    /* Printable character? */
    char ch = remap_for_sheet(kc);
    if (ch == 0) return;

    if (!g_editing) {
        /* Start a fresh edit: typing replaces the cell (spreadsheet feel). */
        g_edit[0] = '\0';
        g_editing = 1;
    }
    int n = (int)k_strlen(g_edit);
    if (n < CELL_MAX - 1) {
        g_edit[n] = ch;
        g_edit[n + 1] = '\0';
    }
}

/* -----------------------------------------------------------------------
 * Pointer handling: left-click selects (and commits any current edit).
 * --------------------------------------------------------------------- */
static int g_prev_btn = 0;

static void on_pointer(int mx, int my, int buttons)
{
    int left = buttons & 1;
    if (left && !g_prev_btn) {
        /* fresh press */
        int c, r;
        if (hit_cell(mx, my, &c, &r)) {
            edit_commit();
            g_sel_col = c;
            g_sel_row = r;
        }
    }
    g_prev_btn = left;
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    print("[SHEET] starting\n");

    /* Zero the cell store (it's BSS, already zero, but be explicit about
       the edit buffer). */
    g_edit[0] = '\0';

    /* Seed a couple of demo cells so the grid is not empty on launch and
       a formula is visible/working immediately. */
    k_strcpy(g_cells[0][0], "10", CELL_MAX);        /* A1 = 10        */
    k_strcpy(g_cells[1][0], "20", CELL_MAX);        /* A2 = 20        */
    k_strcpy(g_cells[2][0], "30", CELL_MAX);        /* A3 = 30        */
    k_strcpy(g_cells[0][1], "5", CELL_MAX);         /* B1 = 5         */
    k_strcpy(g_cells[0][2], "=A1+B1", CELL_MAX);    /* C1 = A1+B1 =15 */
    k_strcpy(g_cells[1][2], "=A1*B1", CELL_MAX);    /* C2 = A1*B1 =50 */
    k_strcpy(g_cells[2][2], "=A1/A2", CELL_MAX);    /* C3 = 10/20=0.5 */
    k_strcpy(g_cells[3][2], "=SUM(A1:A3)", CELL_MAX); /* C4 = 60     */

    if (wl_connect() != 0) {
        print("[SHEET] wl_connect FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Sheet");
    if (!win) {
        print("[SHEET] wl_create_window FAILED\n");
        for (;;) sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    print("[SHEET] window created\n");

    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                /* a=keycode, b=pressed */
                if (b) on_key(a);          /* only on key-down */
            } else if (kind == WL_EVENT_POINTER) {
                on_pointer(a, b, c);
            }
        }

        render(win);
        wl_commit(win);
        sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}

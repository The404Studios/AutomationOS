/*
 * calculator.c -- Integer calculator GUI app (freestanding, ring 3).
 * ==================================================================
 *
 * Opens a 240x320 window titled "Calculator".  The layout is:
 *
 *   +------------------+  y=10
 *   |  display label   |  (right-justified text area, up to 16 chars)
 *   +------------------+  y=52
 *   |  [7] [8] [9] [/] |  y=62
 *   |  [4] [5] [6] [*] |  y=118
 *   |  [1] [2] [3] [-] |  y=174
 *   |  [0] [C] [=] [+] |  y=230
 *   +------------------+
 *
 * Arithmetic: integer +, -, *, / (division truncates toward zero).
 *
 * State is held in a file-static calc_state_t struct; every button
 * callback receives a pointer to that struct (the `ud` parameter).
 *
 * No libc: pure inline syscalls + tiny freestanding helpers.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/calculator/calculator.c -o /tmp/calc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/ui/ui.c -o /tmp/ui.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/calc.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/calc.elf
 *   objdump -d /tmp/calc.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [CALC] starting
 *   [CALC] = <result>
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall helpers -- no libc.
 * --------------------------------------------------------------------- */

#define SYS_WRITE 3

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* -----------------------------------------------------------------------
 * Freestanding int-to-string helpers.
 * --------------------------------------------------------------------- */

/*
 * Format a signed long into buf (must be >= 22 bytes).
 * Returns a pointer into buf where the digits start (may not be buf[0]
 * because we build the string from the back).  Always NUL-terminates.
 */
static char *fmt_long(long val, char *buf, int bufsz)
{
    int negative = (val < 0);
    unsigned long uval = negative ? (unsigned long)(-(unsigned long)val) : (unsigned long)val;

    /* Write from the end of the buffer. */
    int i = bufsz - 1;
    buf[i] = '\0';
    i--;

    if (uval == 0) {
        buf[i] = '0';
        i--;
    } else {
        while (uval > 0 && i >= 0) {
            buf[i] = (char)('0' + (int)(uval % 10));
            uval /= 10;
            i--;
        }
    }

    if (negative && i >= 0) {
        buf[i] = '-';
        i--;
    }

    /* Return pointer to the first character of the number. */
    return &buf[i + 1];
}

/* Copy src into dst (including NUL).  dst must have room. */
static void k_strcpy(char *dst, const char *src)
{
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* String compare; returns 0 if equal. */
static int k_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Append src onto dst (dst must have room). */
static void k_strcat(char *dst, const char *src)
{
    int d = 0;
    while (dst[d]) d++;
    int s = 0;
    while (src[s]) { dst[d++] = src[s++]; }
    dst[d] = '\0';
}

/* -----------------------------------------------------------------------
 * Calculator state.
 * --------------------------------------------------------------------- */

/*
 * ENTRY_MAX: maximum digits the user can type for a single number.
 * DISPLAY_MAX: display buffer size (includes sign + digits + NUL).
 */
#define ENTRY_MAX    16
#define DISPLAY_MAX  20

typedef struct {
    /* Current digit string being built (NUL-terminated). */
    char entry[ENTRY_MAX + 1];

    /* Accumulator: value of the left-hand operand. */
    long accumulator;

    /*
     * Pending operator: one of '\0' (none), '+', '-', '*', '/'.
     * When an operator key is pressed:
     *   1. The current entry is committed to the accumulator (if an
     *      operator was already pending, the binary op is evaluated first).
     *   2. The pending operator is updated.
     *   3. entry is cleared so the user can type the right-hand operand.
     */
    char op;

    /*
     * fresh_result: set after '=' so that the next digit press starts a
     * brand-new entry rather than appending to the result.
     */
    int fresh_result;

    /* Pointer to the display label widget. */
    ui_widget_t *display;
} calc_state_t;

/* File-static instance -- shared by all callbacks. */
static calc_state_t g_calc;

/* -----------------------------------------------------------------------
 * Display update helper.
 * --------------------------------------------------------------------- */

/*
 * Refresh the display label.  If entry is non-empty we show the entry;
 * otherwise we show the accumulator.
 */
static void update_display(calc_state_t *st)
{
    char buf[DISPLAY_MAX];

    if (st->entry[0] != '\0') {
        /* Showing the live entry string. */
        k_strcpy(buf, st->entry);
    } else {
        /* Showing the accumulator (after an operator press or clear). */
        char tmp[24];
        k_strcpy(buf, fmt_long(st->accumulator, tmp, sizeof(tmp)));
    }

    ui_label_set_text(st->display, buf);
}

/* -----------------------------------------------------------------------
 * Parse the entry string into a signed long.
 * Handles optional leading '-' (though the UI never lets the user type
 * one directly -- only the accumulator can be negative).
 * --------------------------------------------------------------------- */
static long parse_entry(const char *s)
{
    long result = 0;
    int negative = 0;
    int i = 0;

    if (s[i] == '-') { negative = 1; i++; }

    while (s[i] >= '0' && s[i] <= '9') {
        result = result * 10 + (s[i] - '0');
        i++;
    }

    return negative ? -result : result;
}

/* -----------------------------------------------------------------------
 * Evaluate accumulator OP entry -> accumulator.
 * Returns 0 on success, -1 on divide-by-zero (result set to 0).
 * --------------------------------------------------------------------- */
static int apply_op(calc_state_t *st)
{
    if (st->op == '\0') {
        /* No pending operator; just commit entry as new accumulator. */
        st->accumulator = parse_entry(st->entry);
        return 0;
    }

    long rhs = parse_entry(st->entry);
    switch (st->op) {
    case '+': st->accumulator = st->accumulator + rhs; break;
    case '-': st->accumulator = st->accumulator - rhs; break;
    case '*': st->accumulator = st->accumulator * rhs; break;
    case '/':
        if (rhs == 0) {
            st->accumulator = 0;
            return -1;
        }
        st->accumulator = st->accumulator / rhs;
        break;
    default: break;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Button on_click callbacks.
 * All callbacks receive &g_calc as `ud`.
 * --------------------------------------------------------------------- */

/*
 * Digit button handler.
 * `ud` points to the calc_state_t.  The digit character is encoded as the
 * first byte of a tiny 2-byte string stored right after the struct pointer
 * in the per-button ud_pair below.
 *
 * We use a small (on-stack at _start) array of ud_pair_t objects, one per
 * digit button, so each callback knows which digit to append.
 */
typedef struct {
    calc_state_t *st;
    char          digit; /* '0'..'9' */
} ud_digit_t;

static void on_digit(void *ud)
{
    ud_digit_t   *d  = (ud_digit_t *)ud;
    calc_state_t *st = d->st;

    /*
     * If the previous action was '=', start fresh instead of appending to
     * the result that is sitting in the accumulator.
     */
    if (st->fresh_result) {
        st->entry[0]    = '\0';
        st->fresh_result = 0;
    }

    /* Append digit (if there is room). */
    unsigned long len = k_strlen(st->entry);
    if (len < ENTRY_MAX) {
        st->entry[len]     = d->digit;
        st->entry[len + 1] = '\0';
    }

    update_display(st);
}

/*
 * Operator button (+, -, *, /).
 */
typedef struct {
    calc_state_t *st;
    char          op;
} ud_op_t;

static void on_op(void *ud)
{
    ud_op_t      *o  = (ud_op_t *)ud;
    calc_state_t *st = o->st;

    /*
     * If there is a live entry, evaluate whatever was pending, then set the
     * new operator and clear the entry.
     */
    if (st->entry[0] != '\0') {
        apply_op(st);
        st->entry[0] = '\0';
    }
    /* If entry was empty (e.g. two consecutive operator presses) just
     * replace the pending operator without touching the accumulator. */

    st->op           = o->op;
    st->fresh_result = 0;

    update_display(st);
}

/*
 * Equals.
 */
static void on_equals(void *ud)
{
    calc_state_t *st = (calc_state_t *)ud;

    if (st->entry[0] != '\0') {
        apply_op(st);
        st->entry[0] = '\0';
    }
    st->op           = '\0';
    st->fresh_result = 1;

    update_display(st);

    /* Serial: "[CALC] = N\n" */
    char tmp[24];
    char *numstr = fmt_long(st->accumulator, tmp, sizeof(tmp));
    serial_print("[CALC] = ");
    serial_print(numstr);
    serial_print("\n");
}

/*
 * Clear.
 */
static void on_clear(void *ud)
{
    calc_state_t *st = (calc_state_t *)ud;

    st->entry[0]     = '\0';
    st->accumulator  = 0;
    st->op           = '\0';
    st->fresh_result = 0;

    ui_label_set_text(st->display, "0");
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */

/*
 * Static storage for per-button user-data structs.
 * (Cannot use stack-allocated structs whose addresses outlive _start,
 *  because _start never returns but the widgets retain the pointers --
 *  stack memory is safe here only because _start loops forever via
 *  ui_app_run, but static storage is cleaner and unambiguous.)
 */
static ud_digit_t g_ud_digits[10]; /* '0'..'9' */
static ud_op_t    g_ud_ops[4];     /* '+','-','*','/' */

void _start(void)
{
    serial_print("[CALC] starting\n");

    /* ---- Initialise calculator state. ---- */
    g_calc.entry[0]    = '\0';
    g_calc.accumulator = 0;
    g_calc.op          = '\0';
    g_calc.fresh_result = 0;
    g_calc.display      = 0;   /* filled in below */

    /* ---- Create window: 240 x 320. ---- */
    ui_app_t    *app  = ui_app_create("Calculator", 240, 320);
    ui_widget_t *root = ui_app_root(app);

    /* ---------------------------------------------------------------
     * Layout constants.
     *
     * Window: 240 wide, 320 tall.
     * Display panel:  x=8,  y=10, w=224, h=44
     * Button grid starts at y=62; each button is 50w x 48h.
     * Four columns at x = 8, 66, 124, 182  (gap 8px).
     * Four rows    at y = 62, 118, 174, 230 (gap 8px).
     * ------------------------------------------------------------- */

    /* Display panel (dark background, slightly lighter than the window). */
    ui_widget_t *disp_panel = ui_panel(root, 8, 10, 224, 44, 0xFF2C2C2E);

    /*
     * Display label: right-justified look -- place it at x=8 within the
     * panel (the toolkit left-aligns text, so we position it so longer
     * numbers appear near the right edge).  Text "0" on startup.
     */
    g_calc.display = ui_label(disp_panel, 8, 14, "0", 0xFFFFFFFF);

    /* ---- Set up per-digit ud structs. ---- */
    for (int i = 0; i <= 9; i++) {
        g_ud_digits[i].st    = &g_calc;
        g_ud_digits[i].digit = (char)('0' + i);
    }

    /* ---- Set up per-operator ud structs. ---- */
    const char ops[4] = { '+', '-', '*', '/' };
    for (int i = 0; i < 4; i++) {
        g_ud_ops[i].st = &g_calc;
        g_ud_ops[i].op = ops[i];
    }

    /*
     * Button grid helper macro -- reduces repetition.
     * COL(c): x for column c (0-based, 0=leftmost).
     * ROW(r): y for row r (0-based, 0=topmost digit row).
     */
#define BTN_W 50
#define BTN_H 48
#define GAP    8
#define COL(c) (GAP + (c) * (BTN_W + GAP))
#define ROW(r) (62  + (r) * (BTN_H + GAP))

    /*
     * Row 0: 7  8  9  /
     */
    ui_button(root, COL(0), ROW(0), BTN_W, BTN_H, "7",
              on_digit, (void *)&g_ud_digits[7]);
    ui_button(root, COL(1), ROW(0), BTN_W, BTN_H, "8",
              on_digit, (void *)&g_ud_digits[8]);
    ui_button(root, COL(2), ROW(0), BTN_W, BTN_H, "9",
              on_digit, (void *)&g_ud_digits[9]);
    /* '/' is ops[3] */
    ui_button(root, COL(3), ROW(0), BTN_W, BTN_H, "/",
              on_op, (void *)&g_ud_ops[3]);

    /*
     * Row 1: 4  5  6  *
     */
    ui_button(root, COL(0), ROW(1), BTN_W, BTN_H, "4",
              on_digit, (void *)&g_ud_digits[4]);
    ui_button(root, COL(1), ROW(1), BTN_W, BTN_H, "5",
              on_digit, (void *)&g_ud_digits[5]);
    ui_button(root, COL(2), ROW(1), BTN_W, BTN_H, "6",
              on_digit, (void *)&g_ud_digits[6]);
    /* '*' is ops[2] */
    ui_button(root, COL(3), ROW(1), BTN_W, BTN_H, "*",
              on_op, (void *)&g_ud_ops[2]);

    /*
     * Row 2: 1  2  3  -
     */
    ui_button(root, COL(0), ROW(2), BTN_W, BTN_H, "1",
              on_digit, (void *)&g_ud_digits[1]);
    ui_button(root, COL(1), ROW(2), BTN_W, BTN_H, "2",
              on_digit, (void *)&g_ud_digits[2]);
    ui_button(root, COL(2), ROW(2), BTN_W, BTN_H, "3",
              on_digit, (void *)&g_ud_digits[3]);
    /* '-' is ops[1] */
    ui_button(root, COL(3), ROW(2), BTN_W, BTN_H, "-",
              on_op, (void *)&g_ud_ops[1]);

    /*
     * Row 3: 0  C  =  +
     */
    ui_button(root, COL(0), ROW(3), BTN_W, BTN_H, "0",
              on_digit, (void *)&g_ud_digits[0]);
    ui_button(root, COL(1), ROW(3), BTN_W, BTN_H, "C",
              on_clear, (void *)&g_calc);
    ui_button(root, COL(2), ROW(3), BTN_W, BTN_H, "=",
              on_equals, (void *)&g_calc);
    /* '+' is ops[0] */
    ui_button(root, COL(3), ROW(3), BTN_W, BTN_H, "+",
              on_op, (void *)&g_ud_ops[0]);

#undef BTN_W
#undef BTN_H
#undef GAP
#undef COL
#undef ROW

    /* ---- Enter the event loop (never returns). ---- */
    ui_app_run(app);
}

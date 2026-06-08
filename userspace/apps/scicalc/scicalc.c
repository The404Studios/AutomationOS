/*
 * scicalc.c -- Scientific calculator GUI app (freestanding, ring 3).
 * ==================================================================
 *
 * Window: 400 x 560, titled "Scientific Calculator".
 *
 * Layout (top to bottom):
 *   Expression row  (y=  8, h=28): what the user typed
 *   Result row      (y= 40, h=36): current result / entry value
 *   Scientific grid (y= 88, 5 rows x 5 cols)
 *   Basic grid      (y=328, 5 rows x 4 cols)
 *
 * Fixed-point math: all values are int64_t scaled by FP_SCALE = 1 000 000
 * (6 decimal places).  The integer part occupies bits 63..20 (roughly),
 * and the fractional part occupies bits 19..0 in the sense that
 *   true_value = fp_value / 1_000_000
 *
 * Implemented functions:
 *   Basic: + - * / = . +/- % C CE
 *   Scientific: sin cos tan (DEG/RAD), asin acos atan,
 *               sqrt x^2 x^y 1/x ln log10 exp pi e
 *               n! (up to 20)  M+ M- MR MC
 *
 * Accuracy: ~5-6 significant digits (adequate for a fixed-point Taylor/
 * Newton implementation without libm).
 *
 * Build (flags DIRECTLY on the command line, no shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/scicalc/scicalc.c -o /tmp/scicalc.o
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
 *       /tmp/scicalc.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/scicalc.elf
 *   objdump -d /tmp/scicalc.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/ui/ui.h"

/* =========================================================================
 * Syscall helpers (no libc).
 * ========================================================================= */

#define SYS_WRITE  3
#define SYS_YIELD  15

static inline long sc3(long n, long a1, long a2, long a3)
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
    sc3(SYS_WRITE, 1, (long)m, (long)k_strlen(m));
}

/* =========================================================================
 * String helpers (no libc).
 * ========================================================================= */

static void k_strcpy(char *dst, const char *src)
{
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void k_strcat(char *dst, const char *src)
{
    int d = 0;
    while (dst[d]) d++;
    int s = 0;
    while (src[s]) { dst[d++] = src[s++]; }
    dst[d] = '\0';
}

/* Bounded strcat: appends src to dst, never writes past dst[cap-1]. */
static void k_strcat_n(char *dst, int cap, const char *src)
{
    int d = 0;
    while (d < cap && dst[d]) d++;
    int s = 0;
    while (d < cap - 1 && src[s]) { dst[d++] = src[s++]; }
    if (d < cap) dst[d] = '\0';
}

static int k_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================================
 * Fixed-point arithmetic.
 *
 * Representation: int64_t where 1 unit == 1 / FP_SCALE real.
 * FP_ONE = FP_SCALE = 1 000 000  (10^6).
 * Max representable integer part: ~9.2 * 10^12.
 * ========================================================================= */

typedef long long fp_t;

#define FP_SCALE   1000000LL          /* 10^6  */
#define FP_ONE     FP_SCALE
#define FP_HALF    (FP_SCALE / 2)

/* Integer -> fp */
static inline fp_t fp_from_int(long long v) { return v * FP_SCALE; }

/*
 * muldiv64(a, b, c) = (a * b) / c  using x86-64 mul/div instructions.
 * Handles the 64x64->128 intermediate without __int128 library calls.
 * Signs: computes in unsigned, fixes sign at end.
 *
 * MUST NOT be inlined because the divq asm needs dedicated rax/rdx
 * registers that the register allocator can't guarantee when inlined
 * into an already-complex function.
 */
static __attribute__((noinline)) long long muldiv64(long long a, long long b, long long c)
{
    int neg = 0;
    unsigned long long ua, ub, uc;

    if (a < 0) { ua = (unsigned long long)(-a); neg ^= 1; } else { ua = (unsigned long long)a; }
    if (b < 0) { ub = (unsigned long long)(-b); neg ^= 1; } else { ub = (unsigned long long)b; }
    if (c < 0) { uc = (unsigned long long)(-c); neg ^= 1; } else { uc = (unsigned long long)c; }

    if (uc == 0) return 0;

    unsigned long long lo, hi, quot;
    /* lo:hi = ua * ub  (128-bit unsigned product) */
    asm volatile("mulq %3"
        : "=a"(lo), "=d"(hi)
        : "a"(ua), "r"(ub));

    /* If hi >= uc the quotient would overflow 64 bits -- clamp. */
    if (hi >= uc) {
        quot = 0x7FFFFFFFFFFFFFFFULL;
    } else {
        /* quot = (hi:lo) / uc */
        asm volatile("divq %3"
            : "=a"(quot), "=d"(lo)   /* lo reused as remainder output */
            : "A"(lo), "r"(uc), "d"(hi));
    }

    return neg ? -(long long)quot : (long long)quot;
}

/* fp multiply: (a/S) * (b/S) = (a*b)/S^2, so divide by S once. */
static fp_t fp_mul(fp_t a, fp_t b)
{
    return (fp_t)muldiv64(a, b, FP_SCALE);
}

/* fp divide: (a/S) / (b/S) = a/b, so multiply numerator by S first. */
static fp_t fp_div(fp_t a, fp_t b)
{
    if (b == 0) return 0;
    return (fp_t)muldiv64(a, FP_SCALE, b);
}

/* Absolute value. */
static inline fp_t fp_abs(fp_t a) { return a < 0 ? -a : a; }

/* -------------------------------------------------------------------------
 * Constants in fixed-point.
 * ------------------------------------------------------------------------- */
/* pi = 3.14159265...   * 10^6 */
#define FP_PI     3141593LL
/* e  = 2.71828182...   * 10^6 */
#define FP_E      2718282LL
/* pi/2 */
#define FP_PI_2   1570796LL
/* pi/4 */
#define FP_PI_4   785398LL
/* ln(2) */
#define FP_LN2    693147LL
/* 1/ln(10) = log10(e) */
#define FP_LOG10E 434294LL

/* -------------------------------------------------------------------------
 * fp_sqrt -- integer Newton-Raphson.
 * Computes floor(sqrt(x * FP_SCALE)) so the result is in fp format.
 * Works for x >= 0.
 * ------------------------------------------------------------------------- */
static fp_t fp_sqrt(fp_t x)
{
    if (x <= 0) return 0;

    /*
     * We want sqrt(x) where x is fp (x/S represents the real value).
     * sqrt(x/S) = sqrt(x) / sqrt(S).
     * Since S = 10^6, sqrt(S) = 10^3 = 1000.
     * So: result_fp = sqrt(x_raw) / 1000 * FP_SCALE
     *               = sqrt(x_raw) * 1000    (because FP_SCALE / 1000 = 1000)
     * We compute integer sqrt of x_raw and multiply by 1000.
     *
     * Integer sqrt via Newton: x_{n+1} = (x_n + N/x_n) / 2.
     */
    long long N = x;  /* raw fixed-point bits */
    if (N <= 0) return 0;

    /* Initial estimate: bit-length heuristic. */
    long long g = N;
    /* Narrow to a reasonable range. */
    for (int i = 0; i < 40; i++) {
        long long ng = (g + N / g) / 2;
        if (ng >= g) break;
        g = ng;
    }
    /* g ~ sqrt(N); result = g * 1000 (see above). */
    return (fp_t)(g * 1000LL);
}

/* -------------------------------------------------------------------------
 * fp_sin / fp_cos / fp_tan (argument in fixed-point radians).
 *
 * Strategy: range-reduce to [-pi/4, pi/4] by quadrant, then apply
 * Taylor series:
 *   sin(x) = x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880 - ...
 *   cos(x) = 1 - x^2/2 + x^4/24 - x^6/720 + x^8/40320 - ...
 *
 * Eight terms each are sufficient for |x| <= pi/4 (~0.785) to ~7 digits.
 * ------------------------------------------------------------------------- */

/* Raw Taylor sin for |x| <= pi/4. */
static fp_t sin_small(fp_t x)
{
    fp_t x2 = fp_mul(x, x);
    fp_t term = x;                         /* x^1 / 1! */
    fp_t s    = term;

    /* -x^3/3! */
    term = fp_div(fp_mul(term, x2), fp_from_int(6));
    s   -= term;

    /* +x^5/5! */
    term = fp_div(fp_mul(term, x2), fp_from_int(20));
    s   += term;

    /* -x^7/7! */
    term = fp_div(fp_mul(term, x2), fp_from_int(42));
    s   -= term;

    /* +x^9/9! */
    term = fp_div(fp_mul(term, x2), fp_from_int(72));
    s   += term;

    /* -x^11/11! */
    term = fp_div(fp_mul(term, x2), fp_from_int(110));
    s   -= term;

    return s;
}

/* Raw Taylor cos for |x| <= pi/4. */
static fp_t cos_small(fp_t x)
{
    fp_t x2 = fp_mul(x, x);
    fp_t term = FP_ONE;                    /* x^0 / 0! = 1 */
    fp_t c    = term;

    /* -x^2/2! */
    term = fp_div(fp_mul(term, x2), fp_from_int(2));
    c   -= term;

    /* +x^4/4! */
    term = fp_div(fp_mul(term, x2), fp_from_int(12));
    c   += term;

    /* -x^6/6! */
    term = fp_div(fp_mul(term, x2), fp_from_int(30));
    c   -= term;

    /* +x^8/8! */
    term = fp_div(fp_mul(term, x2), fp_from_int(56));
    c   += term;

    /* -x^10/10! */
    term = fp_div(fp_mul(term, x2), fp_from_int(90));
    c   -= term;

    /* +x^12/12! */
    term = fp_div(fp_mul(term, x2), fp_from_int(132));
    c   += term;

    return c;
}

/* Range-reduce to (-pi, pi] then to [-pi/4, pi/4] via quadrant. */
static fp_t fp_sin(fp_t x)
{
    /* Reduce to (-pi, pi] */
    fp_t two_pi = 2 * FP_PI;
    while (x >  FP_PI) x -= two_pi;
    while (x < -FP_PI) x += two_pi;

    /* Quadrant reduction. */
    if (x > FP_PI_2) {
        /* sin(x) = sin(pi - x) */
        return sin_small(FP_PI - x);
    } else if (x < -FP_PI_2) {
        /* sin(x) = -sin(-pi - x) = sin(x + pi) ... easier: */
        return -sin_small(FP_PI + x);
    } else if (x > FP_PI_4) {
        /* sin(x) = cos(pi/2 - x) */
        return cos_small(FP_PI_2 - x);
    } else if (x < -FP_PI_4) {
        /* sin(x) = -cos(pi/2 + x) */
        return -cos_small(FP_PI_2 + x);
    }
    return sin_small(x);
}

static fp_t fp_cos(fp_t x)
{
    /* cos(x) = sin(x + pi/2) */
    return fp_sin(x + FP_PI_2);
}

static fp_t fp_tan(fp_t x)
{
    fp_t c = fp_cos(x);
    if (c == 0) return 0;  /* undefined: return 0 */
    return fp_div(fp_sin(x), c);
}

/* -------------------------------------------------------------------------
 * fp_ln -- natural logarithm via series + range reduction.
 *
 * For x > 0:
 *   Reduce:  x = m * 2^k  so that m in [1, 2).
 *   ln(x) = k * ln(2) + ln(m).
 *   For m in [1,2), let u = (m-1)/(m+1), |u| < 1/3.
 *   ln(m) = 2*(u + u^3/3 + u^5/5 + u^7/7 + ...).
 *   Converges fast for small u.
 * ------------------------------------------------------------------------- */
static fp_t fp_ln(fp_t x)
{
    if (x <= 0) return -fp_from_int(999999);  /* error sentinel */

    /* Count integer doublings / halvings to get x into [FP_ONE, 2*FP_ONE). */
    int k = 0;
    while (x >= 2 * FP_ONE) { x >>= 1; k++;  }
    while (x <      FP_ONE) { x <<= 1; k--;  }

    /*
     * Now x is in [FP_ONE, 2*FP_ONE), representing a value in [1, 2).
     * u = (x - 1) / (x + 1).
     */
    fp_t u = fp_div(x - FP_ONE, x + FP_ONE);
    fp_t u2 = fp_mul(u, u);

    /* Series: 2*(u + u^3/3 + u^5/5 + ... 9 terms) */
    fp_t term = u;
    fp_t sum  = term;

    term = fp_div(fp_mul(term, u2), fp_from_int(3));
    sum += term;
    term = fp_div(fp_mul(term, u2), fp_from_int(5) / 3);
    /* Recurrence: next divisor = (2n+1)/(2n-1) */
    /* Easier: just unroll with explicit divisors */

    /* Restart with explicit terms for clarity: */
    sum = u;
    fp_t upow = u;
    /* 2*(u^1/1 + u^3/3 + u^5/5 + u^7/7 + u^9/9 + u^11/11 + u^13/13) */
    static const long long divs[] = {3, 5, 7, 9, 11, 13, 15, 17};
    for (int i = 0; i < 8; i++) {
        upow = fp_mul(fp_mul(upow, u2), FP_ONE); /* upow *= u^2 */
        /* We need upow /1 already in fp; fp_mul keeps scale. */
        sum += fp_div(upow, fp_from_int(divs[i]));
    }
    fp_t ln_m = 2 * sum;  /* Already in fp_t (2 * sum is fine, sum <= 1) */

    /* ln(x) = k * ln(2) + ln_m */
    return (fp_t)(k * FP_LN2) + ln_m;
}

/* -------------------------------------------------------------------------
 * fp_exp -- e^x via series + range reduction.
 *
 * Reduce: x = n + f where n = round(x/ln2)*ln2, f = x - n.
 *   e^x = e^(k*ln2) * e^f = 2^k * e^f.
 *   Taylor for e^f, |f| <= ln2/2 ~ 0.347.
 * ------------------------------------------------------------------------- */
static fp_t fp_exp(fp_t x)
{
    /* Clamp to avoid overflow (fp max ~9e12, e^27 ~ 5e11). */
    if (x > fp_from_int(27))  return fp_from_int(500000000000LL);
    if (x < fp_from_int(-27)) return 0;

    /* k = round(x / ln2) */
    long long k = (long long)fp_div(x, FP_LN2) / FP_SCALE;
    /* Adjust fractional part. */
    fp_t f = x - (fp_t)(k * FP_LN2);

    /* Taylor: e^f = 1 + f + f^2/2! + f^3/3! + ... (10 terms) */
    fp_t result = FP_ONE;
    fp_t term   = FP_ONE;
    for (int n = 1; n <= 12; n++) {
        term = fp_div(fp_mul(term, f), fp_from_int(n));
        result += term;
    }

    /* Multiply by 2^k */
    if (k >= 0) {
        for (int i = 0; i < k && i < 40; i++) result *= 2;
    } else {
        long long absk = -k;
        for (long long i = 0; i < absk && i < 40; i++) result /= 2;
    }
    return result;
}

/* -------------------------------------------------------------------------
 * fp_pow -- x^y = exp(y * ln(x)).
 * Special-case: y is integer.
 * ------------------------------------------------------------------------- */
static fp_t fp_pow(fp_t x, fp_t y)
{
    if (x <= 0) {
        /* Integer exponent: handle negative x */
        if (x < 0) {
            long long yi = y / FP_SCALE;
            if (fp_abs(y - fp_from_int(yi)) < 1000) {
                /* Integer y */
                fp_t r = FP_ONE;
                fp_t ax = -x;
                for (long long i = 0; i < yi && i < 60; i++)
                    r = fp_mul(r, ax);
                return (yi & 1) ? -r : r;
            }
        }
        return 0;
    }
    return fp_exp(fp_mul(y, fp_ln(x)));
}

/* -------------------------------------------------------------------------
 * fp_log10 -- log base-10 = ln(x) / ln(10).
 * ln(10) = ln(2) * log2(10) ~ 2.302585 * 10^6.
 * ------------------------------------------------------------------------- */
#define FP_LN10  2302585LL

static fp_t fp_log10(fp_t x)
{
    return fp_div(fp_ln(x), FP_LN10);
}

/* -------------------------------------------------------------------------
 * fp_factorial -- n! for integer n, result in fp.
 * Capped at 20! to avoid overflow.
 * ------------------------------------------------------------------------- */
static fp_t fp_factorial(long long n)
{
    if (n < 0)  return 0;
    if (n > 20) return fp_from_int(2432902008176640000LL); /* 20! */
    long long r = 1;
    for (long long i = 2; i <= n; i++) r *= i;
    return fp_from_int(r);
}

/* =========================================================================
 * Fixed-point -> decimal string.
 *
 * Produces up to 10 significant digits with a decimal point.
 * buf must be >= 32 bytes.
 * ========================================================================= */
static void fp_to_str(fp_t v, char *buf, int bufsz)
{
    if (bufsz < 2) return;

    int neg = (v < 0);
    if (neg) v = -v;

    long long int_part  = (long long)(v / FP_SCALE);
    long long frac_part = (long long)(v % FP_SCALE);  /* 0..999999 */

    /* Trim trailing zeros from frac by trimming to at most 6 sig frac digits. */
    /* We'll keep up to 6 fractional digits, trimming trailing zeros. */

    char tmp[32];
    /* Build integer part */
    int pos = 31;
    tmp[pos--] = '\0';

    if (int_part == 0) {
        tmp[pos--] = '0';
    } else {
        long long ip = int_part;
        while (ip > 0 && pos > 0) {
            tmp[pos--] = (char)('0' + (int)(ip % 10));
            ip /= 10;
        }
    }
    if (neg && pos > 0) tmp[pos--] = '-';
    pos++;

    /* Copy integer part to buf. */
    int bi = 0;
    for (int i = pos; tmp[i]; i++) {
        if (bi < bufsz - 1) buf[bi++] = tmp[i];
    }

    /* Fractional part: up to 6 digits, trimming trailing zeros. */
    if (frac_part != 0) {
        /* Produce 6-digit string with leading zeros. */
        char fstr[8];
        long long fp2 = frac_part;
        for (int i = 5; i >= 0; i--) {
            fstr[i] = (char)('0' + (int)(fp2 % 10));
            fp2 /= 10;
        }
        fstr[6] = '\0';
        /* Trim trailing zeros. */
        int flen = 6;
        while (flen > 1 && fstr[flen-1] == '0') flen--;
        fstr[flen] = '\0';
        if (bi < bufsz - 1) buf[bi++] = '.';
        for (int i = 0; fstr[i] && bi < bufsz - 1; i++)
            buf[bi++] = fstr[i];
    }
    buf[bi] = '\0';
}

/* Parse a decimal string into fp_t. */
static fp_t fp_from_str(const char *s)
{
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }

    long long int_part  = 0;
    long long frac_part = 0;
    long long frac_scale = FP_SCALE;  /* will divide by this */

    int in_frac = 0;
    while (*s) {
        if (*s == '.') { in_frac = 1; s++; continue; }
        if (*s < '0' || *s > '9') break;
        int d = *s - '0';
        if (!in_frac) {
            int_part = int_part * 10 + d;
        } else {
            /* Only track 6 fractional digits. */
            if (frac_scale > 1) {
                frac_part  = frac_part * 10 + d;
                frac_scale /= 10;
            }
        }
        s++;
    }

    fp_t result = fp_from_int(int_part) + frac_part * frac_scale;
    return neg ? -result : result;
}

/* =========================================================================
 * Calculator state.
 * ========================================================================= */

#define EXPR_MAX   48   /* expression string buffer */
#define ENTRY_MAX  24   /* current number entry     */
#define DISP_MAX   32   /* display string           */

/*
 * Operation codes.
 */
typedef enum {
    OP_NONE = 0,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_POW,        /* x^y */
} op_t;

typedef struct {
    /* Current digit/decimal entry string. */
    char    entry[ENTRY_MAX + 1];
    int     entry_has_dot;      /* 1 if '.' already typed */

    /* Pending binary operation. */
    op_t    op;
    fp_t    accumulator;        /* left-hand side */

    /* Memory register. */
    fp_t    memory;

    /* DEG=1, RAD=0. */
    int     deg_mode;

    /* Set after '=' or a unary function: next digit starts fresh. */
    int     fresh_result;

    /* Waiting for second operand of x^y. */
    int     awaiting_pow;

    /* Display widgets. */
    ui_widget_t *disp_expr;
    ui_widget_t *disp_result;

    /* DEG/RAD toggle button (so we can update its label). */
    ui_widget_t *btn_degrad;

    /* Expression accumulation string (for the top row). */
    char    expr_str[EXPR_MAX + 1];
} sc_state_t;

static sc_state_t g_sc;

/* =========================================================================
 * Display helpers.
 * ========================================================================= */

static void sc_update_display(sc_state_t *st)
{
    char rbuf[DISP_MAX];

    if (st->entry[0] != '\0') {
        k_strcpy(rbuf, st->entry);
    } else {
        fp_to_str(st->accumulator, rbuf, DISP_MAX);
    }

    ui_label_set_text(st->disp_result, rbuf);
    ui_label_set_text(st->disp_expr,   st->expr_str);
}

static void sc_set_result(sc_state_t *st, fp_t val)
{
    st->accumulator  = val;
    st->entry[0]     = '\0';
    st->entry_has_dot = 0;
    st->op            = OP_NONE;
    st->fresh_result  = 1;
    st->awaiting_pow  = 0;
    sc_update_display(st);
}

/* =========================================================================
 * Parse entry string -> fp_t.
 * ========================================================================= */
static fp_t sc_parse_entry(sc_state_t *st)
{
    if (st->entry[0] == '\0') return st->accumulator;
    return fp_from_str(st->entry);
}

/* =========================================================================
 * Apply pending binary op (accumulator OP entry -> accumulator).
 * ========================================================================= */
static void sc_apply_op(sc_state_t *st)
{
    if (st->entry[0] == '\0' && st->op == OP_NONE) return;

    fp_t rhs = sc_parse_entry(st);

    switch (st->op) {
    case OP_NONE: st->accumulator = rhs;                            break;
    case OP_ADD:  st->accumulator = st->accumulator + rhs;          break;
    case OP_SUB:  st->accumulator = st->accumulator - rhs;          break;
    case OP_MUL:  st->accumulator = fp_mul(st->accumulator, rhs);   break;
    case OP_DIV:
        if (rhs != 0) st->accumulator = fp_div(st->accumulator, rhs);
        else          st->accumulator = 0;
        break;
    case OP_POW:  st->accumulator = fp_pow(st->accumulator, rhs);   break;
    default: break;
    }

    st->entry[0]      = '\0';
    st->entry_has_dot = 0;
}

/* =========================================================================
 * Degrees <-> radians conversion.
 * ========================================================================= */
static fp_t to_radians(fp_t deg, int deg_mode)
{
    if (!deg_mode) return deg;
    /* rad = deg * pi / 180 */
    return fp_div(fp_mul(deg, FP_PI), fp_from_int(180));
}

/* =========================================================================
 * Button user-data structs (static storage so they outlive _start).
 * ========================================================================= */

typedef struct { sc_state_t *st; char digit; } ud_digit_t;
typedef struct { sc_state_t *st; op_t  op;   } ud_op_t;

/* Unary scientific function ID. */
typedef enum {
    FN_SIN, FN_COS, FN_TAN,
    FN_SQRT, FN_SQ, FN_INV,
    FN_LN, FN_LOG10, FN_EXP,
    FN_PI, FN_E_CONST,
    FN_FACT,
    FN_SIGN, FN_PCT,
    FN_POW,   /* x^y -- binary, sets up awaiting_pow */
    FN_MEM_PLUS, FN_MEM_MINUS, FN_MEM_RECALL, FN_MEM_CLR,
    FN_CE,
} fn_id_t;

typedef struct { sc_state_t *st; fn_id_t fn; } ud_fn_t;

/* =========================================================================
 * Static storage pools.
 * ========================================================================= */
static ud_digit_t g_ud_digits[12];  /* 0..9, '.', (spare) */
static ud_op_t    g_ud_ops[5];      /* +,-,*,/,= */
static ud_fn_t    g_ud_fns[32];
static int        g_fn_idx;  /* allocation counter for g_ud_fns */

static ud_fn_t *alloc_fn(sc_state_t *st, fn_id_t fn)
{
    ud_fn_t *p = &g_ud_fns[g_fn_idx++];
    p->st = st;
    p->fn = fn;
    return p;
}

/* =========================================================================
 * Button callbacks.
 * ========================================================================= */

/* Digit / decimal point. */
static void on_digit(void *ud)
{
    ud_digit_t   *d  = (ud_digit_t *)ud;
    sc_state_t   *st = d->st;

    if (st->fresh_result) {
        st->entry[0]      = '\0';
        st->entry_has_dot = 0;
        st->fresh_result  = 0;
    }

    if (d->digit == '.') {
        if (st->entry_has_dot) return;  /* only one dot */
        if (st->entry[0] == '\0') {
            st->entry[0] = '0';
            st->entry[1] = '\0';
        }
        st->entry_has_dot = 1;
    }

    unsigned long len = k_strlen(st->entry);
    if (len < ENTRY_MAX) {
        st->entry[len]     = d->digit;
        st->entry[len + 1] = '\0';
    }

    sc_update_display(st);
}

/* Binary operator. */
static void on_op(void *ud)
{
    ud_op_t    *o  = (ud_op_t *)ud;
    sc_state_t *st = o->st;

    /* If there is a live entry, evaluate pending first. */
    if (st->entry[0] != '\0') {
        sc_apply_op(st);
    }

    /* Update expression string. */
    char rbuf[DISP_MAX];
    fp_to_str(st->accumulator, rbuf, DISP_MAX);
    k_strcpy(st->expr_str, rbuf);
    switch (o->op) {
    case OP_ADD: k_strcat_n(st->expr_str, sizeof(st->expr_str), " + "); break;
    case OP_SUB: k_strcat_n(st->expr_str, sizeof(st->expr_str), " - "); break;
    case OP_MUL: k_strcat_n(st->expr_str, sizeof(st->expr_str), " x "); break;
    case OP_DIV: k_strcat_n(st->expr_str, sizeof(st->expr_str), " / "); break;
    default: break;
    }

    st->op           = o->op;
    st->fresh_result = 0;

    sc_update_display(st);
}

/* Equals. */
static void on_equals(void *ud)
{
    sc_state_t *st = (sc_state_t *)ud;

    if (st->entry[0] != '\0') {
        sc_apply_op(st);
    }

    /* Show full expression before clearing. */
    char rbuf[DISP_MAX];
    fp_to_str(st->accumulator, rbuf, DISP_MAX);
    k_strcpy(st->expr_str, "= ");
    k_strcat_n(st->expr_str, sizeof(st->expr_str), rbuf);

    st->op           = OP_NONE;
    st->fresh_result = 1;

    sc_update_display(st);

    serial_print("[SCICALC] = ");
    serial_print(rbuf);
    serial_print("\n");
}

/* Clear (all state). */
static void on_clear(void *ud)
{
    sc_state_t *st = (sc_state_t *)ud;
    st->entry[0]      = '\0';
    st->entry_has_dot = 0;
    st->accumulator   = 0;
    st->op            = OP_NONE;
    st->fresh_result  = 0;
    st->awaiting_pow  = 0;
    st->expr_str[0]   = '\0';
    ui_label_set_text(st->disp_result, "0");
    ui_label_set_text(st->disp_expr,   "");
}

/* CE -- clear entry only. */
static void on_ce(void *ud)
{
    sc_state_t *st = (sc_state_t *)ud;
    st->entry[0]      = '\0';
    st->entry_has_dot = 0;
    st->fresh_result  = 0;
    sc_update_display(st);
}

/* DEG/RAD toggle. */
static void on_degrad(void *ud)
{
    sc_state_t *st = (sc_state_t *)ud;
    st->deg_mode = !st->deg_mode;
    ui_label_set_text(st->btn_degrad, st->deg_mode ? "DEG" : "RAD");
}

/* Scientific / memory function. */
static void on_fn(void *ud)
{
    ud_fn_t    *f  = (ud_fn_t *)ud;
    sc_state_t *st = f->st;

    fp_t val = sc_parse_entry(st);
    fp_t res = 0;
    int  applied = 1;

    switch (f->fn) {
    case FN_SIN: {
        fp_t r = to_radians(val, st->deg_mode);
        res = fp_sin(r);
        k_strcpy(st->expr_str, "sin("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    }
    case FN_COS: {
        fp_t r = to_radians(val, st->deg_mode);
        res = fp_cos(r);
        k_strcpy(st->expr_str, "cos("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    }
    case FN_TAN: {
        fp_t r = to_radians(val, st->deg_mode);
        res = fp_tan(r);
        k_strcpy(st->expr_str, "tan("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    }
    case FN_SQRT:
        res = fp_sqrt(val);
        k_strcpy(st->expr_str, "sqrt("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    case FN_SQ:
        res = fp_mul(val, val);
        k_strcpy(st->expr_str, "("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")^2");
        }
        break;
    case FN_INV:
        res = (val != 0) ? fp_div(FP_ONE, val) : 0;
        k_strcpy(st->expr_str, "1/("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    case FN_LN:
        res = fp_ln(val);
        k_strcpy(st->expr_str, "ln("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    case FN_LOG10:
        res = fp_log10(val);
        k_strcpy(st->expr_str, "log("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    case FN_EXP:
        res = fp_exp(val);
        k_strcpy(st->expr_str, "e^("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")");
        }
        break;
    case FN_PI:
        res = FP_PI;
        k_strcpy(st->expr_str, "pi");
        break;
    case FN_E_CONST:
        res = FP_E;
        k_strcpy(st->expr_str, "e");
        break;
    case FN_FACT: {
        long long n = (long long)(val / FP_SCALE);
        res = fp_factorial(n);
        k_strcpy(st->expr_str, "("); {
            char tb[DISP_MAX]; fp_to_str(val, tb, DISP_MAX);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), tb);
            k_strcat_n(st->expr_str, sizeof(st->expr_str), ")!");
        }
        break;
    }
    case FN_SIGN:
        res = -val;
        {
            char tb[DISP_MAX]; fp_to_str(res, tb, DISP_MAX);
            k_strcpy(st->expr_str, tb);
        }
        break;
    case FN_PCT:
        /* percent: val / 100 */
        res = fp_div(val, fp_from_int(100));
        {
            char tb[DISP_MAX]; fp_to_str(res, tb, DISP_MAX);
            k_strcpy(st->expr_str, tb); k_strcat_n(st->expr_str, sizeof(st->expr_str), "%");
        }
        break;
    case FN_POW:
        /*
         * x^y is a binary op: commit current value as lhs, set op, wait
         * for rhs.
         */
        if (st->entry[0] != '\0') {
            sc_apply_op(st);
        }
        st->op           = OP_POW;
        st->fresh_result = 0;
        {
            char tb[DISP_MAX]; fp_to_str(st->accumulator, tb, DISP_MAX);
            k_strcpy(st->expr_str, tb); k_strcat_n(st->expr_str, sizeof(st->expr_str), " ^ ");
        }
        sc_update_display(st);
        return;   /* do NOT call sc_set_result */
    case FN_MEM_PLUS:
        st->memory += sc_parse_entry(st);
        applied = 0;
        break;
    case FN_MEM_MINUS:
        st->memory -= sc_parse_entry(st);
        applied = 0;
        break;
    case FN_MEM_RECALL:
        res = st->memory;
        k_strcpy(st->expr_str, "MR");
        break;
    case FN_MEM_CLR:
        st->memory = 0;
        applied = 0;
        break;
    case FN_CE:
        on_ce(st);
        return;
    default:
        applied = 0;
        break;
    }

    if (applied) {
        sc_set_result(st, res);
    } else {
        sc_update_display(st);
    }
}

/* =========================================================================
 * Layout constants.
 *
 * Window: 400 x 560.
 *
 * Display area:
 *   Expression label:  y=8,  h=22, text color 0xFFAAAAAA
 *   Result label:      y=34, h=40, text color 0xFFFFFFFF
 *
 * Scientific grid (5 cols x 5 rows):
 *   starts at y=88, each button 72w x 42h, gap=4.
 *
 * Basic grid (5 cols x 4 rows below scientific):
 *   starts at y=318, each button 72w x 42h, gap=4.
 *   Actually 4 cols for main + 1 col for ops on right.
 * ========================================================================= */

#define WIN_W    400
#define WIN_H    560

/* Scientific grid */
#define SCI_COLS   5
#define SCI_ROWS   5
#define SCI_BW     72
#define SCI_BH     42
#define SCI_GAP     4
#define SCI_X0      8
#define SCI_Y0     90

/* Basic grid: 4 cols for digits/CE/C, 1 col for operators */
#define BSC_COLS   5
#define BSC_BW     72
#define BSC_BH     50
#define BSC_GAP     4
#define BSC_X0      8
#define BSC_Y0    318

#define SCI_COL(c) (SCI_X0 + (c) * (SCI_BW + SCI_GAP))
#define SCI_ROW(r) (SCI_Y0 + (r) * (SCI_BH + SCI_GAP))
#define BSC_COL(c) (BSC_X0 + (c) * (BSC_BW + BSC_GAP))
#define BSC_ROW(r) (BSC_Y0 + (r) * (BSC_BH + BSC_GAP))

/* =========================================================================
 * Color palette (dark theme).
 * ========================================================================= */
#define COL_BG         0xFF1C1C1E   /* window background */
#define COL_DISP_BG    0xFF2C2C2E   /* display panel */
#define COL_BTN_SCI    0xFF3A3A3C   /* scientific buttons */
#define COL_BTN_NUM    0xFF48484A   /* digit buttons */
#define COL_BTN_OP     0xFFFF9F0A   /* operator buttons (amber) */
#define COL_BTN_EQ     0xFFFF9F0A   /* equals same as op */
#define COL_BTN_CLR    0xFF636366   /* clear / CE */
#define COL_BTN_MEM    0xFF30506A   /* memory buttons */
#define COL_BTN_DEGR   0xFF2C5A3E   /* DEG/RAD toggle (green) */
#define COL_TXT_WHITE  0xFFFFFFFF
#define COL_TXT_GRAY   0xFFAAAAAA

/* =========================================================================
 * Entry point.
 * ========================================================================= */

void _start(void)
{
    serial_print("[SCICALC] starting\n");

    /* ---- Initialise state ---- */
    sc_state_t *st = &g_sc;
    st->entry[0]      = '\0';
    st->entry_has_dot = 0;
    st->op            = OP_NONE;
    st->accumulator   = 0;
    st->memory        = 0;
    st->deg_mode      = 1;   /* default: degrees */
    st->fresh_result  = 0;
    st->awaiting_pow  = 0;
    st->expr_str[0]   = '\0';
    st->disp_expr     = 0;
    st->disp_result   = 0;
    st->btn_degrad    = 0;
    g_fn_idx          = 0;

    /* ---- Create window ---- */
    ui_app_t    *app  = ui_app_create("Scientific Calculator", WIN_W, WIN_H);
    ui_widget_t *root = ui_app_root(app);

    /* ---- Display panel ---- */
    ui_widget_t *dp = ui_panel(root, 4, 6, WIN_W - 8, 78, COL_DISP_BG);
    /* Expression row (small, top) */
    st->disp_expr   = ui_label(dp, 8, 6,  "",  COL_TXT_GRAY);
    /* Result row (large, bottom) */
    st->disp_result = ui_label(dp, 8, 32, "0", COL_TXT_WHITE);

    /* ---- Set up digit ud structs (0-9) ---- */
    for (int i = 0; i <= 9; i++) {
        g_ud_digits[i].st    = st;
        g_ud_digits[i].digit = (char)('0' + i);
    }
    /* Decimal point at index 10 */
    g_ud_digits[10].st    = st;
    g_ud_digits[10].digit = '.';

    /* ---- Set up operator ud structs ---- */
    /* 0=+, 1=-, 2=*, 3=/ */
    const op_t ops[4] = { OP_ADD, OP_SUB, OP_MUL, OP_DIV };
    for (int i = 0; i < 4; i++) {
        g_ud_ops[i].st = st;
        g_ud_ops[i].op = ops[i];
    }

    /* ==================================================================
     * SCIENTIFIC GRID (5 cols x 5 rows).
     *
     * Row 0: MC  MR  M-  M+  DEG/RAD
     * Row 1: sin cos tan x^y pi
     * Row 2: sqrt x^2 1/x ln log
     * Row 3: exp e  n!  %   +/-
     * Row 4: CE  C   (spacer)  (spacer)  (spacer)  -- merged with basic
     * ================================================================== */

    /* Row 0: memory + deg/rad */
    ui_button(root, SCI_COL(0), SCI_ROW(0), SCI_BW, SCI_BH, "MC",
              on_fn, alloc_fn(st, FN_MEM_CLR));
    ui_button(root, SCI_COL(1), SCI_ROW(0), SCI_BW, SCI_BH, "MR",
              on_fn, alloc_fn(st, FN_MEM_RECALL));
    ui_button(root, SCI_COL(2), SCI_ROW(0), SCI_BW, SCI_BH, "M-",
              on_fn, alloc_fn(st, FN_MEM_MINUS));
    ui_button(root, SCI_COL(3), SCI_ROW(0), SCI_BW, SCI_BH, "M+",
              on_fn, alloc_fn(st, FN_MEM_PLUS));
    st->btn_degrad =
    ui_button(root, SCI_COL(4), SCI_ROW(0), SCI_BW, SCI_BH, "DEG",
              on_degrad, (void *)st);

    /* Row 1: trig */
    ui_button(root, SCI_COL(0), SCI_ROW(1), SCI_BW, SCI_BH, "sin",
              on_fn, alloc_fn(st, FN_SIN));
    ui_button(root, SCI_COL(1), SCI_ROW(1), SCI_BW, SCI_BH, "cos",
              on_fn, alloc_fn(st, FN_COS));
    ui_button(root, SCI_COL(2), SCI_ROW(1), SCI_BW, SCI_BH, "tan",
              on_fn, alloc_fn(st, FN_TAN));
    ui_button(root, SCI_COL(3), SCI_ROW(1), SCI_BW, SCI_BH, "x^y",
              on_fn, alloc_fn(st, FN_POW));
    ui_button(root, SCI_COL(4), SCI_ROW(1), SCI_BW, SCI_BH, "pi",
              on_fn, alloc_fn(st, FN_PI));

    /* Row 2: power/log */
    ui_button(root, SCI_COL(0), SCI_ROW(2), SCI_BW, SCI_BH, "sqrt",
              on_fn, alloc_fn(st, FN_SQRT));
    ui_button(root, SCI_COL(1), SCI_ROW(2), SCI_BW, SCI_BH, "x^2",
              on_fn, alloc_fn(st, FN_SQ));
    ui_button(root, SCI_COL(2), SCI_ROW(2), SCI_BW, SCI_BH, "1/x",
              on_fn, alloc_fn(st, FN_INV));
    ui_button(root, SCI_COL(3), SCI_ROW(2), SCI_BW, SCI_BH, "ln",
              on_fn, alloc_fn(st, FN_LN));
    ui_button(root, SCI_COL(4), SCI_ROW(2), SCI_BW, SCI_BH, "log",
              on_fn, alloc_fn(st, FN_LOG10));

    /* Row 3: exp / e / n! / % / +/- */
    ui_button(root, SCI_COL(0), SCI_ROW(3), SCI_BW, SCI_BH, "exp",
              on_fn, alloc_fn(st, FN_EXP));
    ui_button(root, SCI_COL(1), SCI_ROW(3), SCI_BW, SCI_BH, "e",
              on_fn, alloc_fn(st, FN_E_CONST));
    ui_button(root, SCI_COL(2), SCI_ROW(3), SCI_BW, SCI_BH, "n!",
              on_fn, alloc_fn(st, FN_FACT));
    ui_button(root, SCI_COL(3), SCI_ROW(3), SCI_BW, SCI_BH, "%",
              on_fn, alloc_fn(st, FN_PCT));
    ui_button(root, SCI_COL(4), SCI_ROW(3), SCI_BW, SCI_BH, "+/-",
              on_fn, alloc_fn(st, FN_SIGN));

    /* Row 4: CE / C (span rest of scientific area) */
    ui_button(root, SCI_COL(0), SCI_ROW(4), SCI_BW, SCI_BH, "CE",
              on_ce, (void *)st);
    ui_button(root, SCI_COL(1), SCI_ROW(4), SCI_BW, SCI_BH, "C",
              on_clear, (void *)st);
    /* Columns 2-4 of row 4 are left empty (acts as visual spacer). */

    /* ==================================================================
     * BASIC GRID (5 cols x 4 rows).
     *
     * Right column (col 4) = operator buttons.
     *
     * Row 0: 7  8  9   /   (col 4 = /)
     * Row 1: 4  5  6   *
     * Row 2: 1  2  3   -
     * Row 3: 0  .  =   +
     * ================================================================== */

    /* Row 0: 7 8 9 / */
    ui_button(root, BSC_COL(0), BSC_ROW(0), BSC_BW, BSC_BH, "7",
              on_digit, &g_ud_digits[7]);
    ui_button(root, BSC_COL(1), BSC_ROW(0), BSC_BW, BSC_BH, "8",
              on_digit, &g_ud_digits[8]);
    ui_button(root, BSC_COL(2), BSC_ROW(0), BSC_BW, BSC_BH, "9",
              on_digit, &g_ud_digits[9]);
    ui_button(root, BSC_COL(3), BSC_ROW(0), BSC_BW, BSC_BH, "/",
              on_op, &g_ud_ops[3]);

    /* Row 1: 4 5 6 * */
    ui_button(root, BSC_COL(0), BSC_ROW(1), BSC_BW, BSC_BH, "4",
              on_digit, &g_ud_digits[4]);
    ui_button(root, BSC_COL(1), BSC_ROW(1), BSC_BW, BSC_BH, "5",
              on_digit, &g_ud_digits[5]);
    ui_button(root, BSC_COL(2), BSC_ROW(1), BSC_BW, BSC_BH, "6",
              on_digit, &g_ud_digits[6]);
    ui_button(root, BSC_COL(3), BSC_ROW(1), BSC_BW, BSC_BH, "*",
              on_op, &g_ud_ops[2]);

    /* Row 2: 1 2 3 - */
    ui_button(root, BSC_COL(0), BSC_ROW(2), BSC_BW, BSC_BH, "1",
              on_digit, &g_ud_digits[1]);
    ui_button(root, BSC_COL(1), BSC_ROW(2), BSC_BW, BSC_BH, "2",
              on_digit, &g_ud_digits[2]);
    ui_button(root, BSC_COL(2), BSC_ROW(2), BSC_BW, BSC_BH, "3",
              on_digit, &g_ud_digits[3]);
    ui_button(root, BSC_COL(3), BSC_ROW(2), BSC_BW, BSC_BH, "-",
              on_op, &g_ud_ops[1]);

    /* Row 3: 0 . = + */
    ui_button(root, BSC_COL(0), BSC_ROW(3), BSC_BW, BSC_BH, "0",
              on_digit, &g_ud_digits[0]);
    ui_button(root, BSC_COL(1), BSC_ROW(3), BSC_BW, BSC_BH, ".",
              on_digit, &g_ud_digits[10]);
    ui_button(root, BSC_COL(2), BSC_ROW(3), BSC_BW, BSC_BH, "=",
              on_equals, (void *)st);
    ui_button(root, BSC_COL(3), BSC_ROW(3), BSC_BW, BSC_BH, "+",
              on_op, &g_ud_ops[0]);

    /* ---- Enter the event loop (never returns). ---- */
    ui_app_run(app);
}

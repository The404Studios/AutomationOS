/*
 * userspace/libc/math.c  —  Freestanding math shim
 *
 * Implements the functions declared in math.h using only integer ops,
 * basic FP arithmetic, and Taylor/minimax polynomials.  No libc, no
 * libm dependency.  Accuracy is suitable for font rasterization
 * (stb_truetype) and cubic/elastic animation easing — not IEEE-perfect.
 *
 * Strategy:
 *   sqrt   – Newton-Raphson (6 iterations → ~48 bits accuracy)
 *   exp    – range-reduce mod ln2, then a degree-6 minimax polynomial
 *   log    – argument reduction + a degree-5 polynomial on [1,sqrt(2)]
 *   sin/cos – range-reduce to [-pi/4, pi/4] then degree-13 Taylor
 *   tan    – sin/cos
 *   asin/acos – series near 0, identity near ±1
 *   atan   – range-reduce + degree-13 series
 *   pow    – exp(y * log(x))
 *   floor/ceil/fmod/fabs/round/trunc/copysign/fmin/fmax – direct
 *
 * Float variants simply cast, call the double version, and cast back.
 */

#include "math.h"

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

/* Reinterpret a double as a uint64 without UB (type-punning via union) */
typedef unsigned long long u64;
typedef unsigned int       u32;

static inline u64 double_to_bits(double x) {
    union { double d; u64 u; } v;
    v.d = x;
    return v.u;
}

static inline double bits_to_double(u64 b) {
    union { double d; u64 u; } v;
    v.u = b;
    return v.d;
}

static inline u32 float_to_bits(float x) {
    union { float f; u32 u; } v;
    v.f = x;
    return v.u;
}

static inline float bits_to_float(u32 b) {
    union { float f; u32 u; } v;
    v.u = b;
    return v.f;
}

/* =========================================================================
 * fabs / fabsf
 * ====================================================================== */

double fabs(double x) {
    return bits_to_double(double_to_bits(x) & ~(u64)0x8000000000000000ULL);
}

float fabsf(float x) {
    return bits_to_float(float_to_bits(x) & ~(u32)0x80000000U);
}

/* =========================================================================
 * copysign / copysignf
 * ====================================================================== */

double copysign(double x, double y) {
    u64 bx = double_to_bits(x) & ~(u64)0x8000000000000000ULL;
    u64 by = double_to_bits(y) &  (u64)0x8000000000000000ULL;
    return bits_to_double(bx | by);
}

float copysignf(float x, float y) {
    u32 bx = float_to_bits(x) & ~(u32)0x80000000U;
    u32 by = float_to_bits(y) &  (u32)0x80000000U;
    return bits_to_float(bx | by);
}

/* =========================================================================
 * floor / ceil / trunc / round
 * ====================================================================== */

double floor(double x) {
    /* Extract the integer part via truncation */
    long long n = (long long)x;
    double d = (double)n;
    /* If x was negative and fractional, subtract 1 */
    return (x < d) ? d - 1.0 : d;
}

double ceil(double x) {
    long long n = (long long)x;
    double d = (double)n;
    return (x > d) ? d + 1.0 : d;
}

double trunc(double x) {
    return (double)(long long)x;
}

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

float floorf(float x)  { return (float)floor((double)x); }
float ceilf(float x)   { return (float)ceil((double)x); }
float truncf(float x)  { return (float)trunc((double)x); }
float roundf(float x)  { return (float)round((double)x); }

/* =========================================================================
 * fmod / fmodf
 * ====================================================================== */

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;  /* undefined; return 0 */
    long long n = (long long)(x / y);
    return x - (double)n * y;
}

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

/* =========================================================================
 * fmin / fmax / fminf / fmaxf
 * ====================================================================== */

double fmin(double x, double y) { return (x < y) ? x : y; }
double fmax(double x, double y) { return (x > y) ? x : y; }
float  fminf(float x, float y)  { return (x < y) ? x : y; }
float  fmaxf(float x, float y)  { return (x > y) ? x : y; }

/* =========================================================================
 * ldexp / frexp / modf
 * ====================================================================== */

double ldexp(double x, int e) {
    /* Adjust biased exponent field directly */
    u64 bits = double_to_bits(x);
    int exp_field = (int)((bits >> 52) & 0x7FF);
    if (exp_field == 0 || exp_field == 0x7FF) return x; /* 0 / inf / nan */
    exp_field += e;
    if (exp_field <= 0)   return copysign(0.0, x);
    if (exp_field >= 0x7FF) return copysign((double)((u64)0x7FF0000000000000ULL), x);
    bits = (bits & ~(u64)0x7FF0000000000000ULL) | ((u64)exp_field << 52);
    return bits_to_double(bits);
}

double frexp(double x, int *ep) {
    if (x == 0.0) { *ep = 0; return 0.0; }
    u64 bits = double_to_bits(x);
    int e = (int)((bits >> 52) & 0x7FF) - 1022;
    /* Set exponent to 1022 (mantissa in [0.5, 1)) */
    bits = (bits & ~(u64)0x7FF0000000000000ULL) | ((u64)1022 << 52);
    *ep = e;
    return bits_to_double(bits);
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    *iptr = i;
    return x - i;
}

/* =========================================================================
 * sqrt — Newton-Raphson
 *
 * Initial guess from bit-manipulation of the IEEE exponent.
 * 6 iterations give full double precision for positive inputs.
 * ====================================================================== */

double sqrt(double x) {
    if (x < 0.0) return -1.0; /* NaN substitute — graphics code won't hit this */
    if (x == 0.0) return 0.0;

    /* Initial estimate: halve the exponent */
    u64 bits = double_to_bits(x);
    u64 guess_bits = ((bits >> 1) + (u64)0x1FF8000000000000ULL);
    double g = bits_to_double(guess_bits);

    /* 6 Newton-Raphson iterations: g = (g + x/g) / 2 */
    g = (g + x / g) * 0.5;
    g = (g + x / g) * 0.5;
    g = (g + x / g) * 0.5;
    g = (g + x / g) * 0.5;
    g = (g + x / g) * 0.5;
    g = (g + x / g) * 0.5;
    return g;
}

float sqrtf(float x) { return (float)sqrt((double)x); }

/* =========================================================================
 * exp — e^x
 *
 * Range-reduce: x = n*ln2 + r,  |r| <= ln2/2
 * Then exp(x) = 2^n * exp(r)
 * exp(r) computed with a degree-6 minimax polynomial on [-ln2/2, ln2/2].
 * ====================================================================== */

/* Polynomial coefficients for exp(r)-1 on [-0.347, 0.347] */
double exp(double x) {
    /* Clamp to avoid overflow in 2^n (double range ~±709) */
    if (x >  709.0) return 8.98846567431e+307; /* ~DBL_MAX */
    if (x < -745.0) return 0.0;

    /* n = round(x / ln2) */
    double ln2  = 6.93147180559945309e-1;
    double inv_ln2 = 1.44269504088896341;

    int n = (int)(x * inv_ln2 + 0.5);
    if (x < 0.0 && (x * inv_ln2 + 0.5) < 0.0) n--;  /* proper round for negatives */

    double r = x - (double)n * ln2;

    /* Horner's method for exp(r): coefficients from [0, 1/2!..1/6!] */
    double p = 1.0
        + r * (1.0
        + r * (5.0e-1
        + r * (1.66666666666666667e-1
        + r * (4.16666666666666667e-2
        + r * (8.33333333333333333e-3
        + r * 1.38888888888888889e-3)))));

    return ldexp(p, n);
}

float expf(float x) { return (float)exp((double)x); }

/* =========================================================================
 * log — natural logarithm
 *
 * Argument reduction: x = 2^e * m,  m in [1, 2)
 * Then log(x) = e*log(2) + log(m)
 * log(m): further reduce m to [1, sqrt(2)] so argument is in [-0.172, 0.172]:
 *   if m > sqrt(2), m /= 2, e++
 * Then f = (m-1)/(m+1), log(m) = 2*atanh(f) via degree-5 series.
 * ====================================================================== */

double log(double x) {
    if (x <= 0.0) return -8.98846567431e+307; /* -inf substitute */

    int e;
    double m = frexp(x, &e);   /* x = m * 2^e, m in [0.5, 1) */

    /* Shift m to [1, 2) */
    m *= 2.0;
    e -= 1;

    /* If m > sqrt(2), adjust */
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e += 1;
    }

    /* f = (m-1)/(m+1), series for 2*atanh(f) = log(m) */
    double f  = (m - 1.0) / (m + 1.0);
    double f2 = f * f;
    double series = 2.0 * f * (1.0
        + f2 * (1.0/3.0
        + f2 * (1.0/5.0
        + f2 * (1.0/7.0
        + f2 * (1.0/9.0)))));

    return series + (double)e * 6.93147180559945309e-1;
}

float logf(float x) { return (float)log((double)x); }

double log2(double x)  { return log(x) * 1.44269504088896341; }
double log10(double x) { return log(x) * 4.34294481903251828e-1; }

/* =========================================================================
 * pow — x^y = exp(y * log(x))
 *
 * Special cases handled to match typical usage in graphics code.
 * ====================================================================== */

double pow(double x, double y) {
    if (y == 0.0)  return 1.0;
    if (x == 1.0)  return 1.0;
    if (x == 0.0)  return (y > 0.0) ? 0.0 : 8.98846567431e+307;
    if (x < 0.0) {
        /* Only valid for integer exponents */
        long long n = (long long)y;
        if ((double)n != y) return 0.0; /* undefined for non-integer */
        double result = exp((double)n * log(-x));
        return (n & 1) ? -result : result;
    }
    return exp(y * log(x));
}

float powf(float x, float y) { return (float)pow((double)x, (double)y); }

/* =========================================================================
 * sin / cos — Taylor series after range reduction
 *
 * Reduce to [-pi/4, pi/4] using quadrant information.
 * Taylor for sin(x) = x - x^3/3! + x^5/5! - x^7/7! + x^9/9! - x^11/11! + x^13/13!
 * Taylor for cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + x^8/8! - x^10/10! + x^12/12!
 * Degree 13 gives < 1e-13 error on [-pi/4, pi/4].
 * ====================================================================== */

/* Core sin on [-pi/4, pi/4] */
static double sin_core(double x) {
    double x2 = x * x;
    return x * (1.0
        + x2 * (-1.66666666666666667e-1
        + x2 * ( 8.33333333333333333e-3
        + x2 * (-1.98412698412698413e-4
        + x2 * ( 2.75573192239858907e-6
        + x2 * (-2.50521083854417188e-8
        + x2 *   1.60590438368216145e-10))))));
}

/* Core cos on [-pi/4, pi/4] */
static double cos_core(double x) {
    double x2 = x * x;
    return 1.0
        + x2 * (-5.0e-1
        + x2 * ( 4.16666666666666667e-2
        + x2 * (-1.38888888888888889e-3
        + x2 * ( 2.48015873015873016e-5
        + x2 * (-2.75573192239858907e-7
        + x2 *   2.08767569878680989e-9)))));
}

double sin(double x) {
    /* Reduce to [-pi, pi] */
    double pi2 = 2.0 * M_PI;
    if (x > M_PI || x < -M_PI) {
        double q = fmod(x, pi2);
        if (q >  M_PI) q -= pi2;
        if (q < -M_PI) q += pi2;
        x = q;
    }

    /* Reduce to [-pi/2, pi/2] */
    if (x >  M_PI_2) { x =  M_PI - x; }
    if (x < -M_PI_2) { x = -M_PI - x; }

    /* Reduce to [-pi/4, pi/4] */
    if (x >  M_PI_4) { return  cos_core(x - M_PI_2); }
    if (x < -M_PI_4) { return -cos_core(-x - M_PI_2); }
    return sin_core(x);
}

double cos(double x) {
    return sin(x + M_PI_2);
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return 8.98846567431e+307;
    return sin(x) / c;
}

float sinf(float x)  { return (float)sin((double)x); }
float cosf(float x)  { return (float)cos((double)x); }
float tanf(float x)  { return (float)tan((double)x); }

/* =========================================================================
 * atan — arctangent
 *
 * Reduce |x| to [0, 1] using atan(x) = pi/2 - atan(1/x) for |x|>1
 * then further to [0, tan(pi/12)] using atan(x)=atan(c)+atan((x-c)/(1+cx))
 * with c = tan(pi/6) = 1/sqrt(3).  Finally a degree-13 Taylor on a small
 * interval. Alternatively: simple degree-13 series after reduction to <=1.
 * ====================================================================== */

/* Degree-13 minimax for atan on [0, 1] */
static double atan_core(double x) {
    double x2 = x * x;
    return x * (1.0
        + x2 * (-3.33333333333333333e-1
        + x2 * ( 2.0e-1
        + x2 * (-1.42857142857142857e-1
        + x2 * ( 1.11111111111111111e-1
        + x2 * (-9.09090909090909091e-2
        + x2 *   7.69230769230769231e-2))))));
}

double atan(double x) {
    int neg = (x < 0.0);
    if (neg) x = -x;

    double result;
    if (x > 1.0) {
        result = M_PI_2 - atan_core(1.0 / x);
    } else {
        result = atan_core(x);
    }

    return neg ? -result : result;
}

double atan2(double y, double x) {
    if (x == 0.0) {
        if (y > 0.0) return  M_PI_2;
        if (y < 0.0) return -M_PI_2;
        return 0.0;
    }
    double r = atan(y / x);
    if (x < 0.0) {
        r += (y >= 0.0) ? M_PI : -M_PI;
    }
    return r;
}

float atanf(float x)              { return (float)atan((double)x); }
float atan2f(float y, float x)    { return (float)atan2((double)y, (double)x); }

/* =========================================================================
 * asin / acos
 *
 * asin(x) = atan(x / sqrt(1 - x^2))   for |x| < 1
 * acos(x) = pi/2 - asin(x)
 * For |x| close to 1 use the identity:
 *   asin(x) = pi/2 - 2*asin(sqrt((1-x)/2))  (better conditioned)
 * ====================================================================== */

double asin(double x) {
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;

    int neg = (x < 0.0);
    if (neg) x = -x;

    double result;
    if (x > 0.7) {
        /* Use identity near ±1 */
        double w = sqrt((1.0 - x) * 0.5);
        result = M_PI_2 - 2.0 * atan(w / sqrt(1.0 - w * w));
    } else {
        double s = sqrt(1.0 - x * x);
        result = atan(x / s);
    }

    return neg ? -result : result;
}

double acos(double x) {
    return M_PI_2 - asin(x);
}

float asinf(float x)  { return (float)asin((double)x); }
float acosf(float x)  { return (float)acos((double)x); }

/* =========================================================================
 * hypot
 * ====================================================================== */

double hypot(double x, double y) {
    return sqrt(x * x + y * y);
}

float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }

/* =========================================================================
 * isnan / isinf — bit-level classification
 * ====================================================================== */

int isnan(double x) {
    u64 b = double_to_bits(x);
    /* exponent all-ones AND mantissa non-zero => NaN */
    return ((b & (u64)0x7FF0000000000000ULL) == (u64)0x7FF0000000000000ULL) &&
           ((b & (u64)0x000FFFFFFFFFFFFFULL) != 0);
}

int isinf(double x) {
    u64 b = double_to_bits(x);
    /* exponent all-ones AND mantissa zero => infinity */
    return ((b & (u64)0x7FFFFFFFFFFFFFFFULL) == (u64)0x7FF0000000000000ULL);
}

/* =========================================================================
 * cbrt — cube root
 *
 * Use Newton-Raphson: g_{n+1} = (2*g_n + x/g_n^2) / 3
 * Initial guess via exponent manipulation: cbrt(2^e * m) ~ 2^(e/3) * cbrt(m)
 * 8 iterations converge to full precision.
 * ====================================================================== */

double cbrt(double x) {
    if (x == 0.0) return 0.0;
    int neg = (x < 0.0);
    if (neg) x = -x;

    /* Initial guess: halve the biased exponent by 3 */
    u64 bits  = double_to_bits(x);
    int  e    = (int)((bits >> 52) & 0x7FF) - 1023;
    int  ecbrt = e / 3;
    /* Adjust mantissa bits to a neutral exponent for fractional part */
    u64 mant_bits = (bits & (u64)0x000FFFFFFFFFFFFFULL) | ((u64)1023 << 52);
    double m = bits_to_double(mant_bits);   /* m in [1,2) */

    /* Good initial guess for cbrt(m): polynomial approximation on [1,2) */
    double g = 0.6299605249474366 + 0.3700394750525634 * m; /* linear approx */
    /* Apply the exponent correction */
    g = ldexp(g, ecbrt);
    /* Compensate for e not divisible by 3 */
    int erem = e - 3 * ecbrt;
    if      (erem == 1) g *= 1.2599210498948732;  /* cbrt(2) */
    else if (erem == 2) g *= 1.5874010519681994;  /* cbrt(4) */
    else if (erem ==-1) g *= 0.7937005259840998;  /* cbrt(0.5) */
    else if (erem ==-2) g *= 0.6299605249474366;  /* cbrt(0.25) */

    /* Newton iterations for g^3 = x: g = (2g + x/g^2) / 3 */
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;
    g = (2.0 * g + x / (g * g)) / 3.0;

    return neg ? -g : g;
}

float cbrtf(float x) { return (float)cbrt((double)x); }

/* =========================================================================
 * exp2 — 2^x
 *
 * exp2(x) = exp(x * ln2)
 * ====================================================================== */

double exp2(double x) {
    return exp(x * 6.93147180559945309e-1);
}

float exp2f(float x) { return (float)exp2((double)x); }

/* =========================================================================
 * sinh / cosh / tanh — hyperbolic functions
 *
 * sinh(x) = (e^x - e^{-x}) / 2
 * cosh(x) = (e^x + e^{-x}) / 2
 * tanh(x) = sinh(x)/cosh(x)
 *
 * For large |x| (>= 20), e^{-|x|} is negligible:
 *   sinh(x) ~  copysign(exp(|x|)/2, x)
 *   cosh(x) ~  exp(|x|)/2
 * For small |x| (< 1e-10): sinh(x) ~ x, tanh(x) ~ x
 * ====================================================================== */

double sinh(double x) {
    double ax = fabs(x);
    if (ax >= 20.0) {
        double e = exp(ax);
        return copysign(e * 0.5, x);
    }
    if (ax < 1e-10) return x;  /* linear region */
    double ep = exp(x);
    double em = exp(-x);
    return (ep - em) * 0.5;
}

double cosh(double x) {
    double ax = fabs(x);
    if (ax >= 20.0) {
        return exp(ax) * 0.5;
    }
    double ep = exp(x);
    double em = exp(-x);
    return (ep + em) * 0.5;
}

double tanh(double x) {
    double ax = fabs(x);
    if (ax >= 20.0) return copysign(1.0, x);
    if (ax < 1e-10) return x;
    double ep = exp(x);
    double em = exp(-x);
    return (ep - em) / (ep + em);
}

float sinhf(float x)  { return (float)sinh((double)x); }
float coshf(float x)  { return (float)cosh((double)x); }
float tanhf(float x)  { return (float)tanh((double)x); }

/* =========================================================================
 * math_selftest — basic sanity checks for all major functions
 *
 * Returns 0 on pass, a non-zero bitmask of failed checks on failure.
 * Each bit corresponds to one check (bit 0 = check 0, etc.).
 * ====================================================================== */

int math_selftest(void) {
    /* Tolerance for all comparisons */
    const double EPS = 1e-6;
    int failures = 0;

    /* Helper macro: set bit i if the absolute error exceeds EPS */
#define CHECK(i, got, expected) \
    do { \
        double _g = (double)(got); \
        double _e = (double)(expected); \
        double _d = _g - _e; \
        if (_d < 0.0) _d = -_d; \
        if (_d > EPS) failures |= (1 << (i)); \
    } while (0)

    CHECK( 0, sqrt(2.0),          1.41421356);
    CHECK( 1, pow(2.0, 10.0),     1024.0);
    CHECK( 2, exp(0.0),           1.0);
    CHECK( 3, exp(1.0),           2.71828182845904);
    CHECK( 4, log(M_E),           1.0);
    CHECK( 5, sin(0.0),           0.0);
    CHECK( 6, cos(0.0),           1.0);
    CHECK( 7, sin(M_PI_2),        1.0);
    CHECK( 8, floor(2.7),         2.0);
    CHECK( 9, ceil(2.1),          3.0);
    CHECK(10, fmod(10.0, 3.0),    1.0);
    CHECK(11, atan2(1.0, 1.0),    M_PI_4);
    CHECK(12, cbrt(27.0),         3.0);
    CHECK(13, exp2(8.0),          256.0);
    CHECK(14, sinh(0.0),          0.0);
    CHECK(15, cosh(0.0),          1.0);
    CHECK(16, tanh(0.0),          0.0);
    CHECK(17, log2(8.0),          3.0);
    CHECK(18, log10(1000.0),      3.0);
    CHECK(19, hypot(3.0, 4.0),    5.0);
    CHECK(20, fabs(-3.14),        3.14);
    CHECK(21, trunc(3.9),         3.0);
    CHECK(22, round(2.5),         3.0);

#undef CHECK

    return failures;
}

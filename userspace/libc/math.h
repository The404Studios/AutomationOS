#ifndef MATH_H
#define MATH_H

/* -------------------------------------------------------------------------
 * Freestanding math shim for the userspace C library.
 * Accuracy is suitable for font rasterization and animation easing;
 * not IEEE-754 bit-exact.
 * ---------------------------------------------------------------------- */

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_SQRT2     1.41421356237309504880
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390

#ifndef INFINITY
#define INFINITY    (__builtin_inff())
#endif

#ifndef NAN
#define NAN         (__builtin_nanf(""))
#endif

/* Double precision */
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sqrt(double x);
double cbrt(double x);
double pow(double x, double y);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double round(double x);
double trunc(double x);
double hypot(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
int    isnan(double x);
int    isinf(double x);

/* Float (single-precision) variants */
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float sqrtf(float x);
float cbrtf(float x);
float powf(float x, float y);
float expf(float x);
float exp2f(float x);
float logf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float roundf(float x);
float truncf(float x);
float hypotf(float x, float y);
float copysignf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);

/* Self-test: returns 0 on pass, non-zero bitmask of failed checks */
int math_selftest(void);

#endif /* MATH_H */

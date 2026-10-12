#ifndef _MATH_H
#define _MATH_H

#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define M_E 2.71828182845904523536
#define M_LOG2E 1.44269504088896340736
#define M_LOG10E 0.434294481903251827651
#define M_LN2 0.693147180559945309417
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.785398163397448309616
#define M_1_PI 0.318309886183790671538
#define M_2_PI 0.636619772367581343076
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.707106781186547524401

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
int __fpclassify(double x);
#define fpclassify(x) __fpclassify((double)(x))
#define isnan(x) ((x) != (x))
#define isinf(x) (fpclassify(x) == FP_INFINITE)
#define isfinite(x) (fpclassify(x) != FP_INFINITE && fpclassify(x) != FP_NAN)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x) __signbit((double)(x))
int __signbit(double x);

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long lround(double x);
long lrint(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double modf(double x, double *ip);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int exp);
long double ldexpl(long double x, int exp);
long double frexpl(long double x, int *exp);
double sqrt(double x);
long double sqrtl(long double x);
double cbrt(double x);
double hypot(double x, double y);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
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
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double fdim(double x, double y);
double fma(double x, double y, double z);

float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float sinf(float x);
float cosf(float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);

#endif

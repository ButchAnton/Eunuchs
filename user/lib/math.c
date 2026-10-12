/*
 * math: the C math library.
 *
 * The core functions reduce their argument and sum a series in 128-bit
 * long double (soft float), then round to double: slow-ish, but accurate
 * to the last bit or two, and short enough to read.
 */
#include <math.h>
#include <stdint.h>

typedef long double ld;

static const ld LN2 = 0.693147180559945309417232121458176568L;
static const ld PI = 3.14159265358979323846264338327950288L;
static const ld PI_2 = 1.57079632679489661923132169163975144L;

union dbits {
    double d;
    uint64_t u;
};

/* ---- classification and bits ---- */

int __fpclassify(double x)
{
    union dbits b = { x };
    int e = (int)((b.u >> 52) & 0x7ff);
    uint64_t m = b.u & ((1ULL << 52) - 1);
    if (e == 0x7ff)
        return m ? FP_NAN : FP_INFINITE;
    if (e == 0)
        return m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __signbit(double x)
{
    union dbits b = { x };
    return (int)(b.u >> 63);
}

double fabs(double x)
{
    union dbits b = { x };
    b.u &= ~(1ULL << 63);
    return b.d;
}

double copysign(double x, double y)
{
    union dbits a = { x }, b = { y };
    a.u = (a.u & ~(1ULL << 63)) | (b.u & (1ULL << 63));
    return a.d;
}

double frexp(double x, int *exp)
{
    union dbits b = { x };
    int e = (int)((b.u >> 52) & 0x7ff);
    if (e == 0x7ff || x == 0) {
        *exp = 0;
        return x;
    }
    if (e == 0) {                       /* subnormal: normalize first */
        x *= 18014398509481984.0;       /* 2^54 */
        b.d = x;
        e = (int)((b.u >> 52) & 0x7ff) - 54;
    }
    *exp = e - 1022;
    b.u = (b.u & ~(0x7ffULL << 52)) | (1022ULL << 52);
    return b.d;
}

double ldexp(double x, int exp)
{
    if (x == 0 || x != x || __fpclassify(x) == FP_INFINITE)
        return x;
    while (exp > 1000) {
        x *= 0x1p1000;
        exp -= 1000;
        if (__fpclassify(x) == FP_INFINITE)
            return x;
    }
    while (exp < -1000) {
        x *= 0x1p-1000;
        exp += 1000;
        if (x == 0)
            return x;
    }
    union dbits b;
    if (exp >= -1022) {
        b.u = (uint64_t)(exp + 1023) << 52;
        return x * b.d;
    }
    b.u = (uint64_t)(exp + 1022 + 1023) << 52;
    return x * b.d * 0x1p-1022;
}

double scalbn(double x, int exp) { return ldexp(x, exp); }

long double ldexpl(long double x, int exp)
{
    if (x == 0 || x != x)
        return x;
    while (exp >= 64) { x *= 18446744073709551616.0L; exp -= 64; }
    while (exp <= -64) { x /= 18446744073709551616.0L; exp += 64; }
    while (exp > 0) { x *= 2; exp--; }
    while (exp < 0) { x /= 2; exp++; }
    return x;
}

long double frexpl(long double x, int *exp)
{
    *exp = 0;
    if (x == 0 || x != x)
        return x;
    long double a = x < 0 ? -x : x;
    while (a >= 1) { a /= 2; (*exp)++; }
    while (a < 0.5L) { a *= 2; (*exp)--; }
    return x < 0 ? -a : a;
}

/* ---- rounding ---- */

double trunc(double x)
{
    if (fabs(x) >= 0x1p52 || x != x)
        return x;
    return copysign((double)(int64_t)x, x);
}

double floor(double x)
{
    double t = trunc(x);
    return t > x ? t - 1 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return t < x ? t + 1 : t;
}

double round(double x)
{
    if (fabs(x) >= 0x1p52 || x != x)
        return x;
    /* halfway cases away from zero; x - t is exact, unlike x + 0.5, which
       rounds 0.49999999999999994 up to 1 */
    double t = trunc(x);
    if (fabs(x - t) >= 0.5)
        t += copysign(1.0, x);
    return t;
}

double rint(double x)
{
    /* round half to even (the default rounding mode) */
    if (fabs(x) >= 0x1p52 || x != x)
        return x;
    double f = floor(x), d = x - f;
    if (d > 0.5 || (d == 0.5 && fmod(f, 2) != 0))
        f += 1;
    return copysign(f, x);
}

double nearbyint(double x) { return rint(x); }
long lround(double x) { return (long)round(x); }
long lrint(double x) { return (long)rint(x); }

double modf(double x, double *ip)
{
    double t = trunc(x);
    *ip = t;
    if (__fpclassify(x) == FP_INFINITE)
        return copysign(0, x);
    return copysign(x - t, x);
}

double fmod(double x, double y)
{
    if (y == 0 || x != x || y != y || __fpclassify(x) == FP_INFINITE)
        return (x * y) / (x * y);
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay)
        return x;
    /* exact: subtract the largest power-of-two multiple of y that fits */
    int ex, ey;
    frexp(ax, &ex);
    frexp(ay, &ey);
    for (int k = ex - ey; k >= 0; k--) {
        double t = ldexp(ay, k);
        if (t <= ax)
            ax -= t;
    }
    return copysign(ax, x);
}

double remainder(double x, double y)
{
    double r = fmod(x, y), ay = fabs(y);
    if (fabs(r) * 2 > ay || (fabs(r) * 2 == ay && fmod(trunc(x / y), 2) != 0))
        r -= copysign(ay, r);
    return r;
}

double fmin(double x, double y) { return x != x ? y : y != y ? x : x < y ? x : y; }
double fmax(double x, double y) { return x != x ? y : y != y ? x : x > y ? x : y; }
double fdim(double x, double y) { return x > y ? x - y : 0; }
double fma(double x, double y, double z) { return (double)((ld)x * y + z); }

/* ---- roots ---- */

double sqrt(double x)
{
    return __builtin_sqrt(x);           /* fsqrt.d */
}

long double sqrtl(long double x)
{
    if (x <= 0 || x != x || x == __builtin_infl())
        return x == 0 || x == __builtin_infl() ? x : __builtin_nanl("");
    long double g = __builtin_sqrt((double)x);
    if (g == 0 || g == __builtin_inf())                 /* outside double's range */
        g = x < 1 ? 1e-300L : 1e300L;
    for (int i = 0; i < 6; i++)
        g = (g + x / g) / 2;
    return g;
}

double cbrt(double x)
{
    if (x == 0 || x != x || __fpclassify(x) == FP_INFINITE)
        return x;
    double a = fabs(x);
    int e;
    double m = frexp(a, &e);
    /* initial guess 2^(e/3), then Newton steps */
    double g = ldexp(m > 0.7 ? 1.0 : 0.85, e / 3);
    for (int i = 0; i < 60; i++) {
        double n = g - (g * g * g - a) / (3 * g * g);
        if (n == g)
            break;
        g = n;
    }
    return copysign(g, x);
}

double hypot(double x, double y)
{
    x = fabs(x);
    y = fabs(y);
    if (__fpclassify(x) == FP_INFINITE || __fpclassify(y) == FP_INFINITE)
        return __builtin_inf();
    return (double)sqrtl((ld)x * x + (ld)y * y);
}

/* ---- exponentials and logarithms (long double cores) ---- */

static ld expl_(ld x)
{
    if (x != x)
        return x;
    if (x > 11356)
        return __builtin_infl();
    if (x < -11400)
        return 0;
    /* x = k ln2 + r, |r| <= ln2 / 2 */
    long k = (long)(x / LN2 + (x < 0 ? -0.5L : 0.5L));
    ld r = x - k * LN2;
    ld term = 1, sum = 1;
    for (int n = 1; n < 30; n++) {
        term *= r / n;
        sum += term;
        if (term < 1e-36L && term > -1e-36L)
            break;
    }
    return ldexpl(sum, (int)k);
}

static ld logl_(ld x)
{
    /* x = m 2^e with m in [sqrt(1/2), sqrt(2)): log m = 2 atanh((m-1)/(m+1)) */
    int e;
    ld m = frexpl(x, &e);
    if (m < 0.70710678118654752440L) {
        m *= 2;
        e--;
    }
    ld s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int n = 1; n < 200; n += 2) {
        ld t = term / n;
        sum += t;
        if (t < 1e-36L && t > -1e-36L)
            break;
        term *= s2;
    }
    return e * LN2 + 2 * sum;
}

double exp(double x)
{
    if (x != x)
        return x;
    return (double)expl_(x);
}

double exp2(double x) { return x != x ? x : (double)expl_((ld)x * LN2); }

double expm1(double x)
{
    if (fabs(x) < 1e-5) {
        ld r = x;
        return (double)(r + r * r / 2 + r * r * r / 6);
    }
    return (double)(expl_(x) - 1);
}

static double log_special(double x)
{
    if (x != x || x == __builtin_inf())
        return x;
    if (x == 0)
        return -__builtin_inf();
    return __builtin_nan("");           /* x < 0 */
}

double log(double x) { return x > 0 && x < __builtin_inf() ? (double)logl_(x) : log_special(x); }
double log2(double x) { return x > 0 && x < __builtin_inf() ? (double)(logl_(x) / LN2) : log_special(x); }
double log10(double x)
{
    return x > 0 && x < __builtin_inf() ? (double)(logl_(x) / 2.30258509299404568401799145468436421L)
                                        : log_special(x);
}

double log1p(double x)
{
    if (fabs(x) < 1e-5) {
        ld r = x;
        return (double)(r - r * r / 2 + r * r * r / 3);
    }
    return log(1 + x);
}

double pow(double x, double y)
{
    if (y == 0)
        return 1;
    if (x == 1)
        return 1;
    if (x != x || y != y)
        return x + y;
    int y_int = y == trunc(y);
    int y_odd = y_int && fabs(y) < 0x1p53 && fmod(y, 2) != 0;
    if (x == 0) {
        if (y < 0)
            return y_odd ? copysign(__builtin_inf(), x) : __builtin_inf();
        return y_odd ? x : 0;
    }
    if (__fpclassify(y) == FP_INFINITE) {
        double ax = fabs(x);
        if (ax == 1)
            return 1;
        return (ax > 1) == (y > 0) ? __builtin_inf() : 0;
    }
    if (__fpclassify(x) == FP_INFINITE) {
        if (x > 0)
            return y > 0 ? x : 0;
        return y > 0 ? (y_odd ? x : -x) : (y_odd ? -0.0 : 0);
    }
    if (x < 0 && !y_int)
        return __builtin_nan("");
    if (y_int && fabs(y) <= 64) {
        /* exact-ish repeated squaring */
        ld r = 1, b = x;
        long n = (long)fabs(y);
        while (n) {
            if (n & 1)
                r *= b;
            b *= b;
            n >>= 1;
        }
        return (double)(y < 0 ? 1 / r : r);
    }
    ld r = expl_((ld)y * logl_(fabs(x)));
    return (double)(x < 0 && y_odd ? -r : r);
}

/* ---- trigonometry ---- */

/* x mod y for x >= 0, y > 0, by exact subtractions of y * 2^k. */
static ld fmodl_(ld x, ld y)
{
    int ex, ey;
    if (x < y)
        return x;
    frexpl(x, &ex);
    frexpl(y, &ey);
    for (int k = ex - ey; k >= 0; k--) {
        ld t = ldexpl(y, k);
        if (x >= t)
            x -= t;
    }
    return x;
}

/* Reduce x to r in [-pi/4, pi/4] with x = r + q pi/2; returns q mod 4.
 * Huge arguments are first brought below 2pi; the result is then only as
 * good as the long double value of 2pi allows (no Payne-Hanek reduction). */
static int reduce(ld x, ld *r)
{
    if (x > 0x1p60L || x < -0x1p60L)
        x = x < 0 ? -fmodl_(-x, 4 * PI_2) : fmodl_(x, 4 * PI_2);
    ld q = x / PI_2;
    ld k = q < 0 ? -(ld)(long long)(-q + 0.5L) : (ld)(long long)(q + 0.5L);
    *r = x - k * PI_2;
    long long qi = (long long)k;
    return (int)(qi & 3);
}

static ld sin_series(ld r)
{
    ld r2 = r * r, term = r, sum = r;
    for (int n = 2; n < 40; n += 2) {
        term *= -r2 / (n * (n + 1));
        sum += term;
    }
    return sum;
}

static ld cos_series(ld r)
{
    ld r2 = r * r, term = 1, sum = 1;
    for (int n = 1; n < 40; n += 2) {
        term *= -r2 / (n * (n + 1));
        sum += term;
    }
    return sum;
}

double sin(double x)
{
    if (x != x || __fpclassify(x) == FP_INFINITE)
        return __builtin_nan("");
    ld r;
    switch (reduce(x, &r)) {
    case 0: return (double)sin_series(r);
    case 1: return (double)cos_series(r);
    case 2: return (double)-sin_series(r);
    default: return (double)-cos_series(r);
    }
}

double cos(double x)
{
    if (x != x || __fpclassify(x) == FP_INFINITE)
        return __builtin_nan("");
    ld r;
    switch (reduce(x, &r)) {
    case 0: return (double)cos_series(r);
    case 1: return (double)-sin_series(r);
    case 2: return (double)-cos_series(r);
    default: return (double)sin_series(r);
    }
}

double tan(double x)
{
    if (x != x || __fpclassify(x) == FP_INFINITE)
        return __builtin_nan("");
    ld r;
    int q = reduce(x, &r);
    ld s = sin_series(r), c = cos_series(r);
    return (double)(q & 1 ? -c / s : s / c);
}

static ld atanl_(ld x)
{
    int neg = x < 0;
    if (neg)
        x = -x;
    int inv = x > 1;
    if (inv)
        x = 1 / x;
    /* atan x = pi/6 + atan((x sqrt3 - 1) / (sqrt3 + x)) for x above tan(pi/12) */
    int shift = x > 0.26794919243112270647L;
    static const ld SQRT3 = 1.73205080756887729352744634150587237L;
    if (shift)
        x = (x * SQRT3 - 1) / (SQRT3 + x);
    ld x2 = x * x, term = x, sum = 0;
    for (int n = 1; n < 200; n += 2) {
        ld t = term / n;
        sum += t;
        if (t < 1e-36L && t > -1e-36L)
            break;
        term *= -x2;
    }
    if (shift)
        sum += PI / 6;
    if (inv)
        sum = PI_2 - sum;
    return neg ? -sum : sum;
}

double atan(double x)
{
    if (x != x)
        return x;
    if (__fpclassify(x) == FP_INFINITE)
        return copysign((double)PI_2, x);
    return (double)atanl_(x);
}

double atan2(double y, double x)
{
    if (x != x || y != y)
        return x + y;
    if (x == 0 && y == 0)
        return __signbit(x) ? copysign((double)PI, y) : copysign(0, y);
    if (__fpclassify(x) == FP_INFINITE || __fpclassify(y) == FP_INFINITE) {
        if (__fpclassify(x) == FP_INFINITE && __fpclassify(y) == FP_INFINITE)
            return copysign((double)(x > 0 ? PI / 4 : 3 * PI / 4), y);
        if (__fpclassify(y) == FP_INFINITE)
            return copysign((double)PI_2, y);
        return x > 0 ? copysign(0, y) : copysign((double)PI, y);
    }
    if (x == 0)
        return copysign((double)PI_2, y);
    ld a = atanl_((ld)y / x);
    if (x < 0)
        a += y < 0 || (y == 0 && __signbit(y)) ? -PI : PI;
    return (double)a;
}

double asin(double x)
{
    if (x < -1 || x > 1)
        return __builtin_nan("");
    return atan2(x, (double)sqrtl(1 - (ld)x * x));
}

double acos(double x)
{
    if (x < -1 || x > 1)
        return __builtin_nan("");
    return atan2((double)sqrtl(1 - (ld)x * x), x);
}

double sinh(double x)
{
    if (fabs(x) < 1e-5)
        return x + x * x * x / 6;
    ld e = expl_(x);
    return (double)((e - 1 / e) / 2);
}

double cosh(double x)
{
    ld e = expl_(x);
    return (double)((e + 1 / e) / 2);
}

double tanh(double x)
{
    if (x > 40)
        return 1;
    if (x < -40)
        return -1;
    if (fabs(x) < 1e-5)
        return x - x * x * x / 3;
    ld e = expl_(2 * (ld)x);
    return (double)((e - 1) / (e + 1));
}

/* ---- float versions ---- */

float fabsf(float x) { return (float)fabs(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }

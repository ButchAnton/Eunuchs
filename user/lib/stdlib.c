/* stdlib: number conversion, sorting, and odds and ends. */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- integers ---- */

static unsigned long long to_ull(const char *s, char **end, int base, int *neg, int *overflow)
{
    const char *p = s;
    *neg = 0;
    *overflow = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-' || *p == '+')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B') &&
               (p[2] == '0' || p[2] == '1')) {
        p += 2;
        base = 2;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p))
            d = *p - '0';
        else if (isalpha((unsigned char)*p))
            d = tolower((unsigned char)*p) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        if (v > (~0ULL - d) / base)
            *overflow = 1;
        v = v * base + d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, ov;
    unsigned long long v = to_ull(s, end, base, &neg, &ov);
    if (ov) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return strtoull(s, end, base);
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, ov;
    unsigned long long v = to_ull(s, end, base, &neg, &ov);
    if (ov || v > (unsigned long long)LLONG_MAX + neg) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

long strtol(const char *s, char **end, int base)
{
    return strtoll(s, end, base);
}

intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r = { a / b, a % b }; return r; }

/* ---- floating point ---- */

static long double pow10l_(int n)
{
    long double r = 1, b = 10;
    int neg = n < 0;
    unsigned u = neg ? -n : n;
    while (u) {
        if (u & 1)
            r *= b;
        b *= b;
        u >>= 1;
    }
    return neg ? 1 / r : r;
}

/* Decimal and hexadecimal floating point, infinity and NaN. The value is
 * built in 128-bit long double, so a double comes out correctly rounded
 * in all but extreme cases. */
long double strtold(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    int neg = 0;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';
    long double v = 0;
    if (!strncasecmp(p, "inf", 3)) {
        p += !strncasecmp(p, "infinity", 8) ? 8 : 3;
        v = ((long double)HUGE_VAL);
    } else if (!strncasecmp(p, "nan", 3)) {
        p += 3;
        if (*p == '(') {
            const char *q = strchr(p, ')');
            if (q)
                p = q + 1;
        }
        v = ((long double)NAN);
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
               (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        p += 2;
        int e2 = 0, any = 0;
        while (isxdigit((unsigned char)*p)) {
            int d = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
            v = v * 16 + d;
            p++;
            any = 1;
        }
        if (*p == '.') {
            p++;
            while (isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
                v = v * 16 + d;
                e2 -= 4;
                p++;
                any = 1;
            }
        }
        if (!any) {
            if (end)
                *end = (char *)s;
            return 0;
        }
        if (*p == 'p' || *p == 'P') {
            char *q;
            long x = strtol(p + 1, &q, 10);
            if (q != p + 1) {
                e2 += (int)x;
                p = q;
            }
        }
        while (e2 > 0) { v *= 2; e2--; }
        while (e2 < 0) { v /= 2; e2++; }
    } else {
        unsigned long long mant = 0;
        int digits = 0, e10 = 0, any = 0;
        while (isdigit((unsigned char)*p)) {
            if (digits < 19) {
                mant = mant * 10 + (*p - '0');
                if (mant)
                    digits++;
            } else {
                e10++;
            }
            p++;
            any = 1;
        }
        if (*p == '.') {
            p++;
            while (isdigit((unsigned char)*p)) {
                if (digits < 19) {
                    mant = mant * 10 + (*p - '0');
                    if (mant)
                        digits++;
                    e10--;
                }
                p++;
                any = 1;
            }
        }
        if (!any) {
            if (end)
                *end = (char *)s;
            return 0;
        }
        if (*p == 'e' || *p == 'E') {
            char *q;
            long x = strtol(p + 1, &q, 10);
            if (q != p + 1) {
                if (x > 100000) x = 100000;
                if (x < -100000) x = -100000;
                e10 += (int)x;
                p = q;
            }
        }
        v = mant;
        if (mant && e10) {
            if (e10 > 5000)
                v = ((long double)HUGE_VAL);
            else if (e10 < -5000)
                v = 0;
            else
                v = e10 > 0 ? v * pow10l_(e10) : v / pow10l_(-e10);
        }
    }
    if (end)
        *end = (char *)p;
    return neg ? -v : v;
}

double strtod(const char *s, char **end)
{
    long double v = strtold(s, end);
    double d = (double)v;
    if ((d == HUGE_VAL || d == -HUGE_VAL) && v == v &&
        v != ((long double)HUGE_VAL) && v != -((long double)HUGE_VAL))
        errno = ERANGE;
    return d;
}

float strtof(const char *s, char **end)
{
    return (float)strtold(s, end);
}

double atof(const char *s)
{
    return strtod(s, 0);
}

/* ---- sorting and searching ---- */

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Introsort-free quicksort: median of three, insertion sort for small runs,
 * recursion on the smaller half. */
static void qsort_r_(char *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    while (n > 12) {
        char *lo = base, *mid = base + (n / 2) * size, *hi = base + (n - 1) * size;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        if (cmp(hi, mid) < 0) {
            swap_bytes(hi, mid, size);
            if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        }
        swap_bytes(mid, base, size);            /* pivot to the front */
        size_t i = 1, j = n - 1;
        for (;;) {
            while (i <= j && cmp(base + i * size, base) < 0)
                i++;
            while (j >= i && cmp(base + j * size, base) > 0)
                j--;
            if (i >= j)
                break;
            swap_bytes(base + i * size, base + j * size, size);
            i++;
            j--;
        }
        swap_bytes(base, base + j * size, size);
        size_t left = j, right = n - j - 1;
        if (left < right) {
            qsort_r_(base, left, size, cmp);
            base += (j + 1) * size;
            n = right;
        } else {
            qsort_r_(base + (j + 1) * size, right, size, cmp);
            n = left;
        }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t k = i; k > 0 && cmp(base + (k - 1) * size, base + k * size) > 0; k--)
            swap_bytes(base + (k - 1) * size, base + k * size, size);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    if (n > 1 && size)
        qsort_r_(base, n, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = base;
    while (n) {
        const char *mid = b + (n / 2) * size;
        int r = cmp(key, mid);
        if (!r)
            return (void *)mid;
        if (r > 0) {
            b = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return 0;
}

void *reallocarray(void *p, size_t n, size_t size)
{
    if (size && n > ((size_t)-1) / size) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(p, n * size);
}

/* ---- random numbers (a 64-bit LCG, high bits) ---- */

static unsigned long long rand_state = 1;

void srand(unsigned seed) { rand_state = seed ? seed : 1; }
void srandom(unsigned seed) { srand(seed); }

int rand(void)
{
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(rand_state >> 33);
}

long random(void) { return rand(); }

/* ---- process ---- */

void abort(void)
{
    fflush(0);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(134);
}

void _Exit(int status)
{
    _exit(status);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    fflush(stdout);
    fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", file, line, func, expr);
    abort();
}

int system(const char *cmd)
{
    if (!cmd)
        return 1;
    fflush(0);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "sh", "-c", (char *)cmd, 0 };
        execv("/bin/sh", argv);
        _exit(127);
    }
    void (*oi)(int) = signal(SIGINT, SIG_IGN);
    void (*oq)(int) = signal(SIGQUIT, SIG_IGN);
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) {
            st = -1;
            break;
        }
    signal(SIGINT, oi);
    signal(SIGQUIT, oq);
    return st;
}

int putenv(char *s)
{
    char *eq = strchr(s, '=');
    if (!eq)
        return unsetenv(s);
    char name[128];
    snprintf(name, sizeof(name), "%.*s", (int)(eq - s), s);
    return setenv(name, eq + 1, 1);
}

/* ---- paths and temporary files ---- */

char *realpath(const char *path, char *resolved)
{
    char buf[PATH_MAX];
    if (path[0] == '/') {
        snprintf(buf, sizeof(buf), "%s", path);
    } else {
        if (!getcwd(buf, sizeof(buf)))
            return 0;
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "/%s", path);
    }
    /* normalize: drop empty and "." components, resolve ".." */
    char out[PATH_MAX];
    size_t o = 0;
    out[o++] = '/';
    for (char *t = buf; *t;) {
        while (*t == '/')
            t++;
        if (!*t)
            break;
        char *e = t;
        while (*e && *e != '/')
            e++;
        size_t len = e - t;
        if (len == 1 && t[0] == '.') {
        } else if (len == 2 && t[0] == '.' && t[1] == '.') {
            if (o > 1) {
                o--;
                while (o > 1 && out[o - 1] != '/')
                    o--;
            }
        } else {
            if (o > 1)
                out[o++] = '/';
            if (o + len >= sizeof(out)) {
                errno = ENAMETOOLONG;
                return 0;
            }
            memcpy(out + o, t, len);
            o += len;
        }
        t = e;
    }
    out[o] = 0;
    struct stat st;
    if (stat(out, &st) < 0)
        return 0;
    if (!resolved) {
        resolved = malloc(o + 1);
        if (!resolved)
            return 0;
    }
    memcpy(resolved, out, o + 1);
    return resolved;
}

static int fill_template(char *tmpl)
{
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned long long r = (unsigned long long)getpid() * 2654435761ULL + uptime_ms() + rand();
    for (int i = 0; i < 6; i++) {
        tmpl[n - 6 + i] = chars[r % 36];
        r = r / 36 + rand();
    }
    return 0;
}

int mkstemp(char *tmpl)
{
    for (int tries = 0; tries < 100; tries++) {
        if (fill_template(tmpl) < 0)
            return -1;
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

char *mktemp(char *tmpl)
{
    struct stat st;
    for (int tries = 0; tries < 100; tries++) {
        if (fill_template(tmpl) < 0)
            return 0;
        if (stat(tmpl, &st) < 0)
            return tmpl;
    }
    tmpl[0] = 0;
    return tmpl;
}

char *mkdtemp(char *tmpl)
{
    for (int tries = 0; tries < 100; tries++) {
        if (fill_template(tmpl) < 0)
            return 0;
        if (mkdir(tmpl, 0700) == 0)
            return tmpl;
        if (errno != EEXIST)
            return 0;
    }
    return 0;
}

/* ---- getopt ---- */

char *optarg;
int optind = 1, opterr = 1, optopt;
static int optpos = 1;

int getopt(int argc, char *const argv[], const char *optstring)
{
    if (optind == 0) {          /* reset */
        optind = 1;
        optpos = 1;
    }
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
        return -1;
    if (!strcmp(argv[optind], "--")) {
        optind++;
        return -1;
    }
    int quiet = optstring[0] == ':';
    char c = argv[optind][optpos];
    const char *spec = c == ':' ? 0 : strchr(optstring, c);
    optopt = c;
    if (!spec) {
        if (!argv[optind][++optpos]) {
            optind++;
            optpos = 1;
        }
        if (opterr && !quiet)
            fprintf(stderr, "%s: unknown option -- '%c'\n", argv[0], c);
        return '?';
    }
    if (spec[1] == ':') {
        if (argv[optind][optpos + 1]) {
            optarg = &argv[optind][optpos + 1];
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
        } else {
            optind++;
            optpos = 1;
            if (opterr && !quiet)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            return quiet ? ':' : '?';
        }
        optpos = 1;
        return c;
    }
    if (!argv[optind][++optpos]) {
        optind++;
        optpos = 1;
    }
    return c;
}

/* fnmatch: shell wildcard matching (* ? [abc] [a-z] [!x] and \x). */
#include <ctype.h>
#include <fnmatch.h>

static int fold(int c, int flags)
{
    return flags & FNM_CASEFOLD ? tolower(c) : c;
}

/* A bracket expression at P (just past '['): sets *matched, returns the
   character after ']', or 0 if it isn't one (then '[' is literal). */
static const char *bracket(const char *p, int c, int flags, int *matched)
{
    int neg = *p == '!' || *p == '^';
    if (neg)
        p++;
    int found = 0, first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        int lo = (unsigned char)*p++;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p)
            lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = (unsigned char)*p++;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p)
                hi = (unsigned char)*p++;
        }
        if (fold(c, flags) >= fold(lo, flags) && fold(c, flags) <= fold(hi, flags))
            found = 1;
        else if ((flags & FNM_CASEFOLD) && c >= lo && c <= hi)
            found = 1;
    }
    if (*p != ']')
        return 0;
    *matched = found != neg;
    return p + 1;
}

static int match(const char *p, const char *s, const char *start, int flags)
{
    for (;;) {
        int c = (unsigned char)*s;
        int leading = (flags & FNM_PERIOD) && c == '.' &&
                      (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'));
        switch (*p) {
        case 0:
            return c ? FNM_NOMATCH : 0;
        case '?':
            if (!c || leading || ((flags & FNM_PATHNAME) && c == '/'))
                return FNM_NOMATCH;
            p++, s++;
            continue;
        case '*':
            while (*p == '*')
                p++;
            if (leading)
                return FNM_NOMATCH;
            for (;; s++) {
                if (!match(p, s, start, flags & ~FNM_PERIOD))
                    return 0;
                if (!*s || ((flags & FNM_PATHNAME) && *s == '/'))
                    return FNM_NOMATCH;
            }
        case '[': {
            int m;
            const char *after = c ? bracket(p + 1, c, flags, &m) : 0;
            if (after) {
                if (!m || leading || ((flags & FNM_PATHNAME) && c == '/'))
                    return FNM_NOMATCH;
                p = after, s++;
                continue;
            }
            if (c != '[')
                return FNM_NOMATCH;
            p++, s++;
            continue;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1])
                p++;
            /* fall through */
        default:
            if (fold(*p, flags) != fold(c, flags))
                return FNM_NOMATCH;
            p++, s++;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    return match(pattern, string, string, flags);
}

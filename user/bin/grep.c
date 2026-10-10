/*
 * grep [-v] [-n] [-i] [-c] PATTERN [FILE...]
 * Patterns: c . ^ $ *  (Kernighan & Pike's matcher), plain text otherwise.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int icase;

static int eq(char p, char c)
{
    if (icase) {
        if (p >= 'A' && p <= 'Z') p += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    return p == '.' || p == c;
}

static int match_here(const char *re, const char *s);

static int match_star(char c, const char *re, const char *s)
{
    do {
        if (match_here(re, s))
            return 1;
    } while (*s && eq(c, *s++));
    return 0;
}

static int match_here(const char *re, const char *s)
{
    if (!re[0])
        return 1;
    if (re[1] == '*')
        return match_star(re[0], re + 2, s);
    if (re[0] == '$' && !re[1])
        return !*s;
    if (*s && eq(re[0], *s))
        return match_here(re + 1, s + 1);
    return 0;
}

static int match(const char *re, const char *s)
{
    if (re[0] == '^')
        return match_here(re + 1, s);
    do {
        if (match_here(re, s))
            return 1;
    } while (*s++);
    return 0;
}

int main(int argc, char **argv)
{
    int invert = 0, number = 0, count = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'v') invert = 1;
            else if (*f == 'n') number = 1;
            else if (*f == 'i') icase = 1;
            else if (*f == 'c') count = 1;
            else {
                eprintf("grep: unknown option -%c\n", *f);
                return 2;
            }
        }
    if (i == argc) {
        eprintf("usage: grep [-v] [-n] [-i] [-c] PATTERN [FILE...]\n");
        return 2;
    }
    const char *re = argv[i++];
    int nfiles = argc - i, found = 0;
    for (int k = 0; k < (nfiles ? nfiles : 1); k++) {
        const char *name = nfiles ? argv[i + k] : "(stdin)";
        int fd = nfiles ? open(name, O_RDONLY) : 0;
        if (fd < 0) {
            eprintf("grep: %s: %s\n", name, strerror(errno));
            continue;
        }
        char line[1024], c;
        int len = 0, lineno = 0, matches = 0;
        long r;
        while ((r = read(fd, &c, 1)) >= 0) {
            if (r == 1 && c != '\n') {
                if (len < (int)sizeof(line) - 1)
                    line[len++] = c;
                continue;
            }
            if (r == 0 && len == 0)
                break;
            line[len] = 0;
            lineno++;
            if (match(re, line) != invert) {
                matches++;
                if (!count) {
                    if (nfiles > 1)
                        printf("%s:", name);
                    if (number)
                        printf("%d:", lineno);
                    printf("%s\n", line);
                }
            }
            len = 0;
            if (r == 0)
                break;
        }
        if (count)
            printf("%s%s%d\n", nfiles > 1 ? name : "", nfiles > 1 ? ":" : "", matches);
        found += matches;
        if (nfiles)
            close(fd);
    }
    return found ? 0 : 1;
}

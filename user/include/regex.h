#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>
#include <sys/types.h>

/* POSIX regular expressions: basic (BRE) and extended (ERE) syntax, with
 * the common GNU additions \| \+ \? in BRE, \< \> \b \w \W \s \S, and
 * backreferences \1-\9. Matching is leftmost, by backtracking (not
 * leftmost-longest). */

typedef long regoff_t;

typedef struct {
    size_t re_nsub;     /* number of parenthesized subexpressions */
    void *re_prog;      /* compiled program */
    int re_flags;
} regex_t;

typedef struct {
    regoff_t rm_so;     /* start, or -1 */
    regoff_t rm_eo;     /* end (exclusive) */
} regmatch_t;

/* regcomp flags */
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8
/* regexec flags */
#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_STARTEND 4  /* match within pmatch[0].rm_so .. rm_eo */

/* errors */
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ECOLLATE 3
#define REG_ECTYPE   4
#define REG_EESCAPE  5
#define REG_ESUBREG  6
#define REG_EBRACK   7
#define REG_EPAREN   8
#define REG_EBRACE   9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int regcomp(regex_t *re, const char *pattern, int cflags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int err, const regex_t *re, char *buf, size_t size);
void regfree(regex_t *re);

#endif

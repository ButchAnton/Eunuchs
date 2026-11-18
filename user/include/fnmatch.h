#ifndef _FNMATCH_H
#define _FNMATCH_H

#define FNM_NOMATCH   1
#define FNM_NOESCAPE  0x01          /* backslash is an ordinary character */
#define FNM_PATHNAME  0x02          /* '/' only matches '/' */
#define FNM_PERIOD    0x04          /* a leading '.' only matches '.' */
#define FNM_CASEFOLD  0x10          /* ignore case */

/* 0 if STRING matches the shell pattern PATTERN (* ? [...] [!...]), else FNM_NOMATCH */
int fnmatch(const char *pattern, const char *string, int flags);

#endif

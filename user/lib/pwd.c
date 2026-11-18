/*
 * pwd.h and grp.h: /etc/passwd and /etc/group, read a line at a time. The
 * structures returned are static and overwritten by the next call.
 */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Split LINE at ':' into at most N fields (the last keeps the rest). */
static int fields(char *line, char **f, int n)
{
    int i = 0;
    line[strcspn(line, "\n")] = 0;
    f[i++] = line;
    while (i < n && (line = strchr(line, ':'))) {
        *line++ = 0;
        f[i++] = line;
    }
    return i;
}

static FILE *pwf;
static char pwline[512];
static struct passwd pw;

void setpwent(void)
{
    if (pwf)
        rewind(pwf);
}

void endpwent(void)
{
    if (pwf)
        fclose(pwf);
    pwf = 0;
}

struct passwd *getpwent(void)
{
    if (!pwf && !(pwf = fopen("/etc/passwd", "r")))
        return 0;
    while (fgets(pwline, sizeof(pwline), pwf)) {
        char *f[7];
        if (fields(pwline, f, 7) < 7)
            continue;                   /* malformed */
        pw.pw_name = f[0];
        pw.pw_passwd = f[1];
        pw.pw_uid = (uid_t)atoi(f[2]);
        pw.pw_gid = (gid_t)atoi(f[3]);
        pw.pw_gecos = f[4];
        pw.pw_dir = f[5];
        pw.pw_shell = f[6];
        return &pw;
    }
    return 0;
}

struct passwd *getpwnam(const char *name)
{
    struct passwd *p;
    setpwent();
    while ((p = getpwent()) && strcmp(p->pw_name, name))
        ;
    endpwent();
    return p;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *p;
    setpwent();
    while ((p = getpwent()) && p->pw_uid != uid)
        ;
    endpwent();
    return p;
}

#define MAXMEM 64

static FILE *grf;
static char grline[1024];
static char *grmem[MAXMEM + 1];
static struct group gr;

void setgrent(void)
{
    if (grf)
        rewind(grf);
}

void endgrent(void)
{
    if (grf)
        fclose(grf);
    grf = 0;
}

struct group *getgrent(void)
{
    if (!grf && !(grf = fopen("/etc/group", "r")))
        return 0;
    while (fgets(grline, sizeof(grline), grf)) {
        char *f[4];
        int nf = fields(grline, f, 4);
        if (nf < 3)
            continue;
        gr.gr_name = f[0];
        gr.gr_passwd = f[1];
        gr.gr_gid = (gid_t)atoi(f[2]);
        int n = 0;
        char *save, *m;
        for (m = nf == 4 ? strtok_r(f[3], ",", &save) : 0; m && n < MAXMEM;
             m = strtok_r(0, ",", &save))
            grmem[n++] = m;
        grmem[n] = 0;
        gr.gr_mem = grmem;
        return &gr;
    }
    return 0;
}

struct group *getgrnam(const char *name)
{
    struct group *g;
    setgrent();
    while ((g = getgrent()) && strcmp(g->gr_name, name))
        ;
    endgrent();
    return g;
}

struct group *getgrgid(gid_t gid)
{
    struct group *g;
    setgrent();
    while ((g = getgrent()) && g->gr_gid != gid)
        ;
    endgrent();
    return g;
}

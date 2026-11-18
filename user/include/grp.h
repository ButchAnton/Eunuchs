#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>

/* /etc/group: name:password:gid:member,member,... */
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;                  /* NULL-terminated */
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
void setgrent(void);
struct group *getgrent(void);
void endgrent(void);

#endif

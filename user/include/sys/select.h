#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <time.h>

/* select(), implemented with poll (descriptors below FD_SETSIZE). */
#define FD_SETSIZE 64
typedef struct {
    unsigned long bits[FD_SETSIZE / 64];
} fd_set;

#define FD_ZERO(s)      do { for (int __i = 0; __i < FD_SETSIZE / 64; __i++) (s)->bits[__i] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->bits[(fd) / 64] |= 1UL << ((fd) % 64))
#define FD_CLR(fd, s)   ((s)->bits[(fd) / 64] &= ~(1UL << ((fd) % 64)))
#define FD_ISSET(fd, s) (((s)->bits[(fd) / 64] >> ((fd) % 64)) & 1)

int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);

#endif

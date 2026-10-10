#ifndef _POLL_H
#define _POLL_H

#include <uapi/net.h>

typedef unsigned long nfds_t;
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);   /* -1: wait forever */

#endif

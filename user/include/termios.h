#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <uapi/termios.h>
#include <unistd.h>

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
void cfmakeraw(struct termios *t);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
int isatty(int fd);
int ioctl(int fd, unsigned long req, ...);

#endif

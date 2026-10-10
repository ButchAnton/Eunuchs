#ifndef _DIRENT_H
#define _DIRENT_H

#include <uapi/fs.h>

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif

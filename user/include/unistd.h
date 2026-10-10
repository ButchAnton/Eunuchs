#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef int pid_t;

long __syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
void _exit(int status) __attribute__((noreturn));
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int fcntl(int fd, int cmd, ...);
pid_t getpid(void);
pid_t getppid(void);
int brk(void *addr);
void *sbrk(long increment);
int sched_yield(void);
int kill(pid_t pid, int sig);
unsigned sleep_ms(unsigned long ms);
unsigned long uptime_ms(void);
int close(int fd);
long lseek(int fd, long off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int link(const char *oldpath, const char *newpath);
int rename(const char *oldpath, const char *newpath);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int dup(int fd);
int dup2(int oldfd, int newfd);
void sync(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);
typedef unsigned short uid_t;
typedef unsigned short gid_t;
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int chown(const char *path, int uid, int gid);
int access(const char *path, int mode);

#endif

#ifndef _SETJMP_H
#define _SETJMP_H

/* ra, sp, s0-s11, fs0-fs11, and the signal mask for sigsetjmp. */
typedef unsigned long jmp_buf[28];
typedef unsigned long sigjmp_buf[30];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
int sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#endif

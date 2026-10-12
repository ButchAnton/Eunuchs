/* sigsetjmp: setjmp plus the signal mask (stored after the registers). */
#include <setjmp.h>
#include <signal.h>

/* sigsetjmp must not return through a C frame of its own, so it is a macro
 * in spirit: we save the mask here and let setjmp (assembly) do the rest. */
int __sigsetjmp_save(sigjmp_buf env, int savemask)
{
    env[28] = (unsigned long)savemask;
    if (savemask) {
        sigset_t m;
        sigprocmask(SIG_BLOCK, 0, &m);
        env[29] = m;
    }
    return 0;
}

void siglongjmp(sigjmp_buf env, int val)
{
    if (env[28]) {
        sigset_t m = (sigset_t)env[29];
        sigprocmask(SIG_SETMASK, &m, 0);
    }
    longjmp(env, val);
}

#include <proc.h>
#include <uapi/errno.h>

size_t __copy_user(void *dst, const void *src, size_t n);
long __strncpy_user(char *dst, uintptr_t usrc, size_t max);

long copy_from_user(void *dst, uintptr_t usrc, size_t n)
{
    if (!user_range_ok(usrc, n))
        return -EFAULT;
    return __copy_user(dst, (const void *)usrc, n) ? -EFAULT : 0;
}

long copy_to_user(uintptr_t udst, const void *src, size_t n)
{
    if (!user_range_ok(udst, n))
        return -EFAULT;
    return __copy_user((void *)udst, src, n) ? -EFAULT : 0;
}

long strncpy_from_user(char *dst, uintptr_t usrc, size_t max)
{
    if (!max || !user_range_ok(usrc, 1))
        return -EFAULT;
    /* Never let the copy run past USER_TOP. */
    size_t limit = MIN(max, (size_t)(USER_TOP - usrc));
    long n = __strncpy_user(dst, usrc, limit);
    if (n < 0 || ((size_t)n == limit && limit < max))
        return -EFAULT;                 /* fault, or no NUL before USER_TOP */
    if ((size_t)n >= max) {
        dst[max - 1] = '\0';
        return -ENAMETOOLONG;
    }
    dst[n] = '\0';
    return n;
}

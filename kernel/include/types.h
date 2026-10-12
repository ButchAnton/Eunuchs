#ifndef EUNUCHS_TYPES_H
#define EUNUCHS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __TINYC__
/*
 * Building with tcc: gcc's builtins the kernel uses. tcc's <stdatomic.h>
 * has the __ATOMIC_* orders and the __atomic_*_n forms; tcc turns the
 * __atomic_* operations into calls to its runtime (atomic.S, stdatomic.c),
 * linked into the kernel, as it does __builtin_ctz (builtin.c).
 */
#include <stdatomic.h>
#define __sync_synchronize() __asm__ __volatile__("fence rw, rw" ::: "memory")
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((__typeof__(x))(a) - 1))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))

/* Guarded: some also exist in host headers (host tests compile kernel code). */
#ifndef __noreturn
#define __noreturn        __attribute__((noreturn))
#endif
#ifndef __noinline
#define __noinline        __attribute__((noinline))
#endif
#ifndef __used
#define __used            __attribute__((used))
#endif
#ifndef __unused
#define __unused          __attribute__((unused))
#endif
#ifndef __section
#define __section(s)      __attribute__((section(s)))
#endif
#ifndef __aligned
#define __aligned(n)      __attribute__((aligned(n)))
#endif
#ifndef __printf_like
#define __printf_like(f, a) __attribute__((format(printf, f, a)))
#endif
#define likely(x)         __builtin_expect(!!(x), 1)
#define unlikely(x)       __builtin_expect(!!(x), 0)

#endif

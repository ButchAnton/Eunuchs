/*
 * TinyCC configuration for Eunuchs. Shared by the compiler that runs on
 * Eunuchs (/bin/tcc) and the host cross compiler (build/tcc/xtcc) used to
 * build its runtime library. Every path is relative to the tcc directory
 * ({B}, /usr/lib/tcc by default, or -B), so the host compiler can use the
 * staged sysroot in build/sysroot unchanged.
 */
#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_RISCV64 1
#define CONFIG_TCC_STATIC 1             /* no dlopen in tcc itself */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCCDIR "/usr/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:{B}/../../include"
#define CONFIG_TCC_LIBPATHS "{B}:{B}/.."
#define CONFIG_TCC_CRTPREFIX "{B}/.."
#define CONFIG_TCC_ELFINTERP "-"
#define CONFIG_TCC_SWITCHES "-static"   /* no shared libraries on Eunuchs */
#define CONFIG_RUNMEM_RO 1             /* -run: text r-x, data rw- (the kernel enforces W^X) */
#define CONFIG_DWARF_VERSION 5         /* -g: DWARF (what db reads), not stabs */
#define ONE_SOURCE 1

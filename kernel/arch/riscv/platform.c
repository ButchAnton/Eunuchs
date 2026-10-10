#include <mm.h>
#include <platform.h>
#include <sbi.h>

/* sifive,test0 "finisher" commands */
#define FINISHER_FAIL  0x3333
#define FINISHER_PASS  0x5555
#define FINISHER_RESET 0x7777

static volatile u32 *finisher;

void platform_init(void)
{
    if (platform.test.base)
        finisher = ioremap(platform.test.base, platform.test.size ? platform.test.size : 0x1000);
}

void platform_exit(int code)
{
    if (finisher)
        *finisher = code == 0 ? FINISHER_PASS : ((u32)(code & 0xffff) << 16) | FINISHER_FAIL;
    sbi_shutdown(code != 0);
}

void platform_reboot(void)
{
    if (finisher)
        *finisher = FINISHER_RESET;
    sbi_reboot();
}

/* Goldfish RTC (QEMU virt): wall-clock time in ns since the epoch. */
#include <mm.h>
#include <platform.h>
#include <riscv.h>

#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

static volatile u32 *rtc;

u64 rtc_time_ns(void)
{
    if (!rtc) {
        if (!platform.rtc.base)
            return 0;
        rtc = ioremap(platform.rtc.base, PAGE_SIZE);
    }
    u32 lo = rtc[RTC_TIME_LOW / 4];         /* reading low latches high */
    u32 hi = rtc[RTC_TIME_HIGH / 4];
    return (u64)hi << 32 | lo;
}

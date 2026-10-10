#ifndef EUNUCHS_SBI_H
#define EUNUCHS_SBI_H

#include <types.h>

#define SBI_EXT_BASE  0x10
#define SBI_EXT_TIME  0x54494D45 /* "TIME" */
#define SBI_EXT_IPI   0x735049   /* "sPI"  */
#define SBI_EXT_HSM   0x48534D   /* "HSM"  */
#define SBI_EXT_SRST  0x53525354 /* "SRST" */

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_call(long ext, long fid, long a0, long a1, long a2);

void sbi_console_putchar(int ch);
int  sbi_console_getchar(void);

long sbi_spec_version(void);
long sbi_impl_id(void);
long sbi_impl_version(void);
bool sbi_probe(long ext);
const char *sbi_impl_name(long id);

void sbi_set_timer(u64 stime);
long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base);
long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque);
long sbi_hart_status(unsigned long hartid);

void sbi_shutdown(bool failure) __attribute__((noreturn));
void sbi_reboot(void) __attribute__((noreturn));

#endif

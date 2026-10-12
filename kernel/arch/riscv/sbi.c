#define LOG_SUBSYS SUBSYS_CORE
#include <printk.h>
#include <sbi.h>

#define SBI_EXT_LEGACY_PUTCHAR 0x01
#define SBI_EXT_LEGACY_GETCHAR 0x02

struct sbiret sbi_call(long ext, long fid, long a0, long a1, long a2)
{
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a6 asm("a6") = fid;
    register long r_a7 asm("a7") = ext;
    asm volatile("ecall"
                 : "+r"(r_a0), "+r"(r_a1)
                 : "r"(r_a2), "r"(r_a6), "r"(r_a7)
                 : "memory");
    return (struct sbiret){ .error = r_a0, .value = r_a1 };
}

void sbi_console_putchar(int ch)
{
    sbi_call(SBI_EXT_LEGACY_PUTCHAR, 0, ch, 0, 0);
}

int sbi_console_getchar(void)
{
    return (int)sbi_call(SBI_EXT_LEGACY_GETCHAR, 0, 0, 0, 0).error;
}

long sbi_spec_version(void) { return sbi_call(SBI_EXT_BASE, 0, 0, 0, 0).value; }
long sbi_impl_id(void)      { return sbi_call(SBI_EXT_BASE, 1, 0, 0, 0).value; }
long sbi_impl_version(void) { return sbi_call(SBI_EXT_BASE, 2, 0, 0, 0).value; }

bool sbi_probe(long ext)
{
    return sbi_call(SBI_EXT_BASE, 3, ext, 0, 0).value != 0;
}

const char *sbi_impl_name(long id)
{
    static const char *const names[] = {
        "BBL", "OpenSBI", "Xvisor", "KVM", "RustSBI", "Diosix", "Coffer",
    };
    return id >= 0 && id < (long)ARRAY_SIZE(names) ? names[id] : "unknown";
}

void sbi_set_timer(u64 stime)
{
    sbi_call(SBI_EXT_TIME, 0, (long)stime, 0, 0);
}

long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base)
{
    return sbi_call(SBI_EXT_IPI, 0, hart_mask, hart_mask_base, 0).error;
}

long sbi_remote_fence_i(unsigned long hart_mask, unsigned long hart_mask_base)
{
    return sbi_call(SBI_EXT_RFENCE, 0, hart_mask, hart_mask_base, 0).error;
}

long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque)
{
    return sbi_call(SBI_EXT_HSM, 0, hartid, start_addr, opaque).error;
}

long sbi_hart_status(unsigned long hartid)
{
    struct sbiret r = sbi_call(SBI_EXT_HSM, 2, hartid, 0, 0);
    return r.error ? r.error : r.value;
}

void sbi_shutdown(bool failure)
{
    /* SRST type 0 = shutdown; reason 0 = none, 1 = system failure */
    sbi_call(SBI_EXT_SRST, 0, 0, failure ? 1 : 0, 0);
    sbi_call(0x08, 0, 0, 0, 0);         /* no SRST: the legacy shutdown */
    for (;;)
        asm volatile("wfi");
}

void sbi_reboot(void)
{
    sbi_call(SBI_EXT_SRST, 0, 1 /* cold reboot */, 0, 0);
    pr_err("reboot: the SBI cannot reset the machine (no SRST); powering off\n");
    sbi_shutdown(false);
}

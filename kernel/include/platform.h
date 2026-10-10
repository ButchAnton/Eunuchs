#ifndef EUNUCHS_PLATFORM_H
#define EUNUCHS_PLATFORM_H

#include <types.h>

/* Hardware description gathered from the device tree at boot. */

#define MAX_VIRTIO 8

struct mmio_dev {
    uintptr_t base;
    size_t size;
    u32 irq;
};

struct platform_info {
    uintptr_t dtb;
    u32 dtb_size;
    unsigned long boot_hart;
    u32 ncpus;              /* cpu nodes in the device tree */
    unsigned long hartids[8];   /* their hart ids (reg), first MAX_CPUS */
    u64 timebase;           /* timer ticks per second */
    u64 mem_base;
    u64 mem_size;
    struct mmio_dev uart;
    struct mmio_dev plic;
    struct mmio_dev clint;
    struct mmio_dev test;   /* sifive,test0: QEMU exit device */
    struct mmio_dev rtc;
    struct mmio_dev virtio[MAX_VIRTIO];
    int nvirtio;
    struct {
        u64 base, size;
    } reserved[8];          /* /reserved-memory children and /memreserve/ */
    int nreserved;
    char model[64];
    char bootargs[256];
};

extern struct platform_info platform;

/* fdt.c */
int fdt_parse(uintptr_t dtb);
void fdt_dump(int (*print)(const char *fmt, ...));
void platform_print(int (*print)(const char *fmt, ...));

/* platform.c */
void platform_init(void);                   /* map platform devices (exit device) */
void platform_exit(int code) __noreturn;   /* 0 = success; QEMU exits with `code` */
void platform_reboot(void) __noreturn;

/* Kernel image layout from the linker script. */
extern char __kernel_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __bss_start[], __bss_end[], __data_end[];
extern char __stacks_start[], __stacks_end[];
extern char __ksyms_start[], __kernel_end[];
extern char boot_stack[], boot_stack_top[], overflow_stack_top[];

#endif

/*
 * RISC-V PLIC: routes device interrupts to the boot hart's S-mode context
 * (cpu0). With SMP the other CPUs take only timer ticks and IPIs; keeping
 * device interrupts on one CPU means no claim races and handlers never run
 * concurrently with themselves.
 * Only the register pages actually used are mapped (priorities, our enable
 * bits, our threshold/claim page).
 */
#define LOG_SUBSYS SUBSYS_DRV
#include <debug.h>
#include <irq.h>
#include <mm.h>
#include <percpu.h>
#include <platform.h>
#include <printk.h>
#include <riscv.h>
#include <trace.h>

#define PLIC_PRIORITY(irq)     (4 * (irq))
#define PLIC_ENABLE(ctx)       (0x2000 + 0x80 * (ctx))
#define PLIC_THRESHOLD(ctx)    (0x200000 + 0x1000 * (ctx))
#define PLIC_CLAIM(ctx)        (0x200004 + 0x1000 * (ctx))

struct irq_desc {
    const char *name;
    irq_handler_t handler;
    void *arg;
    u64 count;
    u64 time;               /* rdtime ticks spent in the handler */
};

static struct irq_desc irqs[MAX_IRQS];
static u64 spurious;
static uintptr_t plic;      /* kernel VA of the PLIC base */
static int plic_ctx;

static volatile u32 *reg(uintptr_t off) { return (volatile u32 *)(plic + off); }

void plic_init(void)
{
    if (!platform.plic.base) {
        pr_warn("no PLIC in the device tree; device interrupts disabled\n");
        return;
    }
    plic_ctx = 2 * platform.boot_hart + 1;      /* S-mode context of this hart */
    uintptr_t pa = platform.plic.base;
    ioremap(pa, PAGE_SIZE);                                     /* priorities */
    ioremap(pa + ALIGN_DOWN(PLIC_ENABLE(plic_ctx), PAGE_SIZE), PAGE_SIZE);
    ioremap(pa + ALIGN_DOWN(PLIC_THRESHOLD(plic_ctx), PAGE_SIZE), PAGE_SIZE);
    plic = (uintptr_t)__va(pa);

    *reg(PLIC_THRESHOLD(plic_ctx)) = 0;
    csr_set(sie, SIE_SEIE);
    pr_info("PLIC at %#lx, hart %lu S-mode context %d\n", (unsigned long)pa,
            platform.boot_hart, plic_ctx);
}

int irq_register(u32 irq, const char *name, irq_handler_t handler, void *arg)
{
    if (!plic || irq == 0 || irq >= MAX_IRQS || irqs[irq].handler) {
        pr_err("irq_register(%u, %s) failed\n", irq, name);
        return -1;
    }
    irqs[irq] = (struct irq_desc){ .name = name, .handler = handler, .arg = arg };
    *reg(PLIC_PRIORITY(irq)) = 1;
    *reg(PLIC_ENABLE(plic_ctx) + 4 * (irq / 32)) |= 1U << (irq % 32);
    pr_info("irq %u -> %s\n", irq, name);
    return 0;
}

void plic_handle(void)
{
    struct cpu *c = this_cpu();
    u32 irq;

    while ((irq = *reg(PLIC_CLAIM(plic_ctx))) != 0) {
        c->last_irq = irq;
        if (irq < MAX_IRQS && irqs[irq].handler) {
            u64 t0 = rdtime();
            irqs[irq].count++;
            irqs[irq].handler(irqs[irq].arg);
            irqs[irq].time += rdtime() - t0;
        } else {
            spurious++;
        }
        *reg(PLIC_CLAIM(plic_ctx)) = irq;       /* complete */
    }
}

void irq_print(print_fn print)
{
    u64 tb = platform.timebase ? platform.timebase : 10000000;
    static const struct { int code; const char *name; } local[] = {
        { IRQ_S_TIMER, "timer" }, { IRQ_S_SOFT, "ipi" }, { IRQ_S_EXT, "external" },
    };

    print("  source         ");
    for (int c = 0; c < ncpus_online; c++)
        print("      cpu%d", c);
    print("\n");
    for (size_t i = 0; i < ARRAY_SIZE(local); i++) {
        print("  %-14s ", local[i].name);
        for (int c = 0; c < ncpus_online; c++)
            print(" %9lu", (unsigned long)cpus[c].irq_count[local[i].code]);
        print("%s\n", local[i].code == IRQ_S_TIMER ? "   (per-CPU tick)" : "");
    }
    print("  device interrupts (PLIC, all routed to cpu0):\n");
    print("  source              count    time(us)  handler\n");
    for (int i = 0; i < MAX_IRQS; i++) {
        if (!irqs[i].handler)
            continue;
        char s[96];
        ksym_format(s, sizeof(s), (uintptr_t)irqs[i].handler);
        print("  plic %-3d %-6s %10lu %10lu  %s\n", i, irqs[i].name, (unsigned long)irqs[i].count,
              (unsigned long)(irqs[i].time * 1000000 / tb), s);
    }
    print("  spurious PLIC claims: %lu\n", (unsigned long)spurious);
}

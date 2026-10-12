#ifndef EUNUCHS_RISCV_H
#define EUNUCHS_RISCV_H

#include <types.h>

#define csr_read(csr) ({                                   \
    unsigned long __v;                                     \
    asm volatile("csrr %0, " #csr : "=r"(__v) :: "memory"); \
    __v;                                                   \
})
#define csr_write(csr, val) \
    asm volatile("csrw " #csr ", %0" :: "rK"((unsigned long)(val)) : "memory")
#define csr_set(csr, val) \
    asm volatile("csrs " #csr ", %0" :: "rK"((unsigned long)(val)) : "memory")
#define csr_clear(csr, val) \
    asm volatile("csrc " #csr ", %0" :: "rK"((unsigned long)(val)) : "memory")

/* sstatus */
#define SSTATUS_SIE   (1UL << 1)
#define SSTATUS_SPIE  (1UL << 5)
#define SSTATUS_SPP   (1UL << 8)
#define SSTATUS_SUM   (1UL << 18)
#define SSTATUS_FS    (3UL << 13)      /* FP state: 0 off, 1 initial, 2 clean, 3 dirty */
#define SSTATUS_FS_INITIAL (1UL << 13)

/* sie / sip */
#define SIE_SSIE      (1UL << 1)
#define SIE_STIE      (1UL << 5)
#define SIE_SEIE      (1UL << 9)

/* scause */
#define SCAUSE_INTR   (1UL << 63)
#define SCAUSE_CODE(c) ((c) & ~SCAUSE_INTR)

enum exc_code {
    EXC_INSN_MISALIGNED  = 0,
    EXC_INSN_ACCESS      = 1,
    EXC_ILLEGAL_INSN     = 2,
    EXC_BREAKPOINT       = 3,
    EXC_LOAD_MISALIGNED  = 4,
    EXC_LOAD_ACCESS      = 5,
    EXC_STORE_MISALIGNED = 6,
    EXC_STORE_ACCESS     = 7,
    EXC_ECALL_U          = 8,
    EXC_ECALL_S          = 9,
    EXC_INSN_PAGE_FAULT  = 12,
    EXC_LOAD_PAGE_FAULT  = 13,
    EXC_STORE_PAGE_FAULT = 15,
    EXC_SW_CHECK         = 18,
    EXC_HW_ERROR         = 19,
    EXC_MAX              = 20,
};

enum irq_code {
    IRQ_S_SOFT  = 1,
    IRQ_S_TIMER = 5,
    IRQ_S_EXT   = 9,
    IRQ_COUNTER = 13,
    IRQ_MAX     = 16,
};

static inline u64 rdtime(void)
{
    u64 t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

static inline void intr_off(void) { csr_clear(sstatus, SSTATUS_SIE); }
static inline void intr_on(void)  { csr_set(sstatus, SSTATUS_SIE); }
static inline bool intr_enabled(void) { return csr_read(sstatus) & SSTATUS_SIE; }

/* Sleep (wfi) for up to `ticks` timer ticks; may wake early. idle.c */
void cpu_wait_ticks(u64 ticks);

/* The caller's frame pointer. A macro: as an inline function it would be
   its own frame under a compiler that does not inline (tcc). */
#define current_fp() ((uintptr_t)__builtin_frame_address(0))

#endif

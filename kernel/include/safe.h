#ifndef EUNUCHS_SAFE_H
#define EUNUCHS_SAFE_H

/*
 * Fault-tolerant memory access for debugging tools. Each returns 0 on success
 * or -1 if the access faulted. Implemented in safe.S using the exception
 * table: the trap handler redirects a faulting access to its fixup label.
 */

#include <types.h>

int safe_read_u8(uintptr_t addr, u8 *out);
int safe_read_u16(uintptr_t addr, u16 *out);
int safe_read_u32(uintptr_t addr, u32 *out);
int safe_read_u64(uintptr_t addr, u64 *out);
int safe_write_u8(uintptr_t addr, u8 val);
int safe_write_u16(uintptr_t addr, u16 val);
int safe_write_u32(uintptr_t addr, u32 val);
int safe_write_u64(uintptr_t addr, u64 val);

struct extable_entry {
    uintptr_t insn;
    uintptr_t fixup;
};

uintptr_t extable_lookup(uintptr_t pc);   /* fixup address or 0 */

#endif

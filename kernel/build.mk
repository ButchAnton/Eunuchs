# The kernel, built on Eunuchs by tcc from this source tree (shared from the
# Mac on /mnt/host) and linked by kld with kernel/arch/riscv/kernel.ld. Run
# after user/build.mk, which builds kld. `make selfhost` on the Mac runs both
# twice and boots what they built; `make tcc-kernel` runs this one on the Mac
# (with the cross tcc and the host kld).
#
#   cd /mnt/host && make -f kernel/build.mk [OUT=dir] [TCC=compiler] [KLD=linker]
#
# OUT/kernel/kernel.elf   the kernel (with its symbol table: kld --ksyms)
# OUT/kernel/eboot.elf    the boot loader (boot/), which loads /boot/kernel.elf

TCC     = tcc
OUT     = build/selfhost
KLD     = $(OUT)/bin/kld
KOUT    = $(OUT)/kernel
TCCSRC  = third_party/tinycc

# No floating point and no C library: the kernel's headers, tcc's own
# (stdatomic.h, stddef.h, ...), and tcc's runtime for atomics and builtins.
CFLAGS  = -Wall -nostdinc -Ikernel/include -Ilib/include -I$(TCCSRC)/include
CC      = $(TCC) $(CFLAGS)

KDIRS   := kernel/arch/riscv kernel/core kernel/mm kernel/proc kernel/fs kernel/drivers \
           kernel/net kernel/debug kernel/tests lib
KSRC_S  := $(sort $(foreach d,$(KDIRS),$(wildcard $(d)/*.S)))
KSRC_C  := $(sort $(foreach d,$(KDIRS),$(wildcard $(d)/*.c)))
KOBJS   := $(patsubst %.S,$(KOUT)/%.o,$(KSRC_S)) $(patsubst %.c,$(KOUT)/%.o,$(KSRC_C))
RT_O    := $(patsubst %,$(KOUT)/rt/%.o,atomic stdatomic builtin)
LDSCRIPT = kernel/arch/riscv/kernel.ld

all: $(KOUT)/kernel.elf $(KOUT)/eboot.elf
	@echo "kernel and boot loader built in $(KOUT)"

$(KOUT)/kernel.elf: $(KOBJS) $(RT_O) $(LDSCRIPT) $(KLD)
	$(KLD) -T $(LDSCRIPT) -o $@ --ksyms $(KOBJS) $(RT_O)

EBOOT_O := $(KOUT)/boot/start.o $(KOUT)/boot/eboot.o

$(KOUT)/eboot.elf: $(EBOOT_O) boot/eboot.ld $(KLD)
	$(KLD) -T boot/eboot.ld -o $@ $(EBOOT_O)

$(KOUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

$(KOUT)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -D__ASSEMBLY__ -c -o $@ $<

$(KOUT)/rt/%.o: $(TCCSRC)/lib/%.c
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

$(KOUT)/rt/%.o: $(TCCSRC)/lib/%.S
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

clean:
	rm -rf $(KOUT)

.PHONY: all clean

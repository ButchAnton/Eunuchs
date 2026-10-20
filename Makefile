# Eunuchs — top-level build
#
#   make run        boot in QEMU (Ctrl-^ X quits QEMU)
#   make test       boot headless, run in-kernel tests, exit with pass/fail
#   make test-host  run host-native unit tests (ASan/UBSan)
#   make debug      boot halted, waiting for GDB on :1234
#   make gdb        attach riscv64-elf-gdb to a `make debug` session
#   make qemu-log   run with QEMU tracing of traps/guest errors -> build/qemu.log
#
# Kernel options go in APPEND, e.g.  make run APPEND="log=all:debug break"

CROSS   ?= riscv64-elf-
CC      := $(CROSS)gcc
NM      := $(CROSS)nm
OBJDUMP := $(CROSS)objdump
GDB     := $(CROSS)gdb
QEMU    ?= qemu-system-riscv64
PYTHON  ?= python3
HOSTCC  ?= clang

CPUS    ?= 4
MEM     ?= 128M
APPEND  ?=
TEST_TIMEOUT ?= 60

BUILD   := build

# Terse output; `make V=1` shows full commands.
ifeq ($(V),1)
Q :=
msg = @true
else
Q := @
msg = @printf '  %-7s %s\n' $(1) $(2)
endif

# The kernel uses no floating point (the FP registers belong to user threads).
CFLAGS  := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib -fno-pic -fno-stack-protector \
           -fno-omit-frame-pointer -g -O1 -Wall -Wextra -Werror -std=gnu11 \
           -Ikernel/include -Ilib/include
ASFLAGS := $(CFLAGS) -D__ASSEMBLY__
LDSCRIPT := kernel/arch/riscv/kernel.ld
LDFLAGS := -nostdlib -Wl,--no-warn-rwx-segments -T $(LDSCRIPT)

KDIRS   := kernel/arch/riscv kernel/core kernel/mm kernel/proc kernel/fs kernel/drivers \
           kernel/net kernel/debug kernel/tests lib
KSRCS_C := $(foreach d,$(KDIRS),$(wildcard $(d)/*.c))
KSRCS_S := $(foreach d,$(KDIRS),$(wildcard $(d)/*.S))
KOBJS   := $(patsubst %.S,$(BUILD)/%.o,$(KSRCS_S)) $(patsubst %.c,$(BUILD)/%.o,$(KSRCS_C))

KERNEL  := $(BUILD)/kernel.elf
PASS1   := $(BUILD)/kernel.pass1.elf

# --- User space: RV64GC with hardware floating point (lp64d), our own headers only ---
GCC_INC := $(shell $(CC) -print-file-name=include)
UCFLAGS := -march=rv64gc -mabi=lp64d -mcmodel=medany \
           -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib \
           -nostdinc -isystem $(GCC_INC) -fno-pic -fno-stack-protector \
           -fno-omit-frame-pointer -g -O1 -Wall -Wextra -Werror -std=gnu11 \
           -Iuser/include -Ilib/include -DFMT_FLOAT
# libc: crt0 first, then every other user/lib source, and the printf and
# string code shared with the kernel.
ULIB    := $(BUILD)/user/lib/crt0.o \
           $(patsubst user/lib/%.c,$(BUILD)/user/lib/%.o,$(wildcard user/lib/*.c)) \
           $(patsubst user/lib/%.S,$(BUILD)/user/lib/%.o,$(filter-out user/lib/crt0.S,$(wildcard user/lib/*.S))) \
           $(BUILD)/user/lib/printf.o $(BUILD)/user/lib/string.o
UPROGS  := $(patsubst user/bin/%.c,$(BUILD)/user/bin/%,$(wildcard user/bin/*.c))
# Programs spread over several files: user/bin/NAME/*.c -> /bin/NAME
UMULTI  := $(notdir $(patsubst %/,%,$(dir $(wildcard user/bin/*/*.c))))
UMULTI  := $(sort $(UMULTI))
UPROGS  += $(addprefix $(BUILD)/user/bin/,$(UMULTI))
UMULTI_OBJS := $(patsubst user/bin/%.c,$(BUILD)/user/multi/%.o,$(wildcard user/bin/*/*.c))
# --- Disk image: a GPT disk with an efs filesystem in each partition, made by
# eudisk (tools/eudisk): boot and boot.old (a kernel each, mounted on /boot
# and /boot.old), root (user programs, sources, a few directories), home, and
# swap space, where the kernel puts user pages when memory runs short.
# build/fs.img is made afresh for the tests; disk.img, which make run uses, is
# made once and then updated in place (your files stay: see eudisk install).
MKFS    := $(BUILD)/tools/mkfs
EUDISK  := $(BUILD)/tools/eudisk
EFS_LIB := tools/efs/efs.c tools/efs/image.c
EFS_HDRS := tools/efs/efs.h tools/efs/image.h kernel/include/efs_format.h kernel/include/gpt_format.h
# the kernel linker, for tcc's kernel (tools/kld)
KLD     := $(BUILD)/tools/kld
FSIMG   := $(BUILD)/fs.img
DISK_IMG ?= disk.img
BOOT_MB ?= 16
FS_MB   ?= 64
HOME_MB ?= 64
SWAP_MB ?= 128
DISK_LAYOUT = boot:$(BOOT_MB) boot.old:$(BOOT_MB) root:$(FS_MB) home:$(HOME_MB) swap:$(SWAP_MB)
HOME_FILES  = /guest/:0755:1000:1000
EUTILS_LINKS = echo mkdir rm rmdir touch ln mv cp wc stat head tail tee chmod chown env id crc32 profile \
               true false yes kill date dmesg free sleep strace su dbg uname clear \
               reboot poweroff
FS_FILES = $(foreach p,$(UPROGS),/bin/$(notdir $(p))=$(BUILD)/user/stripped/$(notdir $(p))) \
           $(foreach l,$(EUTILS_LINKS),/bin/$(l)=@/bin/eutils) /bin/sh=@/bin/esh /bin/less=@/bin/more \
           /bin/vi=@/bin/edit /bin/view=@/bin/edit /bin/emacs=@/bin/edit /bin/em=@/bin/edit \
           +/etc/motd=user/etc/motd +/etc/passwd=user/etc/passwd +/etc/group=user/etc/group \
           +/etc/hostname=user/etc/hostname +/etc/eshrc=user/etc/eshrc /dev/ /proc/ /tmp/:0777 \
           /www/ /www/index.html=user/www/index.html \
           /home/ /home/guest/:0755:1000:1000 /mnt/ /mnt/host/ \
           $(foreach f,$(SYS_FILES),$(patsubst $(SYSROOT)%,%,$(f))=$(f)) \
           $(foreach f,$(filter user/bin/%,$(USR_SRC)),/usr/src/$(patsubst user/bin/%,%,$(f))=$(f)) \
           /usr/src/Makefile=user/usr-src/Makefile \
           /boot/ /boot.old/ \
           $(foreach f,$(EU_SRC),/usr/src/eunuchs/$(f)=$(f))
DISKOPTS = -global virtio-mmio.force-legacy=false \
           -drive file=$(1),if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0

# -nographic muxes serial + monitor. QEMU's default escape is Ctrl-A (1),
# which is beginning-of-line in emacs and the shell. Ctrl-^ (30) is unused
# by either; Ctrl-^ X still quits QEMU, Ctrl-^ Ctrl-^ sends a literal Ctrl-^.
QEMU_ECHR ?= 30
# QEMU starts the boot loader, which loads /boot/kernel.elf from the root
# disk (boot/eboot.c); BOOT=direct has QEMU load build/kernel.elf instead.
BOOT    ?= eboot
EBOOT   := $(BUILD)/eboot.elf
BOOT_IMAGE = $(if $(filter direct,$(BOOT)),$(KERNEL),$(EBOOT))
QEMUBASE = -machine virt -bios default -nographic -echr $(QEMU_ECHR) -smp $(CPUS) -m $(MEM) \
           -semihosting-config enable=on,target=native -kernel $(BOOT_IMAGE)

# Networking: QEMU's user-mode network (guest 10.0.2.15, host 10.0.2.2).
# Host ports forward to the guest: HTTP_PORT -> 80 (httpd), SHELL_PORT -> 23
# (rshd), UDP_PORT -> 7 (UDP echo). Every run records the traffic in
# build/net.pcap (open it in Wireshark).
HTTP_PORT  ?= 8080
SHELL_PORT ?= 2323
UDP_PORT   ?= 5555
HOSTFWD  ?= hostfwd=tcp:127.0.0.1:$(HTTP_PORT)-:80,hostfwd=tcp:127.0.0.1:$(SHELL_PORT)-:23,hostfwd=udp:127.0.0.1:$(UDP_PORT)-:7
NETOPTS  = -netdev user,id=net0$(if $(HOSTFWD),$(comma)$(HOSTFWD)) \
           -device virtio-net-device,netdev=net0 \
           -object filter-dump,id=pcap0,netdev=net0,file=$(BUILD)/net.pcap
comma := ,
# make run/debug also share a Mac directory (the project, unless HOSTDIR=...;
# HOSTDIR= shares nothing) through virtio-9p; the kernel mounts it on
# /mnt/host. It is the real directory: what the guest writes there changes
# the Mac's files.
HOSTDIR  ?= $(CURDIR)
SHAREOPTS = $(if $(HOSTDIR),-fsdev local$(comma)id=hostfs$(comma)path=$(HOSTDIR)$(comma)security_model=none \
            -device virtio-9p-device$(comma)fsdev=hostfs$(comma)mount_tag=host)
QEMUOPTS = $(QEMUBASE) $(call DISKOPTS,$(DISK_IMG)) $(SHAREOPTS) $(NETOPTS)

.PHONY: all user fsck gui run debug gdb test selfhost tcc-kernel stress test-host trace-json flamegraph record replay replay-debug replay-gdb replay-check ci qemu-log disasm clean sysroot

# Written on Apple Silicon. A different host (Intel Mac) or Homebrew GCC
# can change RISC-V ELF ABI tags; .S files have no header deps, so they
# would otherwise stay stale and the kernel link fails (soft- vs double-float).
# Updated at parse time so GNU Make 3.81 sees a newer file before it walks
# the graph (a FORCE recipe can rewrite the stamp in the same second and
# then skip dependents).
CC_ID := $(BUILD)/.cc-id
$(shell mkdir -p $(BUILD) && printf '%s\n' "$(shell uname -m)" "$(CC)" "$(CFLAGS)" "$(UCFLAGS)" "$(shell $(CC) --version 2>/dev/null | head -1)" "$(shell $(HOSTCC) --version 2>/dev/null | head -1)" > $(CC_ID).tmp && { cmp -s $(CC_ID) $(CC_ID).tmp || mv $(CC_ID).tmp $(CC_ID); } && rm -f $(CC_ID).tmp)

all: $(KERNEL) $(FSIMG) $(KLD) $(EBOOT) $(EUDISK)

# --- The boot loader (boot/): QEMU's -kernel, which loads /boot/kernel.elf ---
EBOOT_OBJS := $(BUILD)/boot/start.o $(BUILD)/boot/eboot.o

$(EBOOT): $(EBOOT_OBJS) boot/eboot.ld
	$(call msg,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(filter-out -T $(LDSCRIPT),$(LDFLAGS)) -T boot/eboot.ld -o $@ $(EBOOT_OBJS)

# The kernel on the root disk: without DWARF (GDB reads build/kernel.elf)
$(BUILD)/kernel.boot: $(KERNEL)
	$(Q)$(CROSS)objcopy --strip-debug $< $@

# --- Two-pass link: pass 1 has an empty symbol table; its symbols generate
# --- the real table for pass 2. .ksyms is last in the image, so no other
# --- address moves; we verify that anyway.
$(BUILD)/ksyms0.S: tools/gen_ksyms.py
	@mkdir -p $(BUILD)
	$(Q)$(PYTHON) tools/gen_ksyms.py < /dev/null > $@

$(PASS1): $(KOBJS) $(BUILD)/ksyms0.o $(LDSCRIPT)
	$(call msg,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KOBJS) $(BUILD)/ksyms0.o

$(BUILD)/ksyms.S: $(PASS1) tools/gen_ksyms.py
	$(call msg,KSYMS,$@)
	$(Q)$(NM) -n $(PASS1) | $(PYTHON) tools/gen_ksyms.py > $@

$(KERNEL): $(KOBJS) $(BUILD)/ksyms.o $(LDSCRIPT)
	$(call msg,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KOBJS) $(BUILD)/ksyms.o
	@$(NM) -n $(PASS1) | grep -i ' t ' > $(BUILD)/pass1.syms
	@$(NM) -n $@ | grep -i ' t ' > $(BUILD)/pass2.syms
	@cmp -s $(BUILD)/pass1.syms $(BUILD)/pass2.syms || \
		(echo "error: text symbols moved between link passes" && false)

$(BUILD)/ksyms0.o $(BUILD)/ksyms.o: $(BUILD)/%.o: $(BUILD)/%.S
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

# --- User programs and the initrd (rules before the generic kernel ones) ---

$(BUILD)/user/lib/printf.o: lib/printf.c
	@mkdir -p $(dir $@)
	$(call msg,UCC,$<)
	$(Q)$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/lib/string.o: lib/string.c
	@mkdir -p $(dir $@)
	$(call msg,UCC,$<)
	$(Q)$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(call msg,UCC,$<)
	$(Q)$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/user/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(call msg,UAS,$<)
	$(Q)$(CC) $(UCFLAGS) -D__ASSEMBLY__ -c $< -o $@

$(BUILD)/user/bin/%: $(BUILD)/user/bin/%.o $(ULIB) user/user.ld
	$(call msg,ULD,$@)
	$(Q)$(CC) $(UCFLAGS) -nostdlib -static -T user/user.ld -o $@ $< $(ULIB) -lgcc

$(BUILD)/user/multi/%.o: user/bin/%.c
	@mkdir -p $(dir $@)
	$(call msg,UCC,$<)
	$(Q)$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# A multi-file program links every object in its directory.
define MULTI_RULE
$(BUILD)/user/bin/$(1): $(patsubst user/bin/$(1)/%.c,$(BUILD)/user/multi/$(1)/%.o,$(wildcard user/bin/$(1)/*.c)) $(ULIB) user/user.ld
	$$(call msg,ULD,$$@)
	$$(Q)$$(CC) $$(UCFLAGS) -nostdlib -static -T user/user.ld -o $$@ $$(filter %.o,$$^) -lgcc
endef
$(foreach p,$(UMULTI),$(eval $(call MULTI_RULE,$(p))))

# --- TinyCC: a C compiler that runs on Eunuchs ---
# /bin/tcc is built by gcc against our libc. The host cross compiler
# build/tcc/xtcc (same configuration) builds tcc's runtime library. The
# "sysroot" is what goes into the image: /usr/include (our headers),
# /usr/lib/{crt1.o,crti.o,crtn.o,libc.a} and /usr/lib/tcc/{include,libtcc1.a}.
TCC_SRC   := third_party/tinycc
TCC_DEPS  := $(wildcard $(TCC_SRC)/*.c $(TCC_SRC)/*.h) ports/tcc/config.h
XTCC      := $(BUILD)/tcc/xtcc
SYSROOT   := $(BUILD)/sysroot
LIBGCC    := $(shell $(CC) -march=rv64gc -mabi=lp64d -print-libgcc-file-name)
TCC_RT    := $(addprefix $(BUILD)/tcc/rt/,stdatomic.o atomic.o builtin.o alloca.o armflush.o)
SYS_HDRS  := $(patsubst user/include/%,$(SYSROOT)/usr/include/%,$(shell find user/include -name '*.h')) \
             $(patsubst lib/include/%,$(SYSROOT)/usr/include/%,$(shell find lib/include -name '*.h')) \
             $(patsubst $(TCC_SRC)/include/%,$(SYSROOT)/usr/lib/tcc/include/%,$(wildcard $(TCC_SRC)/include/*.h))
SYS_LIBS  := $(addprefix $(SYSROOT)/usr/lib/,crt1.o crti.o crtn.o libc.a tcc/libtcc1.a tcc/runmain.o)
# /usr/src: the programs' sources and a Makefile to rebuild them with tcc
USR_SRC   := $(wildcard user/bin/*.c user/bin/*/*.c user/bin/*/*.h) user/usr-src/Makefile
# /usr/src/eunuchs: the source tree Eunuchs builds itself from (user/build.mk,
# kernel/build.mk, tools/selfhost.sh), so it can without the Mac's share
EU_SRC    := $(shell find boot kernel lib user/include user/lib user/bin -type f \( -name '*.[chS]' -o -name '*.ld' \) | sort) \
             kernel/build.mk user/build.mk ports/tcc/config.h tools/kld/kld.c tools/selfhost.sh \
             $(wildcard $(TCC_SRC)/*.c $(TCC_SRC)/*.h $(TCC_SRC)/*.def $(TCC_SRC)/include/*.h \
                        $(TCC_SRC)/lib/*.c $(TCC_SRC)/lib/*.S) $(TCC_SRC)/COPYING
SYS_FILES := $(SYS_HDRS) $(SYS_LIBS)
UPROGS    += $(BUILD)/user/bin/tcc

$(XTCC): $(TCC_DEPS)
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) -O1 -w -DONE_SOURCE=1 -Iports/tcc -o $@ $(TCC_SRC)/tcc.c

$(BUILD)/tcc/tcc.o: $(TCC_DEPS)
	@mkdir -p $(dir $@)
	$(call msg,UCC,$(TCC_SRC)/tcc.c)
	$(Q)$(CC) $(UCFLAGS) -w -DONE_SOURCE=1 -Iports/tcc -c $(TCC_SRC)/tcc.c -o $@

$(BUILD)/user/bin/tcc: $(BUILD)/tcc/tcc.o $(ULIB) user/user.ld
	$(call msg,ULD,$@)
	$(Q)$(CC) $(UCFLAGS) -nostdlib -static -T user/user.ld -o $@ $< $(ULIB) -lgcc

$(BUILD)/tcc/rt/%.o: $(TCC_SRC)/lib/%.c $(XTCC)
	@mkdir -p $(dir $@)
	$(call msg,XTCC,$<)
	$(Q)$(XTCC) -B$(TCC_SRC) -Iuser/include -Ilib/include -c $< -o $@

$(BUILD)/tcc/rt/%.o: $(TCC_SRC)/lib/%.S $(XTCC)
	@mkdir -p $(dir $@)
	$(call msg,XTCC,$<)
	$(Q)$(XTCC) -B$(TCC_SRC) -Iuser/include -Ilib/include -c $< -o $@

# libtcc1.a: tcc's helpers plus libgcc (soft long double, used by libc too).
$(SYSROOT)/usr/lib/tcc/libtcc1.a: $(TCC_RT) $(LIBGCC)
	@mkdir -p $(dir $@) $(BUILD)/tcc/libgcc
	$(call msg,AR,$@)
	$(Q)cd $(BUILD)/tcc/libgcc && rm -f *.o && $(CROSS)ar x $(LIBGCC)
	$(Q)rm -f $@ && $(CROSS)ar rcs $@ $(TCC_RT) $(BUILD)/tcc/libgcc/*.o && $(CROSS)strip -g $@

$(SYSROOT)/usr/lib/tcc/runmain.o: $(BUILD)/tcc/rt/runmain.o
	@mkdir -p $(dir $@)
	$(Q)cp $< $@

$(SYSROOT)/usr/lib/libc.a: $(ULIB)
	@mkdir -p $(dir $@)
	$(call msg,AR,$@)
	$(Q)rm -f $@ && $(CROSS)ar rcs $@ $(filter-out %/crt0.o,$(ULIB)) && $(CROSS)strip -g $@

$(SYSROOT)/usr/lib/crt1.o: user/lib/crt0.S
	@mkdir -p $(dir $@)
	$(call msg,UAS,$@)
	$(Q)$(CC) $(UCFLAGS) -D__ASSEMBLY__ -DNO_GP -c $< -o $@

$(SYSROOT)/usr/lib/crti.o $(SYSROOT)/usr/lib/crtn.o:
	@mkdir -p $(dir $@)
	$(Q)echo "" | $(CC) $(UCFLAGS) -x assembler -c - -o $@

$(SYSROOT)/usr/include/%: user/include/%
	@mkdir -p $(dir $@)
	$(Q)cp $< $@
$(SYSROOT)/usr/include/%: lib/include/%
	@mkdir -p $(dir $@)
	$(Q)cp $< $@
$(SYSROOT)/usr/lib/tcc/include/%: $(TCC_SRC)/include/%
	@mkdir -p $(dir $@)
	$(Q)cp $< $@

sysroot: $(SYS_FILES)

# The disk image gets copies without DWARF (symbol tables stay, for the
# kernel's user backtraces); the full binaries in build/user/bin are for GDB.
$(BUILD)/user/stripped/%: $(BUILD)/user/bin/%
	@mkdir -p $(dir $@)
	$(Q)$(CROSS)objcopy --strip-debug $< $@

# mkfs (a bare efs filesystem; fsck) and eudisk, on the host efs library
$(MKFS): tools/mkfs/mkfs.c $(EFS_LIB) $(EFS_HDRS)
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) -O1 -g -Wall -Wextra -Werror -Ikernel/include -Itools/efs -o $@ $< $(EFS_LIB)

$(EUDISK): tools/eudisk/eudisk.c $(EFS_LIB) $(EFS_HDRS)
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) -O1 -g -Wall -Wextra -Werror -Ikernel/include -Itools/efs -o $@ $< $(EFS_LIB)

# $(call mkdisk,IMAGE,KERNEL,ROOT_FILES): a new disk image, built beside
# IMAGE and renamed over it when complete.
mkdisk = rm -f $(1).new && $(EUDISK) create $(1).new $(DISK_LAYOUT) > /dev/null && \
	$(EUDISK) install $(1).new root $(3) > /dev/null && \
	$(EUDISK) install $(1).new home $(HOME_FILES) > /dev/null && \
	$(EUDISK) kernel install --to boot $(1).new $(2) > /dev/null && \
	$(EUDISK) kernel install --to boot.old $(1).new $(2) > /dev/null && \
	mv -f $(1).new $(1)

DISK_DEPS = $(EUDISK) $(BUILD)/kernel.boot $(patsubst $(BUILD)/user/bin/%,$(BUILD)/user/stripped/%,$(UPROGS)) \
            user/etc/* user/www/* $(SYS_FILES) $(USR_SRC) $(EU_SRC)

# The tests' image: made afresh whenever anything in it changes.
$(FSIMG): $(DISK_DEPS)
	$(call msg,DISK,$@)
	$(Q)$(call mkdisk,$@,$(BUILD)/kernel.boot,$(FS_FILES))

# Yours: made once (with what was in home.img, the old /home disk, if it is
# there), then brought up to date in place. Programs and sources that changed
# are replaced (copies of any you edited go to /var/backup), configuration
# files you edited stay (the new ones arrive as FILE.new), and a new kernel
# goes to boot with the one it replaces in boot.old. `make clean` leaves it;
# rm disk.img for a fresh one.
$(DISK_IMG): $(DISK_DEPS)
	@if [ ! -f $@ ]; then \
		echo "  DISK    $@ (new)"; \
		$(call mkdisk,$@,$(BUILD)/kernel.boot,$(FS_FILES)) || exit 1; \
		if [ -f home.img ]; then \
			if $(EUDISK) copy home.img:/ $@:home:/; then \
				echo "  (home.img's files are in $@'s home partition: home.img can go)"; \
			else echo "  (some of home.img was not copied: see above; eudisk copy home.img:/ $@:home:/ retries)"; fi; \
		fi; \
	else \
		echo "  DISK    $@ (update)"; \
		$(EUDISK) info --json $@ | grep -q '"swap":true' || $(EUDISK) add $@ swap:$(SWAP_MB) || exit 1; \
		$(EUDISK) install $@ root $(FS_FILES) && \
		$(EUDISK) install $@ home $(HOME_FILES) > /dev/null && \
		$(EUDISK) kernel install $@ $(BUILD)/kernel.boot || exit 1; \
	fi
	@touch $@

# Eudisk, the Mac app (tools/eudisk-gui, SwiftUI over eudisk), on disk.img
# (or IMAGE=...). It only reads while QEMU has the image open.
IMAGE ?= $(DISK_IMG)
gui: $(EUDISK)
	$(call msg,SWIFT,$(BUILD)/gui/release/Eudisk)
	@cd tools/eudisk-gui && swift build -q -c release --scratch-path $(CURDIR)/$(BUILD)/gui
	@EUDISK=$(CURDIR)/$(EUDISK) $(BUILD)/gui/release/Eudisk $(abspath $(IMAGE)) > /dev/null 2>&1 &

fsck: $(MKFS)
	$(MKFS) --check $(FSIMG)
	@if [ -f $(DISK_IMG) ]; then $(MKFS) --check $(DISK_IMG); fi

user: $(UPROGS)

.SECONDARY:     # keep intermediate objects (faster rebuilds)

# --- Kernel objects ---

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(call msg,CC,$<)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(call msg,AS,$<)
	$(Q)$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

# --- Running ---

# The console needs a raw terminal on stdout. Wrappers that pipe make's output
# (e.g. grc's `make` shell function) line-buffer it: typed characters and prompts then
# appear only after Return. Bypass with `command make run`.
define check_tty
	@if [ -t 0 ] && [ ! -t 1 ]; then \
		echo "warning: stdout is not a terminal (is make wrapped by grc or a pager?)." >&2; \
		echo "warning: console echo will be line-buffered; use 'command make $@'." >&2; \
	fi
endef

run: $(KERNEL) $(EBOOT) $(DISK_IMG)
	$(check_tty)
	$(QEMU) $(QEMUOPTS) -append "$(APPEND)"

debug: $(KERNEL) $(EBOOT) $(DISK_IMG)
	$(check_tty)
	@echo "QEMU halted; run 'make gdb' in another terminal."
	$(QEMU) $(QEMUOPTS) -append "$(APPEND)" -s -S

gdb: $(KERNEL)
	$(GDB) -q -x tools/gdb/init.gdb $(KERNEL)

# --- Deterministic record/replay (QEMU -icount rr) ---
# `make record` runs like `make run`, but QEMU logs every nondeterministic
# input (console keystrokes, disk completions, network packets, timer
# interrupts). `make replay` re-executes exactly the same run, instruction for
# instruction: the console output repeats byte for byte, timestamps included.
# `make replay-debug` + `make replay-gdb` replay under GDB with reverse
# execution (reverse-continue, reverse-step, reverse-stepi...).
# QEMU supports this with one CPU only. The recording keeps its own copies
# of the kernel, the disk (a qcow2 overlay holding the starting snapshot) and
# the boot options in build/rr/, so rebuilding does not invalidate it. Under
# -icount idle time passes in real time: disk-heavy runs are slower than usual.
RRDIR    := $(BUILD)/rr
RROPTS = -machine virt -bios default -nographic -echr $(QEMU_ECHR) -smp 1 -m $(MEM) -kernel $(RRDIR)/kernel.elf \
         -icount shift=auto,rr=$(1),rrfile=$(RRDIR)/replay.bin,rrsnapshot=init \
         -drive file=$(RRDIR)/snap.qcow2,if=none,id=rr \
         -global virtio-mmio.force-legacy=false \
         -drive file=$(RRDIR)/fs.qcow2,if=none,id=img-direct \
         -drive driver=blkreplay,if=none,image=img-direct,id=img-blkreplay \
         -device virtio-blk-device,drive=img-blkreplay \
         -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
         -object filter-replay,id=replay,netdev=net0
define rr_prepare
	@mkdir -p $(RRDIR)
	@rm -f $(RRDIR)/fs.qcow2 $(RRDIR)/snap.qcow2 $(RRDIR)/replay.bin
	@cp -f $(KERNEL) $(RRDIR)/kernel.elf
	@cp -f $(FSIMG) $(RRDIR)/base.img
	@qemu-img create -q -f qcow2 -b base.img -F raw $(RRDIR)/fs.qcow2
	@qemu-img create -q -f qcow2 $(RRDIR)/snap.qcow2 64M
	@echo '$(1)' > $(RRDIR)/append
endef
define rr_check
	@test -f $(RRDIR)/replay.bin || { echo "no recording: run 'make record' first"; exit 1; }
endef

record: $(KERNEL) $(FSIMG)
	$(check_tty)
	$(call rr_prepare,$(APPEND))
	@echo "recording to $(RRDIR) (one CPU); replay with 'make replay'"
	$(QEMU) $(call RROPTS,record) -append "$(APPEND)"

replay:
	$(rr_check)
	@echo "replaying $(RRDIR) (keystrokes come from the recording)"
	$(QEMU) $(call RROPTS,replay) -append "$$(cat $(RRDIR)/append)"

replay-debug:
	$(rr_check)
	@echo "replay halted at the start; run 'make replay-gdb' in another terminal"
	$(QEMU) $(call RROPTS,replay) -append "$$(cat $(RRDIR)/append)" -s -S

replay-gdb:
	$(GDB) -q -x tools/gdb/init.gdb $(RRDIR)/kernel.elf

# Record a headless boot (the kernel tests), replay it, and require identical
# console output. Part of make ci.
replay-check: $(KERNEL) $(FSIMG)
	$(call rr_prepare,panic=shutdown ktest)
	@$(PYTHON) tools/timeout.py 300 $(QEMU) $(call RROPTS,record) -append "panic=shutdown ktest" \
		< /dev/null > $(RRDIR)/record.log 2>&1
	@$(PYTHON) tools/timeout.py 300 $(QEMU) $(call RROPTS,replay) -append "panic=shutdown ktest" \
		< /dev/null > $(RRDIR)/replay.log 2>&1
	@grep -q "KTEST RESULT: PASS" $(RRDIR)/record.log || { echo "replay-check: recorded run failed"; exit 1; }
	@cmp -s $(RRDIR)/record.log $(RRDIR)/replay.log && \
		echo "replay-check: replay identical to the recording ($$(wc -l < $(RRDIR)/record.log | tr -d ' ') lines, $$(du -h $(RRDIR)/replay.bin | cut -f1) of events)" || \
		{ echo "replay-check: replay DIVERGED from the recording:"; diff $(RRDIR)/record.log $(RRDIR)/replay.log | head -20; exit 1; }

qemu-log: $(KERNEL) $(DISK_IMG)
	$(check_tty)
	$(QEMU) $(QEMUOPTS) -append "$(APPEND)" -d int,guest_errors,unimp -D $(BUILD)/qemu.log

disasm: $(KERNEL)
	$(OBJDUMP) -dS $(KERNEL) > $(BUILD)/kernel.asm

# --- Testing ---

# tools/timeout.py kills QEMU if a test run hangs.
# The whole scenario: kernel + user tests, fsck, persistence across a reboot,
# a simulated power failure mid-commit, log recovery, fsck. See tools/run_tests.py.
test: $(KERNEL) $(EBOOT) $(FSIMG) $(MKFS)
	@$(PYTHON) tools/run_tests.py --qemu "$(QEMU) $(QEMUBASE)" --image $(FSIMG) \
		--mkfs $(MKFS) --timeout $(TEST_TIMEOUT) --append "$(APPEND)"

# Self-hosting: Eunuchs builds itself with tcc from this tree (shared on
# /mnt/host): user space (user/build.mk), then the kernel (kernel/build.mk,
# linked by kld), twice: with the image's tcc, then with the tcc that built
# (tools/selfhost.py; the stages must be identical). Then the test suite
# boots the kernel it built, with an image made from the rest.
SH_OUT   := $(BUILD)/selfhost/stage2
SH_KERNEL := $(SH_OUT)/kernel/kernel.elf
SH_EBOOT := $(SH_OUT)/kernel/eboot.elf
SH_LIBS  := crt1.o crti.o crtn.o libc.a tcc/libtcc1.a tcc/runmain.o
sh_file   = $(if $(findstring =$(BUILD)/user/stripped/,$(1)),$(subst =$(BUILD)/user/stripped/,=$(SH_OUT)/bin/,$(1)),$(if $(filter $(addprefix %/,$(SH_LIBS)),$(1)),$(subst =$(SYSROOT)/usr/lib/,=$(SH_OUT)/sysroot/usr/lib/,$(1)),$(1)))
SH_FILES  = $(foreach f,$(FS_FILES),$(call sh_file,$(f))) /bin/kld=$(SH_OUT)/bin/kld
# per-stage test timeout: tcc's kernel is about half as fast
SH_TIMEOUT ?= 120

selfhost: $(KERNEL) $(EBOOT) $(FSIMG) $(MKFS) $(EUDISK)
	@$(PYTHON) tools/selfhost.py --qemu "$(QEMU) $(QEMUBASE)" --image $(FSIMG)
	@if echo "$(SH_FILES)" | tr ' ' '\n' | grep -E '=$(BUILD)/user/stripped/|=$(SYSROOT)/usr/lib/(crt[1in]|libc|tcc/libtcc1|tcc/runmain)\.'; then \
		echo "selfhost: the files above would still be the gcc-built ones"; exit 1; fi
	$(Q)$(call mkdisk,$(BUILD)/selfhost/fs.img,$(SH_KERNEL),$(SH_FILES))
	@echo "selfhost: $(SH_KERNEL) and build/selfhost/fs.img: built on Eunuchs, by tcc"
	@$(PYTHON) tools/run_tests.py --qemu "$(QEMU) $(subst -kernel $(BOOT_IMAGE),-kernel $(SH_EBOOT),$(QEMUBASE))" \
		--image $(BUILD)/selfhost/fs.img --mkfs $(MKFS) --timeout $(SH_TIMEOUT) --append "$(APPEND)" \
		--programs $(SH_OUT)/bin

# The kernel compiled by the cross tcc and linked by kld, on the Mac (what
# selfhost builds on Eunuchs): build/tcc-kernel/kernel/kernel.elf.
$(KLD): tools/kld/kld.c
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) -O1 -g -Wall -Wextra -Werror -o $@ $<

tcc-kernel: $(XTCC) $(KLD)
	@$(MAKE) --no-print-directory -s -f kernel/build.mk OUT=$(BUILD)/tcc-kernel TCC=$(CURDIR)/$(XTCC) KLD=$(KLD)

# SMP soak test: STRESS_SECS of parallel fork/exec/pipe/file/COW load on
# $(CPUS) CPUs, booted with lockdep=panic (and the lockup watchdog on), then
# fsck. Runs on a scratch copy of the disk image.
STRESS_SECS ?= 120
stress: $(KERNEL) $(EBOOT) $(FSIMG) $(MKFS)
	@cp -f $(FSIMG) $(BUILD)/stress.img
	@$(PYTHON) tools/timeout.py $$(($(STRESS_SECS) + 60)) $(QEMU) $(QEMUBASE) \
		$(call DISKOPTS,$(BUILD)/stress.img) \
		-append "panic=shutdown lockdep=panic utest=stress:$(STRESS_SECS) $(APPEND)"
	@$(MKFS) --check $(BUILD)/stress.img

# Profiles: `profile dump` (shell or monitor) or eu-profile-dump (GDB) writes
# build/profile.bin; this prints the report and writes an interactive SVG
# flamegraph (open it in a browser) and folded stacks.
flamegraph:
	@$(PYTHON) tools/profile/euprof.py report $(BUILD)/profile.bin --no-idle
	@$(PYTHON) tools/profile/euprof.py svg $(BUILD)/profile.bin $(BUILD)/profile.svg --no-idle
	@$(PYTHON) tools/profile/euprof.py folded $(BUILD)/profile.bin $(BUILD)/profile.folded --no-idle
	@echo "open $(BUILD)/profile.svg"

# Everything, headless, from a clean build: see tools/ci.py (CI_FLAGS=--quick).
ci:
	@$(PYTHON) tools/ci.py $(CI_FLAGS)

# Convert a `trace dump` from the monitor (build/trace.bin) for ui.perfetto.dev.
trace-json:
	$(PYTHON) tools/trace/eutrace.py perfetto $(BUILD)/trace.bin $(BUILD)/trace.json
	@echo "text view: $(PYTHON) tools/trace/eutrace.py text $(BUILD)/trace.bin | less"

HOST_TESTS := $(patsubst tests/host/%.c,$(BUILD)/host/%,$(wildcard tests/host/test_*.c))

HOST_CFLAGS := -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
               -Ilib/include -Ikernel/include -Ikernel/mm

# Each host test links the kernel sources it exercises.
$(BUILD)/host/test_printf: lib/printf.c
$(BUILD)/host/test_buddy: kernel/mm/buddy.c
$(BUILD)/host/test_tcp: kernel/net/tcp.c kernel/net/netbuf.c kernel/net/cksum.c

$(BUILD)/host/%: tests/host/%.c
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(if $(filter %test_tcp,$@),-DNET_HOST_TEST) -o $@ $(filter-out $(CC_ID),$^)

$(KOBJS) $(ULIB) $(UMULTI_OBJS) $(BUILD)/tcc/tcc.o \
  $(BUILD)/ksyms0.o $(BUILD)/ksyms.o \
  $(patsubst user/bin/%.c,$(BUILD)/user/bin/%.o,$(wildcard user/bin/*.c)) \
  $(XTCC) $(MKFS) $(EUDISK) $(KLD) $(HOST_TESTS): $(CC_ID)

test-host: $(HOST_TESTS) $(MKFS) $(EUDISK) $(BUILD)/kernel.boot $(BUILD)/user/bin/hello $(XTCC)
	@for t in $(HOST_TESTS); do echo "== $$t"; $$t || exit 1; done
	@echo "== tests/host/test_fsck.py"
	@$(PYTHON) tests/host/test_fsck.py $(MKFS) $(BUILD)/user/bin/hello
	@echo "== tests/host/test_eudisk.py"
	@$(PYTHON) tests/host/test_eudisk.py $(EUDISK) $(BUILD)/kernel.boot
	@echo "== tests/host/test_tcc_asm.py"
	@$(PYTHON) tests/host/test_tcc_asm.py $(XTCC) $(TCC_SRC) $(CROSS)

clean:
	rm -rf $(BUILD)

-include $(KOBJS:.o=.d) $(ULIB:.o=.d) $(UPROGS:=.d) $(UMULTI_OBJS:.o=.d)

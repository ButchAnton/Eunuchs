# Eunuchs — top-level build
#
#   make run        boot in QEMU (Ctrl-A X quits QEMU)
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
# --- Disk image: user programs and a few directories, built by the host mkfs ---
MKFS    := $(BUILD)/tools/mkfs
FSIMG   := $(BUILD)/fs.img
FS_MB   ?= 32
EUTILS_LINKS = echo mkdir rm rmdir touch ln mv cp wc stat head tail tee chmod chown env id crc32 profile \
               true false yes kill date dmesg free sleep strace su dbg uname clear
FS_FILES = $(foreach p,$(UPROGS),/bin/$(notdir $(p))=$(BUILD)/user/stripped/$(notdir $(p))) \
           $(foreach l,$(EUTILS_LINKS),/bin/$(l)=@/bin/eutils) /bin/sh=@/bin/esh /bin/less=@/bin/more \
           /bin/vi=@/bin/edit /bin/view=@/bin/edit /bin/emacs=@/bin/edit /bin/em=@/bin/edit \
           /etc/motd=user/etc/motd /etc/passwd=user/etc/passwd /etc/group=user/etc/group /etc/hostname=user/etc/hostname \
           /etc/eshrc=user/etc/eshrc /dev/ /proc/ /tmp/:0777 \
           /www/ /www/index.html=user/www/index.html \
           /home/ /home/guest/:0755:1000:1000
DISKOPTS = -global virtio-mmio.force-legacy=false \
           -drive file=$(1),if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0

QEMUBASE = -machine virt -bios default -nographic -smp $(CPUS) -m $(MEM) \
           -semihosting-config enable=on,target=native -kernel $(KERNEL)

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
# make run/debug also attach the /home disk (the test scenarios don't).
HOMEOPTS = -drive file=$(HOME_IMG),if=none,format=raw,id=hd1 -device virtio-blk-device,drive=hd1
QEMUOPTS = $(QEMUBASE) $(call DISKOPTS,$(FSIMG)) $(HOMEOPTS) $(NETOPTS)

.PHONY: all user fsck clean-home run debug gdb test stress test-host trace-json flamegraph record replay replay-debug replay-gdb replay-check ci qemu-log disasm clean

all: $(KERNEL) $(FSIMG)

# --- Two-pass link: pass 1 has an empty symbol table; its symbols generate
# --- the real table for pass 2. .ksyms is last in the image, so no other
# --- address moves; we verify that anyway.
$(BUILD)/ksyms0.S: tools/gen_ksyms.py
	@mkdir -p $(BUILD)
	$(Q)$(PYTHON) tools/gen_ksyms.py < /dev/null > $@

$(PASS1): $(KOBJS) $(BUILD)/ksyms0.o $(LDSCRIPT)
	$(call msg,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KOBJS) $(BUILD)/ksyms0.o -lgcc

$(BUILD)/ksyms.S: $(PASS1) tools/gen_ksyms.py
	$(call msg,KSYMS,$@)
	$(Q)$(NM) -n $(PASS1) | $(PYTHON) tools/gen_ksyms.py > $@

$(KERNEL): $(KOBJS) $(BUILD)/ksyms.o $(LDSCRIPT)
	$(call msg,LD,$@)
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KOBJS) $(BUILD)/ksyms.o -lgcc
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

# The disk image gets copies without DWARF (symbol tables stay, for the
# kernel's user backtraces); the full binaries in build/user/bin are for GDB.
$(BUILD)/user/stripped/%: $(BUILD)/user/bin/%
	@mkdir -p $(dir $@)
	$(Q)$(CROSS)objcopy --strip-debug $< $@

$(MKFS): tools/mkfs/mkfs.c kernel/include/efs_format.h
	@mkdir -p $(dir $@)
	$(call msg,HOSTCC,$@)
	$(Q)$(HOSTCC) -O1 -g -Wall -Wextra -Werror -Ikernel/include -o $@ $<

# Rebuilt whenever a program changes, which discards files created at run time.
$(FSIMG): $(MKFS) $(patsubst $(BUILD)/user/bin/%,$(BUILD)/user/stripped/%,$(UPROGS)) user/etc/* user/www/*
	$(call msg,MKFS,$@)
	$(Q)$(MKFS) --label root $@ $(FS_MB) $(FS_FILES) > /dev/null

# /home lives on its own disk, made once and never rebuilt, so what you
# create there survives `make` and `make clean` (`make clean-home` resets it).
HOME_IMG ?= home.img
HOME_MB  ?= 64
$(HOME_IMG): | $(MKFS)
	$(call msg,MKFS,$@)
	$(Q)$(MKFS) --label home $@ $(HOME_MB) /guest/:0755:1000:1000 > /dev/null

clean-home:
	rm -f $(HOME_IMG)

fsck: $(MKFS)
	$(MKFS) --check $(FSIMG)

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

run: $(KERNEL) $(FSIMG) $(HOME_IMG)
	$(check_tty)
	$(QEMU) $(QEMUOPTS) -append "$(APPEND)"

debug: $(KERNEL) $(FSIMG) $(HOME_IMG)
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
RROPTS = -machine virt -bios default -nographic -smp 1 -m $(MEM) -kernel $(RRDIR)/kernel.elf \
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

qemu-log: $(KERNEL) $(FSIMG) $(HOME_IMG)
	$(check_tty)
	$(QEMU) $(QEMUOPTS) -append "$(APPEND)" -d int,guest_errors,unimp -D $(BUILD)/qemu.log

disasm: $(KERNEL)
	$(OBJDUMP) -dS $(KERNEL) > $(BUILD)/kernel.asm

# --- Testing ---

# tools/timeout.py kills QEMU if a test run hangs.
# The whole scenario: kernel + user tests, fsck, persistence across a reboot,
# a simulated power failure mid-commit, log recovery, fsck. See tools/run_tests.py.
test: $(KERNEL) $(FSIMG) $(MKFS)
	@$(PYTHON) tools/run_tests.py --qemu "$(QEMU) $(QEMUBASE)" --image $(FSIMG) \
		--mkfs $(MKFS) --timeout $(TEST_TIMEOUT) --append "$(APPEND)"

# SMP soak test: STRESS_SECS of parallel fork/exec/pipe/file/COW load on
# $(CPUS) CPUs, booted with lockdep=panic (and the lockup watchdog on), then
# fsck. Runs on a scratch copy of the disk image.
STRESS_SECS ?= 120
stress: $(KERNEL) $(FSIMG) $(MKFS)
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
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(if $(filter %test_tcp,$@),-DNET_HOST_TEST) -o $@ $^

test-host: $(HOST_TESTS) $(MKFS) $(BUILD)/user/bin/hello
	@for t in $(HOST_TESTS); do echo "== $$t"; $$t || exit 1; done
	@echo "== tests/host/test_fsck.py"
	@$(PYTHON) tests/host/test_fsck.py $(MKFS) $(BUILD)/user/bin/hello

clean:
	rm -rf $(BUILD)

-include $(KOBJS:.o=.d) $(ULIB:.o=.d) $(UPROGS:=.d) $(UMULTI_OBJS:.o=.d)

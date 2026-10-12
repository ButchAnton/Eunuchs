# User space, built on Eunuchs by tcc from this source tree (shared from the
# Mac and mounted on /mnt/host): the C library and tcc's runtime into a
# sysroot, then every program, tcc included, against that sysroot. Nothing
# here comes from gcc. `make selfhost` on the Mac runs it twice (the second
# time with the tcc the first run built) and tests the result.
#
#   cd /mnt/host && make -f user/build.mk [OUT=dir] [TCC=compiler]
#
# OUT/sysroot/usr/lib    crt1.o crti.o crtn.o libc.a
# OUT/sysroot/usr/lib/tcc libtcc1.a runmain.o
# OUT/bin                 the programs (and kld, the kernel's linker)

TCC     = tcc
OUT     = build/selfhost
LIB     = $(OUT)/sysroot/usr/lib
TCCLIB  = $(LIB)/tcc
TCCSRC  = third_party/tinycc
OBJ     = $(OUT)/obj

# Headers from the source tree (not /usr/include), libraries from OUT.
CFLAGS  = -Wall -DFMT_FLOAT -nostdinc -Iuser/include -Ilib/include -I$(TCCSRC)/include
CC      = $(TCC) $(CFLAGS)
LINK    = $(TCC) $(CFLAGS) -B$(TCCLIB)

LIBC_C  := $(wildcard user/lib/*.c) lib/printf.c lib/string.c
LIBC_O  := $(patsubst %.c,$(OBJ)/%.o,$(LIBC_C)) $(OBJ)/user/lib/setjmp.o $(OBJ)/user/lib/syscall.o
# tcc's runtime for riscv64 (lib-arm64.c is the software long double)
RT_O    := $(patsubst %,$(OBJ)/rt/%.o,lib-arm64 stdatomic builtin armflush atomic alloca)
SYSLIBS := $(LIB)/crt1.o $(LIB)/crti.o $(LIB)/crtn.o $(LIB)/libc.a $(TCCLIB)/libtcc1.a $(TCCLIB)/runmain.o

PROGS   := $(basename $(notdir $(wildcard user/bin/*.c)))
MULTI   := db edit esh
ALL     := $(addprefix $(OUT)/bin/,$(PROGS) $(MULTI) tcc kld)

all: $(ALL)
	@echo "user space built in $(OUT)"

sysroot: $(SYSLIBS)

# --- the sysroot ---

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

$(OBJ)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -D__ASSEMBLY__ -c -o $@ $<

$(OBJ)/rt/%.o: $(TCCSRC)/lib/%.c
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

$(OBJ)/rt/%.o: $(TCCSRC)/lib/%.S
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

$(LIB)/libc.a: $(LIBC_O)
	@mkdir -p $(@D)
	rm -f $@
	$(TCC) -ar rcs $@ $^

$(LIB)/crt1.o: user/lib/crt0.S
	@mkdir -p $(@D)
	$(CC) -D__ASSEMBLY__ -DNO_GP -c -o $@ $<

# empty, as with gcc: tcc's linker wants them (from stdin, so the file name
# they record is "-" whatever OUT is)
$(LIB)/crti.o $(LIB)/crtn.o:
	@mkdir -p $(@D)
	$(TCC) -x assembler -c -o $@ - < /dev/null

$(TCCLIB)/libtcc1.a: $(RT_O)
	@mkdir -p $(@D)
	rm -f $@
	$(TCC) -ar rcs $@ $^

$(TCCLIB)/runmain.o: $(TCCSRC)/lib/runmain.c
	@mkdir -p $(@D)
	$(CC) -c -o $@ $<

# --- the programs ---

$(OUT)/bin/%: user/bin/%.c $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -o $@ $<

$(OUT)/bin/db: $(wildcard user/bin/db/*.c) $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -o $@ $(filter %.c,$^)

$(OUT)/bin/edit: $(wildcard user/bin/edit/*.c) $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -o $@ $(filter %.c,$^)

$(OUT)/bin/esh: $(wildcard user/bin/esh/*.c) $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -o $@ $(filter %.c,$^)

# the kernel's linker (kernel/build.mk uses it)
$(OUT)/bin/kld: tools/kld/kld.c $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -o $@ $<

$(OUT)/bin/tcc: $(wildcard $(TCCSRC)/*.c $(TCCSRC)/*.h) ports/tcc/config.h $(SYSLIBS)
	@mkdir -p $(@D)
	$(LINK) -w -DONE_SOURCE=1 -Iports/tcc -o $@ $(TCCSRC)/tcc.c

clean:
	rm -rf $(OUT)

.PHONY: all sysroot clean

# The Eunuchs programs, rebuilt on Eunuchs with tcc.
#
#   make              build everything into ./bin
#   make bin/ls       build one program
#   make install      copy the new programs over /bin (as root)
#   make clean
#
# The whole system's source, kernel included, is in /usr/src/eunuchs:
# `cd /usr/src/eunuchs && sh tools/selfhost.sh` rebuilds all of it (twice,
# into build/selfhost; it needs about 20 MB free).
#
# Each top-level .c file is a program; db/, edit/ and esh/ are programs made
# of several files. CFLAGS=-g gives programs db can debug at source level. The headers and libraries are in /usr/include and /usr/lib.

CC      = tcc
CFLAGS  = -Wall
BIN     = bin

PROGS  := $(basename $(wildcard *.c))
MULTI  := db edit esh
ALL    := $(addprefix $(BIN)/,$(PROGS) $(MULTI))

all: $(ALL)

$(BIN)/%: %.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/db: $(wildcard db/*.c) db/db.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BIN)/edit: $(wildcard edit/*.c) edit/edit.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BIN)/esh: $(wildcard esh/*.c) esh/esh.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BIN):
	mkdir -p $(BIN)

install: all
	@for p in $(PROGS) $(MULTI); do echo cp $(BIN)/$$p /bin/$$p; cp $(BIN)/$$p /bin/$$p || exit 1; done

clean:
	rm -rf $(BIN)

.PHONY: all install clean

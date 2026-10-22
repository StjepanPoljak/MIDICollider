# Builds init, midid and recvd into $(O). Used by the top-level Makefile and
# by the Buildroot package, which passes CC/CFLAGS/LDFLAGS and STATIC=1.

CC     ?= cc
CFLAGS ?= -O2 -g
O      ?= ../build
override CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra
ifeq ($(STATIC),1)
override LDFLAGS += -static
endif

BIN = $(O)/init $(O)/midid $(O)/recvd

all: $(BIN)

$(O)/%: %.c osc.h | $(O)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lm

$(O):
	mkdir -p $@

clean:
	rm -f $(BIN)

.PHONY: all clean

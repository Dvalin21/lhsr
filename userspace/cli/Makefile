# LHSR Userspace CLI Makefile

CC ?= clang
CFLAGS += -Wall -Wextra -O2 -g
CFLAGS += -I../../include -I../lib

all: lhsrctl

lhsrctl: lhsrctl.c
	$(CC) $(CFLAGS) -o $@ lhsrctl.c ../../lib/raid_engine/lhsr_raid.c

clean:
	rm -f lhsrctl

.PHONY: all clean
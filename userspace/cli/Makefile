# LHSR Userspace CLI Makefile

CC ?= clang
CFLAGS += -Wall -Wextra -O2 -g
CFLAGS += -I../../include -I../../lib
LDFLAGS += -lpthread

all: lhsrctl

lhsrctl: lhsrctl.c shr.c shr.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ lhsrctl.c shr.c ../../lib/liblhsr.a

clean:
	rm -f lhsrctl

.PHONY: all clean
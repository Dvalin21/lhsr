# LHSR Test Makefile
#
# Run tests with: make test-health

CC       ?= cc
CFLAGS   := -Wall -Wextra -O2 -g -I../userspace/daemon -I../include -I../lib

.PHONY: test-health clean

test-health: test-health.c ../userspace/daemon/lhsr-health.o ../userspace/daemon/lhsr-trend.o
	$(CC) $(CFLAGS) -o /tmp/$@ $^ -lsqlite3
	/tmp/$@

clean:
	rm -f /tmp/test-health

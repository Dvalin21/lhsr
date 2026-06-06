# LHSR Library Makefile

CC ?= clang
CFLAGS += -Wall -Wextra -O2 -g
CFLAGS += -I../include -I.

all: liblhsr.a

liblhsr.a: raid_engine/lhsr_raid.c
	$(CC) $(CFLAGS) -c -o raid_engine/lhsr_raid.o raid_engine/lhsr_raid.c
	ar rcs $@ raid_engine/lhsr_raid.o

clean:
	rm -f liblhsr.a raid_engine/*.o

.PHONY: all clean
# LHSR Daemon Makefile

CC ?= cc
CFLAGS := -Wall -Wextra -O2 -g -I../../include -I../lib
LDFLAGS := -lpthread

TARGET = lhsrd
SOURCES = lhsrd.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ ../../lib/liblhsr.a

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean

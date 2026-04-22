# LHSR Recovery Tool Makefile

CC ?= cc
CFLAGS := -Wall -Wextra -O2 -g -I../../include -I../lib
LDFLAGS := -lpthread

TARGET = lhsr-scan
SOURCES = lhsr-scan.c
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

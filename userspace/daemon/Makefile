# LHSR Daemon Makefile
#
# Requires: libdevmapper-dev (for dm_task_* API)
# Requires: libpthread (always present)

CC       ?= cc
CFLAGS   := -Wall -Wextra -O2 -g -I../../include -I../../lib

TARGET   = lhsrd
SOURCES  = lhsrd.c lhsr-dm.c lhsr-smart.c lhsr-config.c
OBJECTS  = $(SOURCES:.c=.o)

LIBS     := -lpthread -ldevmapper

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS) ../../lib/liblhsr.a
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/usr/sbin
	install -m 755 $(TARGET) $(DESTDIR)/usr/sbin/
	install -d $(DESTDIR)/etc/lhsr
	test -f $(DESTDIR)/etc/lhsr/lhsrd.conf || \
		install -m 644 ../config/lhsrd.conf.example $(DESTDIR)/etc/lhsr/lhsrd.conf
	install -d $(DESTDIR)/etc/init.d
	install -m 755 ../init.d/lhsrd $(DESTDIR)/etc/init.d/lhsrd 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)/usr/sbin/$(TARGET)
	rm -f $(DESTDIR)/etc/lhsr/lhsrd.conf

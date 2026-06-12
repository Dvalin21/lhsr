# LHSR Daemon Makefile
#
# Requires: libdevmapper-dev (for dm_task_* API)
# Requires: libsqlite3-dev (for trend database)
# Requires: libpthread (always present)

CC       ?= cc
CFLAGS   := -Wall -Wextra -O2 -g -I../../include -I../../lib

TARGET   = lhsrd
SOURCES  = lhsrd.c lhsr-dm.c lhsr-smart.c lhsr-config.c \
           lhsr-trend.c lhsr-control.c lhsr-health.c
OBJECTS  = $(SOURCES:.c=.o)

LIBS     := -lpthread -ldevmapper -lsqlite3

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
	install -d $(DESTDIR)/var/lib/lhsrd
	install -d $(DESTDIR)/usr/lib/systemd/system
	test -f ../systemd/lhsrd.service && \
		install -m 644 ../systemd/lhsrd.service \
			$(DESTDIR)/usr/lib/systemd/system/lhsrd.service || true

uninstall:
	rm -f $(DESTDIR)/usr/sbin/$(TARGET)
	rm -f $(DESTDIR)/etc/lhsr/lhsrd.conf
	rm -f $(DESTDIR)/usr/lib/systemd/system/lhsrd.service

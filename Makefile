PREFIX ?= /usr/local
DESTDIR ?=
MANPREFIX ?= $(PREFIX)/share/man

CC ?= gcc
PKG_CONFIG ?= pkg-config

X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 2>/dev/null)

ifeq ($(strip $(X11_LIBS)),)
X11_LIBS := -lX11
endif

WMCTRLNG_DIR := ../wmctrlng
WMCTRLNG_LIB := $(WMCTRLNG_DIR)/lib/libwmctrl.a

override CFLAGS += -O2 -Wall -Wextra -pedantic -std=c11 $(X11_CFLAGS) -I$(WMCTRLNG_DIR)/lib/include
override LDFLAGS +=
override LIBS += $(WMCTRLNG_LIB)
override LIBS += $(X11_LIBS)

TARGET := wmposxy
SRC := main.c
MANPAGE := wmposxy.1

all: $(TARGET)

$(WMCTRLNG_LIB):
	$(MAKE) -C $(WMCTRLNG_DIR)/lib

$(TARGET): $(SRC) $(WMCTRLNG_LIB)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) $(LIBS) -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -d $(DESTDIR)$(MANPREFIX)/man1
	install -m 644 $(MANPAGE) $(DESTDIR)$(MANPREFIX)/man1/$(MANPAGE)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(MANPREFIX)/man1/$(MANPAGE)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean

PREFIX ?= /usr/local
DESTDIR ?=

CC ?= gcc
PKG_CONFIG ?= pkg-config

X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 2>/dev/null)

override CFLAGS += -O2 -Wall -Wextra -pedantic -std=c11 $(X11_CFLAGS)
override LDFLAGS +=
override LIBS += $(X11_LIBS)

TARGET := wmposxy
SRC := main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) $(LIBS) -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=c11
LIBS ?= -lX11
TARGET := wmposxy
SRC := main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LIBS) -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean

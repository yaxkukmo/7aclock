CC      = gcc
CFLAGS  = -O2 -Wall -Wextra $(shell pkg-config --cflags x11 cairo cairo-xlib)
LDFLAGS = $(shell pkg-config --libs   x11 cairo cairo-xlib) -lm
TARGET  = 7aclock
SRC     = 7aclock.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean

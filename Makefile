CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-format-truncation
PREFIX  ?= /usr/local
LDLIBS  = -lncurses

clawterm: clawterm.o serial.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clawterm.o: clawterm.c serial.h
serial.o: serial.c serial.h

install: clawterm
	install -D -m 755 clawterm $(DESTDIR)$(PREFIX)/bin/clawterm

clean:
	rm -f clawterm *.o

.PHONY: install clean

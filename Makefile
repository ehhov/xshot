PREFIX = /opt/local
MANPREFIX = ${PREFIX}/share/man

CC = cc
CFLAGS = -std=c99 -Wall -pedantic -Os -D_POSIX_C_SOURCE=200809L
LIBS = -lX11 -lXrandr -lpng

all: xshot

xshot: xshot.c
	${CC} ${CFLAGS} ${LIBS} $< -o $@

clean:
	rm -f xshot

install: all
	mkdir -p ${PREFIX}/bin
	cp -f xshot ${PREFIX}/bin
	chmod 755 ${PREFIX}/bin/xshot
	mkdir -p ${MANPREFIX}/man1
	cp -f xshot.1 ${MANPREFIX}/man1
	chmod 644 ${MANPREFIX}/man1/xshot.1

uninstall:
	rm -f ${PREFIX}/bin/xshot
	rm -f ${MANPREFIX}/man1/xshot.1

.PHONY: all clean install uninstall

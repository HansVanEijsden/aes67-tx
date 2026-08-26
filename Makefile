# aes67-tx - publish an AES67/RTP stream as a PTP-synchronised AES67 source
#
CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= /usr/local

all: aes67-tx

aes67-tx: aes67-tx.c
	$(CC) $(CFLAGS) -o aes67-tx aes67-tx.c

install: aes67-tx
	install -D -m 0755 aes67-tx $(DESTDIR)$(PREFIX)/bin/aes67-tx

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/aes67-tx

clean:
	rm -f aes67-tx

.PHONY: all install uninstall clean

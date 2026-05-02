CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -static

all: tun_forwarder

tun_forwarder: tun_forwarder.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f tun_forwarder

.PHONY: all clean
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g

.PHONY: all clean

all: server

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f server

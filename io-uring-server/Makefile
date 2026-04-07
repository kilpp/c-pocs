CC        = gcc
CFLAGS    = -Wall -Wextra -O2 -g -I/tmp/liburing-install/include
LDFLAGS   = -L/tmp/liburing-install/lib -luring -Wl,-rpath,/tmp/liburing-install/lib

.PHONY: all clean

all: server

server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f server

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lrt -lpthread

all: writer reader

writer: writer.c shared.h
	$(CC) $(CFLAGS) -o writer writer.c $(LDFLAGS)

reader: reader.c shared.h
	$(CC) $(CFLAGS) -o reader reader.c $(LDFLAGS)

clean:
	rm -f writer reader
	# also nuke the shm object if it was left behind
	-rm -f /dev/shm/poc_shm_ipc

.PHONY: all clean

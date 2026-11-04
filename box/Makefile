CFLAGS ?= -O2 -g
CFLAGS += -Wall -std=gnu11 -pthread

nm-relay: nm-relay.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nm-relay

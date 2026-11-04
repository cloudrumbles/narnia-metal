CFLAGS ?= -O2 -g
CFLAGS += -Wall -std=gnu11 -pthread

all: nm-relay nm-input

nm-relay: nm-relay.c
	$(CC) $(CFLAGS) -o $@ $<

nm-input: nm-input.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXtst

clean:
	rm -f nm-relay nm-input

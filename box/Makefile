CFLAGS ?= -O2 -g
CFLAGS += -Wall -std=gnu11 -pthread

all: nm-relay nm-input nm-audio

nm-relay: nm-relay.c
	$(CC) $(CFLAGS) -o $@ $<

nm-input: nm-input.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXtst

nm-audio: nm-audio.c ../common/nm_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libpulse-simple opus) -lm

clean:
	rm -f nm-relay nm-input nm-audio

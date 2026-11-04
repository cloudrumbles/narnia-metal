# Linux test host. Links vkr built by ../vkr/build-vkr.sh (the same
# meson-free build the iOS app uses) from a patched virglrenderer checkout
# (scripts/dev-box-setup.sh vr patches ~/src/virglrenderer).

VR_SRC ?= $(HOME)/src/virglrenderer
VKR_OUT ?= build-vkr
LIBVKR = $(VKR_OUT)/libvkr.a

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wno-unused-parameter -std=gnu11 -pthread \
	-I../common -I../vkr -I$(VR_SRC)/src -I$(VR_SRC)/src/venus \
	-I$(VR_SRC)/src/mesa -I$(VR_SRC)/src/mesa/util

LIBS = $(LIBVKR) -lz -lpthread -ldl -lm

CORE_OBJS = nm_host.o nm_present.o nm_relay_client.o
OBJS = $(CORE_OBJS) nm_presenter_png.o nm_driver_loader.o main_cli.o

nm-host: $(OBJS) $(LIBVKR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

$(LIBVKR): ../vkr/build-vkr.sh ../vkr/config.h ../vkr/shim.c
	../vkr/build-vkr.sh $(VR_SRC) $(VKR_OUT)

%.o: %.c *.h ../common/nm_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf nm-host $(OBJS) $(VKR_OUT)

.PHONY: clean

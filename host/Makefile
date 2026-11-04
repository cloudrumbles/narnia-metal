# Linux test host. Links vkr out of a patched virglrenderer build tree
# (scripts/dev-box-setup.sh vr builds one in ~/src/virglrenderer/build).

VR_SRC ?= $(HOME)/src/virglrenderer
VR_BUILD ?= $(VR_SRC)/build

CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wno-unused-parameter -std=gnu11 -pthread \
	-I../common -I$(VR_BUILD) -I$(VR_BUILD)/src -I$(VR_SRC)/src \
	-I$(VR_SRC)/src/venus -I$(VR_SRC)/src/mesa -I$(VR_SRC)/src/mesa/util \
	-I$(VR_SRC)/src/mesa/include -I$(VR_SRC)/src/gallium/include

LIBS = $(VR_BUILD)/src/libvirgl.a $(VR_BUILD)/src/mesa/libmesa.a \
	-lpthread -ldl -lm

OBJS = nm_host.o nm_driver_loader.o main_linux.o

nm-host: $(OBJS) $(VR_BUILD)/src/libvirgl.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c nm_host.h nm_driver.h ../common/nm_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f nm-host $(OBJS)

.PHONY: clean

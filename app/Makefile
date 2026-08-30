YAC_ROOT ?= $(abspath ..)
YUI_ROOT ?= /media/evil/d/dev/yui
YUI_LIBDIR ?= $(YUI_ROOT)/build/pc/None/None
YC ?= $(YAC_ROOT)/yc_a

CC ?= gcc
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LIBS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)

# Prebuilt .a from ya is ASan-instrumented; the .so must match.
ASAN ?= -fsanitize=address

YUI_INC = -I$(YUI_ROOT)/src -I$(YUI_ROOT)/lib/cjson \
	-I$(YUI_ROOT)/lib/yaml2json -I$(YUI_ROOT)/platform/common

CFLAGS ?= -std=c11 -fPIC -O2 -g $(ASAN) $(SDL_CFLAGS) $(YUI_INC)

SO = native/libyui_yac.so
BOOT_C = $(YUI_ROOT)/platform/common/yui_boot.c

# DAG: glue → yui → yaml2json → yaml/cjson; tsm/socket leaf. No cycle.
YUI_LIBS = \
	$(YUI_LIBDIR)/libyui.a \
	$(YUI_LIBDIR)/libyaml2json.a \
	$(YUI_LIBDIR)/libyaml.a \
	$(YUI_LIBDIR)/libcjson.a \
	$(YUI_LIBDIR)/libtsm.a \
	$(YUI_LIBDIR)/libsocket.a \
	$(SDL_LIBS) -lm -ldl -lpthread

.PHONY: all so apps counter hello lantern clean

all: so apps

so: $(SO)

$(SO): native/yui_yac.c $(BOOT_C) $(YUI_LIBDIR)/libyui.a
	mkdir -p native
	$(CC) $(CFLAGS) -shared -o $@ native/yui_yac.c $(BOOT_C) $(YUI_LIBS)

apps: counter hello lantern

counter: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cat native/yui.yac counter/main.yac > $(YAC_ROOT)/build/app/counter.yac
	cd $(YAC_ROOT) && $(YC) build/app/counter.yac -o app/counter/counter
	chmod +x counter/counter

hello: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cat native/yui.yac hello/main.yac > $(YAC_ROOT)/build/app/hello.yac
	cd $(YAC_ROOT) && $(YC) build/app/hello.yac -o app/hello/hello
	chmod +x hello/hello

lantern: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cat native/yui.yac lantern/main.yac > $(YAC_ROOT)/build/app/lantern.yac
	cd $(YAC_ROOT) && $(YC) build/app/lantern.yac -o app/lantern/lantern
	chmod +x lantern/lantern

clean:
	rm -f $(SO) native/yui_host counter/counter hello/hello lantern/lantern
	rm -rf $(YAC_ROOT)/build/app

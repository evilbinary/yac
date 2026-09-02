YAC_ROOT ?= $(abspath ..)
YUI_ROOT ?= /media/evil/d/dev/yui
YUI_LIBDIR ?= $(YUI_ROOT)/build/pc/None/None
# Prefer native yc_a from `make yc` (build/yc_tmp); .exe for PE hosts / Git Bash.
YC ?= $(firstword $(wildcard \
	$(YAC_ROOT)/build/yc_tmp/yc_a.exe \
	$(YAC_ROOT)/build/yc_tmp/yc_a \
	$(YAC_ROOT)/yc_a.exe \
	$(YAC_ROOT)/yc_a) \
	$(YAC_ROOT)/build/yc_tmp/yc_a)
YC_PKG ?= --pkg src-self,pkg
YC_PKG_LLM ?= --pkg src-self,pkg,app/llm/transformer
# e.g. make lantern YC_FLAGS=-g  for nm/gdb function names
YC_FLAGS ?=

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

.PHONY: all so apps counter hello lantern llm llm-trigram llm-transformer llm-train llm-infer clean

all: so apps

so: $(SO)

$(SO): native/yui_yac.c $(BOOT_C) $(YUI_LIBDIR)/libyui.a
	mkdir -p native
	$(CC) $(CFLAGS) -shared -o $@ native/yui_yac.c $(BOOT_C) $(YUI_LIBS)

apps: counter hello lantern llm

counter: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG) $(YC_FLAGS) app/counter/main.yac -o app/counter/counter
	chmod +x counter/counter

hello: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG) $(YC_FLAGS) app/hello/main.yac -o app/hello/hello
	chmod +x hello/hello

lantern: $(SO)
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG) $(YC_FLAGS) app/lantern/main.yac -o app/lantern/lantern
	chmod +x lantern/lantern

# No YUI. Examples live in app/llm/<name>/ ; shared corpus at app/llm/corpus.txt.
llm: llm-trigram

llm-trigram:
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG) $(YC_FLAGS) app/llm/trigram/main.yac -o app/llm/trigram/trigram
	chmod +x llm/trigram/trigram

llm-transformer: llm-train llm-infer

llm-train:
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG_LLM) $(YC_FLAGS) app/llm/transformer/train.yac -o app/llm/transformer/train
	chmod +x llm/transformer/train

llm-infer:
	mkdir -p $(YAC_ROOT)/build/app
	cd $(YAC_ROOT) && $(YC) $(YC_PKG_LLM) $(YC_FLAGS) app/llm/transformer/infer.yac -o app/llm/transformer/infer
	chmod +x llm/transformer/infer

clean:
	rm -f $(SO) native/yui_host counter/counter hello/hello lantern/lantern \
		llm/llm llm/llm.exe llm/trigram/trigram llm/trigram/trigram.exe \
		llm/train llm/train.exe llm/infer llm/infer.exe llm/transformer.w \
		llm/transformer/train llm/transformer/train.exe \
		llm/transformer/infer llm/transformer/infer.exe \
		llm/transformer/transformer.w
	rm -rf $(YAC_ROOT)/build/app

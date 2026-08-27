CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/value.c src/bignum.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c src/cps.c src/eval_cps.c src/uncps.c src/gc.c src/rtio.c src/ckpt.c src/scheme.c src/profile.c
BUILD = build
OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
BIN = yac
YC_BUILD = $(BUILD)/yc_tmp
YC_BUNDLE = $(YC_BUILD)/yc_bundle.yac
YC_BIN = $(YC_BUILD)/yc
YC_SRCS = src-self/log.yac src-self/pass.yac src-self/target.yac src-self/map.yac \
	src-self/lexer.yac src-self/parser.yac src-self/anf.yac \
	src-self/encode_x64.yac src-self/encode_arm64.yac src-self/encode_riscv64.yac \
	src-self/elf.yac src-self/pe.yac src-self/macho.yac src-self/pack.yac \
	src-self/lir.yac src-self/runtime.yac src-self/emit.yac \
	src-self/emit_x86_64.yac src-self/emit_arm64.yac src-self/emit_riscv64.yac \
	src-self/lower.yac src-self/profile.yac src-self/backend.yac src-self/yc.yac

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(YC_BUILD): | $(BUILD)
	mkdir -p $(YC_BUILD)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(YC_BUNDLE): $(YC_SRCS) | $(YC_BUILD)
	cat $(YC_SRCS) > $@

$(YC_BIN): $(BIN) $(YC_BUNDLE)
	./$(BIN) $(YC_BUNDLE) $(YC_BUNDLE) -o $@
	chmod +x $@

yc: $(YC_BIN)

test: $(BIN)
	./yac tests/run.yac

test-boot: test

prop: $(BIN)
	./yac tests/prop.yac

$(BUILD)/genyac: tools/genyac.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN)
	rm -rf $(BUILD)
	rm -f src/*.o

.PHONY: all clean test test-boot prop yc

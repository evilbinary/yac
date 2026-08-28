CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/value.c src/bignum.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c src/cps.c src/eval_cps.c src/uncps.c src/gc.c src/rtio.c src/ckpt.c src/scheme.c src/profile.c
BUILD = build
OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
BIN = yac
YC_BUILD = $(BUILD)/yc_tmp
YC_BUNDLE = $(YC_BUILD)/yc_bundle.yac
YC_BIN = $(YC_BUILD)/yc
YC_A = $(YC_BUILD)/yc_A
YC_B = $(YC_BUILD)/yc_B
YC_SRCS = src-self/log.yac src-self/pass.yac src-self/target.yac src-self/map.yac \
	src-self/lexer.yac src-self/parser.yac src-self/anf.yac \
	src-self/encode_x64.yac src-self/encode_arm64.yac src-self/encode_riscv64.yac \
	src-self/elf.yac src-self/pe.yac src-self/macho.yac src-self/pack.yac \
	src-self/lir.yac src-self/runtime.yac src-self/emit.yac \
	src-self/emit_x86_64.yac src-self/emit_arm64.yac src-self/emit_riscv64.yac \
	src-self/lower.yac src-self/profile.yac src-self/backend.yac src-self/scheme.yac src-self/yc.yac

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

# L4: C interpreter compiles the bundle to native yc_A.
# This is NOT native yc; C GC on the interpreter heap often takes 1–3 min.
# Native compile (yc_A compiling a .yac) is a separate step and should be seconds.
$(YC_A): $(BIN) $(YC_BUNDLE)
	@echo "L4: ./yac compiling bundle with C GC (slow; not the native <3s path)"
	./$(BIN) $(YC_BUNDLE) $(YC_BUNDLE) -o $@
	chmod +x $@

# L5: native yc_A compiles the same bundle. yc_A and yc_B need not match.
$(YC_B): $(YC_A) $(YC_BUNDLE)
	$(YC_A) $(YC_BUNDLE) -o $@
	chmod +x $@

# Convenience name used by scripts; same bits as yc_A.
$(YC_BIN): $(YC_A)
	cp -f $(YC_A) $@
	chmod +x $@

yc: $(YC_A) $(YC_BIN)

# L5 native self-compile. Needs a yc_A built with the bitmap GC.
bootstrap: $(YC_A) $(YC_B) $(YC_BIN)

# yc_A and yc_B must emit the same ELF for a given program (L5 iso).
yc-iso: bootstrap
	$(YC_A) tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoA_l4_42
	$(YC_B) tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoB_l4_42
	cmp -s $(YC_BUILD)/isoA_l4_42 $(YC_BUILD)/isoB_l4_42

# L6: native yc_A compiles and runs the harness. C yac is only a subprocess
# (interp_suite + L4 bootstrap of yc_A inside the harness).
test: $(YC_A)
	mkdir -p build/test_tmp
	$(YC_A) tests/run.yac -o build/test_tmp/run_tests
	chmod +x build/test_tmp/run_tests
	./build/test_tmp/run_tests

test-boot: $(YC_A)
	mkdir -p build/test_tmp
	cp -f $(YC_A) build/test_tmp/yc_A
	$(YC_A) tests/boot/run.yac -o build/test_tmp/boot_run
	chmod +x build/test_tmp/boot_run
	./build/test_tmp/boot_run

prop: $(BIN)
	./yac tests/prop.yac

$(BUILD)/genyac: tools/genyac.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN)
	rm -rf $(BUILD)
	rm -f src/*.o

.PHONY: all clean test test-boot prop yc bootstrap yc-iso

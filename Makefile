CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/value.c src/bignum.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c src/cps.c src/eval_cps.c src/uncps.c src/gc.c src/rtio.c src/ckpt.c src/scheme.c src/profile.c
BUILD = build
OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
BIN = yac
YC_BUILD = $(BUILD)/yc_tmp
YC_BUNDLE = $(YC_BUILD)/yc_bundle.yac
YC_BIN = $(YC_BUILD)/yc
YC_A = $(YC_BUILD)/yc_a
YC_B = $(YC_BUILD)/yc_b
YC_SRCS = src-self/lib/log.yac src-self/lib/pass.yac src-self/lib/map.yac \
	src-self/front/lexer.yac src-self/front/parser.yac src-self/front/anf.yac src-self/front/lir.yac \
	src-self/pack/target.yac src-self/pack/elf.yac src-self/pack/pe.yac src-self/pack/macho.yac src-self/pack/pack.yac \
	src-self/rt/runtime.yac \
	src-self/back/encode/encode_x64.yac src-self/back/encode/encode_arm64.yac src-self/back/encode/encode_riscv64.yac \
	src-self/back/emit/emit.yac \
	src-self/back/emit/emit_x86_64.yac src-self/back/emit/emit_arm64.yac src-self/back/emit/emit_riscv64.yac \
	src-self/back/lower.yac src-self/back/profile.yac src-self/back/backend.yac \
	src-self/lang/scheme.yac src-self/yc.yac

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

# L4: C interpreter compiles the bundle to native yc_a.
# This is NOT native yc; C GC on the interpreter heap often takes 1–3 min.
# Native compile (yc_a compiling a .yac) is a separate step and should be seconds.
$(YC_A): $(BIN) $(YC_BUNDLE)
	@echo "L4: ./yac compiling bundle with C GC (slow; not the native <3s path)"
	./$(BIN) $(YC_BUNDLE) $(YC_BUNDLE) -o $@
	chmod +x $@

# L5: native yc_a compiles the same bundle. yc_a and yc_b need not match.
$(YC_B): $(YC_A) $(YC_BUNDLE)
	$(YC_A) $(YC_BUNDLE) -o $@
	chmod +x $@

# Convenience name used by scripts; same bits as yc_a.
$(YC_BIN): $(YC_A)
	cp -f $(YC_A) $@
	chmod +x $@

yc: $(YC_A) $(YC_BIN)

# L5 native self-compile. Needs a yc_a built with the bitmap GC.
bootstrap: $(YC_A) $(YC_B) $(YC_BIN)

# yc_a and yc_b must emit the same ELF for a given program (L5 iso).
yc-iso: bootstrap
	$(YC_A) tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoA_l4_42
	$(YC_B) tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoB_l4_42
	cmp -s $(YC_BUILD)/isoA_l4_42 $(YC_BUILD)/isoB_l4_42

# L6: native yc_a compiles and runs the harness. C yac is only a subprocess
# for interp_suite. L4 yc_a is reused (copied); harness does not re-run C L4.
test: $(YC_A)
	mkdir -p build/test_tmp
	cp -f $(YC_A) build/test_tmp/yc_a
	chmod +x build/test_tmp/yc_a
	$(YC_A) tests/run.yac -o build/test_tmp/run_tests
	chmod +x build/test_tmp/run_tests
	./build/test_tmp/run_tests

test-boot: $(YC_A)
	mkdir -p build/test_tmp
	cp -f $(YC_A) build/test_tmp/yc_a
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

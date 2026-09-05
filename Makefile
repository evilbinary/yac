ifeq ($(OS),Windows_NT)
EXEEXT := .exe
else
EXEEXT :=
endif

# Built-in yacc `.y` / `%: %.y` treats a target named `yc` as `yc` <- `yc.y`
# and then as itself. We compile C with explicit rules only.
MAKEFLAGS += -r
.SUFFIXES:

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/value.c src/bignum.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c src/cps.c src/eval_cps.c src/uncps.c src/gc.c src/rtio.c src/ckpt.c src/scheme.c src/profile.c src/oscompat.c
BUILD = build
OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
BIN = yac$(EXEEXT)
YC_BUILD = $(BUILD)/yc_tmp
YC_BUNDLE = $(YC_BUILD)/yc_bundle.yac
YC_BIN = yc$(EXEEXT)
YC_A = $(YC_BUILD)/yc_a$(EXEEXT)
YC_B = $(YC_BUILD)/yc_b$(EXEEXT)
# Bootstrap only: concatenate sources. Language rule is one package per .yac
# (see docs/DESIGN.md §3.3); target is compiling src-self/yc.yac via import.
YC_SRCS = src-self/lib/log.yac src-self/lib/pass.yac src-self/lib/map.yac \
	src-self/front/lexer.yac src-self/front/parser.yac src-self/front/anf.yac src-self/front/cps.yac src-self/front/eval_cps.yac src-self/front/uncps.yac src-self/front/lir.yac \
	src-self/rt/runtime.yac \
	src-self/back/pack/target.yac src-self/back/pack/elf.yac src-self/back/pack/dlib.yac src-self/back/pack/pe.yac src-self/back/pack/macho.yac src-self/back/pack/yjit.yac src-self/back/pack/pack.yac \
	src-self/back/encode/encode_x64.yac src-self/back/encode/encode_arm64.yac src-self/back/encode/encode_riscv64.yac \
	src-self/back/emit/emit.yac \
	src-self/back/emit/linux_x86_64.yac src-self/back/emit/win_x86_64.yac \
	src-self/back/emit/emit_cabi.yac src-self/back/emit/emit_x86_64.yac src-self/back/emit/emit_arm64.yac src-self/back/emit/emit_riscv64.yac \
	src-self/back/lower.yac src-self/back/profile.yac src-self/back/backend.yac \
	src-self/back/jit.yac \
	src-self/back/fold.yac \
	src-self/lang/scheme.yac src-self/yc.yac

all: $(BIN) $(YC_BIN)

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

# Native yc concatenates every positional .yac; L4 C yac still takes one file,
# so bootstrap keeps a cat bundle. After yc_a exists, it compiles that bundle.
# Two native passes: pass 1 updates emit in the compiler; pass 2 re-emits
# runtime (win_clock / time_str) with that emit. One pass leaves [00:00:00].
$(YC_A): $(BIN) $(YC_BUNDLE)
	@if [ -x $@ ]; then \
		echo "native rebuild pass 1 (emit): $@ compiling bundle"; \
		$@ --pkg src-self $(YC_BUNDLE) -o $@.new && chmod +x $@.new && \
		echo "native rebuild pass 2 (runtime): $@.new compiling bundle"; \
		$@.new --pkg src-self $(YC_BUNDLE) -o $@.new2 && chmod +x $@.new2 && \
		mv -f $@.new2 $@ && rm -f $@.new; \
	else \
		echo "L4: ./yac compiling bundle with C GC (slow; not the native <3s path)"; \
		./$(BIN) --pkg src-self $(YC_BUNDLE) -o $@ && chmod +x $@; \
	fi

# L5: native yc_a compiles the same bundle. yc_a and yc_b need not match.
$(YC_B): $(YC_A) $(YC_BUNDLE)
	$(YC_A) --pkg src-self $(YC_BUNDLE) -o $@
	chmod +x $@

# Same bits as yc_a, next to ./yac. `make yc` builds this file (not a phony).
$(YC_BIN): $(YC_A)
	cp -f $(YC_A) $@
	chmod +x $@

yc_a: $(YC_A)

yc_b: $(YC_B)

# L5 native self-compile. Needs a yc_a built with the bitmap GC.
bootstrap: $(YC_A) $(YC_B) $(YC_BIN)

# yc_a and yc_b must emit the same ELF for a given program (L5 iso).
yc-iso: bootstrap
	$(YC_A) --pkg src-self tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoA_l4_42$(EXEEXT)
	$(YC_B) --pkg src-self tests/compiler/cases/l4_42.yac -o $(YC_BUILD)/isoB_l4_42$(EXEEXT)
	cmp -s $(YC_BUILD)/isoA_l4_42$(EXEEXT) $(YC_BUILD)/isoB_l4_42$(EXEEXT)

# L6: native yc_a compiles and runs the harness. C yac is only a subprocess
# for interp_suite. L4 yc_a is reused (copied); harness does not re-run C L4.
# Split: make test-interp|compiler|pkg|boot|qemu|qemu-arm64|qemu-riscv64|iso
TEST_TMP = build/test_tmp
TEST_HARNESS = $(TEST_TMP)/run_tests$(EXEEXT)

$(TEST_HARNESS): $(YC_A) tests/run.yac
	mkdir -p $(TEST_TMP)
	cp -f $(YC_A) $(TEST_TMP)/yc_a$(EXEEXT)
	chmod +x $(TEST_TMP)/yc_a$(EXEEXT)
	$(YC_A) --pkg src-self tests/run.yac -o $@
	chmod +x $@

test: $(TEST_HARNESS)
	./$(TEST_HARNESS)

test-interp: $(TEST_HARNESS)
	./$(TEST_HARNESS) interp

test-compiler: $(TEST_HARNESS)
	./$(TEST_HARNESS) compiler

# compiler/cases via yc --cps:  make test-cps   |  make test-cps add  |  CASE=add
# REPL: make test-repl   |  make test-repl import  |  CASE="let a then b"
CASE ?=
test-cps: $(TEST_HARNESS)
	./$(TEST_HARNESS) cps "$(if $(CASE),$(CASE),$(word 2,$(MAKECMDGOALS)))"

test-repl: $(TEST_HARNESS)
	./$(TEST_HARNESS) repl "$(if $(CASE),$(CASE),$(word 2,$(MAKECMDGOALS)))"

SUITE_CASE_TGTS := test-cps test-repl
ifneq ($(filter $(SUITE_CASE_TGTS),$(MAKECMDGOALS)),)
EXTRA_SUITE_CASE := $(filter-out $(SUITE_CASE_TGTS) test test-interp test-compiler test-pkg test-boot test-qemu test-qemu-arm64 test-qemu-riscv64 test-iso prop yc yc_a yc_b bootstrap yc-iso all clean,$(MAKECMDGOALS))
ifneq ($(EXTRA_SUITE_CASE),)
.PHONY: $(EXTRA_SUITE_CASE)
$(EXTRA_SUITE_CASE):
	@:
endif
endif

test-pkg: $(TEST_HARNESS)
	./$(TEST_HARNESS) pkg

test-boot: $(TEST_HARNESS)
	./$(TEST_HARNESS) boot

test-qemu: $(TEST_HARNESS)
	./$(TEST_HARNESS) qemu

test-qemu-arm64: $(TEST_HARNESS)
	./$(TEST_HARNESS) qemu-arm64

test-qemu-riscv64: $(TEST_HARNESS)
	./$(TEST_HARNESS) qemu-riscv64

test-iso: $(TEST_HARNESS)
	./$(TEST_HARNESS) iso

prop: $(BIN)
	./$(BIN) --pkg src-self tests/prop.yac

$(BUILD)/genyac: tools/genyac.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN) $(YC_BIN)
	rm -rf $(BUILD)
	rm -f src/*.o

.PHONY: all clean test test-interp test-compiler test-pkg test-boot \
	test-qemu test-qemu-arm64 test-qemu-riscv64 test-iso test-cps test-repl prop \
	yc_a yc_b bootstrap yc-iso

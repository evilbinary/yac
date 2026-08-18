CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/value.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c src/cps.c src/eval_cps.c src/uncps.c src/gc.c src/rtio.c
BUILD = build
OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
BIN = yac

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	sh tests/run_tests.sh

prop: $(BIN)
	sh tests/prop.sh

$(BUILD)/genyac: tools/genyac.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN)
	rm -rf $(BUILD)
	rm -f src/*.o

.PHONY: all clean test prop

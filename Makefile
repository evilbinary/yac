CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
SRCS = src/main.c src/arena.c src/env.c src/value.c src/lexer.c src/ast.c src/parser.c src/anf.c src/eval_anf.c
OBJS = $(SRCS:.c=.o)
BIN = yac

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./tests/run_tests.sh

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean test

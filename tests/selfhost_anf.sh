#!/usr/bin/env bash
# Self-hosted compiler M2 ANF test.
# Concatenates lexer + parser + anf + driver, normalizes inputs, checks ANF.
# Run from the repo root: sh tests/selfhost_anf.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
TMP=build/anf_tmp
mkdir -p "$TMP"
cat src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/driver_anf.yac > "$TMP/run.yac"

pass=0
fail=0

check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1)); echo "PASS: $desc"
    else
        fail=$((fail + 1)); echo "FAIL: $desc"
        echo "  expected: $expected"
        echo "  actual:   $actual"
    fi
}

anf_case() {
    name="$1"; input="$2"; expected="$3"
    printf '%s\n' "$input" > "$TMP/in.yac"
    actual=$($BIN "$TMP/run.yac" 2>/dev/null | grep -v '^0$' | tr -d '\r')
    check "$name" "$expected" "$actual"
}

anf_case "literal" '42' '[[], [int, 42]]'
anf_case "var" 'x' '[[], [var, x]]'
anf_case "addition" '1 + 2' '[[[letbin, t0, +, [int, 1], [int, 2]]], [var, t0]]'
anf_case "precedence" '1 + 2 * 3' '[[[letbin, t0, *, [int, 2], [int, 3]], [letbin, t1, +, [int, 1], [var, t0]]], [var, t1]]'
anf_case "if" 'if x then 1 else 2' '[[[letif, t0, [var, x], [[], [int, 1]], [[], [int, 2]]]], [var, t0]]'
anf_case "call" 'f(1, 2)' '[[[letcall, t0, [var, f], [[int, 1], [int, 2]]]], [var, t0]]'
anf_case "lambda" 'fun (a) -> a + 1' '[[[letfun, t1, [a], [[letbin, t0, +, [var, a], [int, 1]]]]], [var, t1]]'
anf_case "let bind" 'let x = 5 in x + 1' '[[[let, x, [int, 5]], [letbin, t0, +, [var, x], [int, 1]]], [var, t0]]'
anf_case "let if" 'let x = if c then 1 else 2 in x + 1' '[[[letif, t0, [var, c], [[], [int, 1]], [[], [int, 2]]], [let, x, [var, t0]], [letbin, t1, +, [var, x], [int, 1]]], [var, t1]]'
anf_case "nested if" 'if a then if b then 1 else 2 else 3' '[[[letif, t1, [var, a], [[[letif, t0, [var, b], [[], [int, 1]], [[], [int, 2]]]], [var, t0]], [[], [int, 3]]]], [var, t1]]'
anf_case "recursive fun" 'let f(n) = if n == 0 then 0 else f(n - 1)' '[[[letfun, t4, [n], [[letbin, t0, ==, [var, n], [int, 0]], [letif, t3, [var, t0], [[], [int, 0]], [[[letbin, t1, -, [var, n], [int, 1]], [letcall, t2, [var, f], [[var, t1]]]], [var, t2]]]]], [let, f, [var, t4]]], [unit]]'
anf_case "curried closure" 'let y = fun (a) -> fun (b) -> a + b in y(1)' '[[[letfun, t2, [a], [[letfun, t1, [b], [[letbin, t0, +, [var, a], [var, b]]]]]], [let, y, [var, t2]], [letcall, t3, [var, y], [[int, 1]]]], [var, t3]]'
anf_case "unary minus" '-5' '[[[letbin, t0, -, [int, 0], [int, 5]]], [var, t0]]'

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

#!/usr/bin/env bash
# Self-hosted compiler M2 parser test.
# Concatenates lexer + parser + driver, parses inputs, checks the AST.
# Run from the repo root: sh tests/selfhost_parser.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
TMP=build/parse_tmp
mkdir -p "$TMP"
cat src-self/lexer.yac src-self/parser.yac src-self/driver_parse.yac > "$TMP/run.yac"

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

# parse_case name input expected
parse_case() {
    name="$1"; input="$2"; expected="$3"
    printf '%s\n' "$input" > "$TMP/in.yac"
    actual=$($BIN "$TMP/run.yac" 2>/dev/null | grep -v '^0$' | tr -d '\r')
    check "$name" "$expected" "$actual"
}

parse_case "int literal" '42' '[[int, 42]]'
parse_case "var" 'x' '[[var, x]]'
parse_case "string" '"hi"' '[[str, hi]]'
parse_case "bool" 'true' '[[bool, true]]'
parse_case "addition" '1 + 2' '[[binop, +, [int, 1], [int, 2]]]'
parse_case "precedence" '1 + 2 * 3' '[[binop, +, [int, 1], [binop, *, [int, 2], [int, 3]]]]'
parse_case "left assoc" 'a - b - c' '[[binop, -, [binop, -, [var, a], [var, b]], [var, c]]]'
parse_case "and or" 'a and b or c' '[[binop, or, [binop, and, [var, a], [var, b]], [var, c]]]'
parse_case "comparison" 'x <= 1' '[[binop, <=, [var, x], [int, 1]]]'
parse_case "unary minus" '-1' '[[binop, -, [int, 0], [int, 1]]]'
parse_case "unary minus mul" '-x * 2' '[[binop, *, [binop, -, [int, 0], [var, x]], [int, 2]]]'
parse_case "call" 'f(1, 2)' '[[call, [var, f], [[int, 1], [int, 2]]]]'
parse_case "nested call" 'f(g(1))' '[[call, [var, f], [[call, [var, g], [[int, 1]]]]]]'
parse_case "not" 'not true' '[[not, [bool, true]]]'
parse_case "list literal" '[1, 2, 3]' '[[list, [[int, 1], [int, 2], [int, 3]]]]'
parse_case "if" 'if a then b else c' '[[if, [var, a], [var, b], [var, c]]]'
parse_case "lambda" 'fun (x) -> x + 1' '[[fun, [x], [binop, +, [var, x], [int, 1]]]]'
parse_case "lambda multi param" 'fun (a, b) -> a * b' '[[fun, [a, b], [binop, *, [var, a], [var, b]]]]'
parse_case "let bind" 'let x = 5 in x + 1' '[[let, x, [int, 5], [binop, +, [var, x], [int, 1]]]]'
parse_case "let fun" 'let f(x) = if x == 0 then 0 else f(x - 1)' '[[let, f, [fun, [x], [if, [binop, ==, [var, x], [int, 0]], [int, 0], [call, [var, f], [[binop, -, [var, x], [int, 1]]]]]], [unit]]]'
parse_case "let empty params" 'let f() = 42 in f()' '[[let, f, [fun, [], [int, 42]], [call, [var, f], []]]]'
parse_case "multi statement" 'let x = 1;
let y = 2;
x + y' '[[let, x, [int, 1], [unit]], [let, y, [int, 2], [unit]], [binop, +, [var, x], [var, y]]]'
parse_case "factorial" 'let fact(n) = if n <= 1 then 1 else n * fact(n - 1)
print fact(5)' '[[let, fact, [fun, [n], [if, [binop, <=, [var, n], [int, 1]], [int, 1], [binop, *, [var, n], [call, [var, fact], [[binop, -, [var, n], [int, 1]]]]]]], [unit]], [print, [call, [var, fact], [[int, 5]]]]]'

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

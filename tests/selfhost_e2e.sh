#!/usr/bin/env bash
# Self-hosted compiler M3.3: full source -> ELF end-to-end.
# Compiles yac source (parse->anf->lower->emit->elf) to a native executable
# and checks its exit code.
# Run from the repo root: sh tests/selfhost_e2e.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
TMP=build/lower_tmp
mkdir -p "$TMP"
SRC='src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac src-self/emit.yac src-self/backend.yac src-self/lower.yac src-self/driver_lower.yac'
cat $SRC > "$TMP/run.yac"

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

# compile source, run, report exit code (8-bit)
run_e2e() {
    name="$1"; src="$2"; want="$3"
    printf '%s\n' "$src" > "$TMP/in.yac"
    if ! $BIN "$TMP/run.yac" >/dev/null 2>&1; then
        fail=$((fail + 1)); echo "FAIL: $name (compile error)"
        return
    fi
    chmod +x "$TMP/out.bin"
    "$TMP/out.bin"
    rc=$?
    check "$name" "$want" "$rc"
}

run_e2e "e2e add" '1 + 2' '3'
run_e2e "e2e let" 'let x = 5 in x + 1' '6'
run_e2e "e2e precedence" '1 + 2 * 3' '7'
run_e2e "e2e div" '17 / 5' '3'
run_e2e "e2e mod" '17 % 5' '2'
run_e2e "e2e if true" 'if 1 < 2 then 7 else 9' '7'
run_e2e "e2e if false" 'if 5 < 2 then 7 else 9' '9'
run_e2e "e2e func call" 'let f(n) = n * 2 in f(5)' '10'
run_e2e "e2e recursion" 'let f(n) = if n <= 1 then 1 else n * f(n - 1) in f(5)' '120'

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

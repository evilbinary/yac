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
SRC='src-self/log.yac src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac src-self/emit.yac src-self/backend.yac src-self/lower.yac src-self/driver_lower.yac'
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
run_e2e "e2e 2 args" 'let add(a, b) = a + b in add(4, 5)' '9'
run_e2e "e2e 3 args" 'let add3(a, b, c) = a + b + c in add3(1, 2, 3)' '6'
run_e2e "e2e 6 args" 'let f(a, b, c, d, e, g) = a + b + c + d + e + g in f(1, 2, 3, 4, 5, 6)' '21'
run_e2e "e2e capture" 'let x = 10 in (fun(n) -> n + x)(5)' '15'
run_e2e "e2e multi-capture" 'let x = 1 in let y = 2 in (fun(n) -> n + x + y)(10)' '13'
run_e2e "e2e capture+2args" 'let x = 100 in (fun(a, b) -> a + b + x)(1, 2)' '103'
run_e2e "e2e higher" 'let twice(f, x) = f(f(x)) in let inc(n) = n + 1 in twice(inc, 5)' '7'
run_e2e "e2e curry" 'let add(a) = fun(b) -> a + b in let g = add(3) in g(4)' '7'
run_e2e "e2e return clo" 'let mk(x) = fun(n) -> n + x in mk(10)(5)' '15'
run_e2e "e2e list len" 'len([1, 2, 3])' '3'
run_e2e "e2e list nth" 'nth([10, 20, 30], 1)' '20'
run_e2e "e2e list cons" 'len(cons(0, [1, 2]))' '3'
run_e2e "e2e foldl" 'let foldl(f, acc, xs) = if len(xs) == 0 then acc else foldl(f, f(acc, nth(xs, 0)), tail(xs)) in foldl(fun(a, x) -> a + x, 0, [1, 2, 3, 4])' '10'
run_e2e "e2e map len" 'let map(f, xs) = if len(xs) == 0 then [] else cons(f(nth(xs, 0)), map(f, tail(xs))) in let inc(n) = n + 1 in len(map(inc, [1, 2, 3]))' '3'
run_e2e "e2e map nth" 'let map(f, xs) = if len(xs) == 0 then [] else cons(f(nth(xs, 0)), map(f, tail(xs))) in let inc(n) = n + 1 in nth(map(inc, [1, 2, 3]), 2)' '4'
run_e2e "e2e filter" 'let gt1(x) = x > 1 in let filter(f, xs) = if len(xs) == 0 then [] else let x = nth(xs, 0) in let keep = f(x) in let rest = filter(f, tail(xs)) in if keep then cons(x, rest) else rest in len(filter(gt1, [1, 2, 3, 0]))' '2'
run_e2e "e2e append" 'nth(append([1, 2], [3, 4]), 3)' '4'
run_e2e "e2e drop" 'nth(drop([1, 2, 3, 4], 2), 0)' '3'
run_e2e "e2e str_len" 'str_len("abc")' '3'
run_e2e "e2e str_ref" 'str_ref("Hi", 1)' '105'
run_e2e "e2e str_cat" 'str_len(str_cat("ab", "cd"))' '4'
run_e2e "e2e band" 'band(7, 3)' '3'
run_e2e "e2e bytes" 'let b = bytes_new() in let _ = bytes_append(b, 65) in let _ = bytes_append(b, 66) in bytes_ref(b, 1)' '66'
run_e2e "e2e int_to_str" 'str_ref(int_to_str(42), 1)' '50'
run_e2e "e2e str_slice" 'str_ref(str_slice("abcdef", 2, 3), 0)' '99'
run_e2e "e2e bytes_to_str" 'let b = bytes_new() in let _ = bytes_append(b, 90) in str_ref(bytes_to_str(b), 0)' '90'
run_e2e "e2e str_eq" 'if "ab" == "ab" then 1 else 0' '1'
run_e2e "e2e str_neq" 'if "ab" == "cd" then 1 else 0' '0'
run_e2e "e2e bool" 'if true then 7 else 9' '7'
run_e2e "e2e and" 'if (1 < 2 and 3 < 4) then 5 else 0' '5'
run_e2e "e2e foldl prim" 'foldl(fun(a, x) -> a + x, 0, [1, 2, 3, 4])' '10'
run_e2e "e2e map prim" 'nth(map(fun(x) -> x + 1, [1, 2, 3]), 2)' '4'
run_e2e "e2e is_kw" 'let w = "let" in foldl(fun(a, k) -> if a then true else w == k, false, ["if", "let", "in"])' '1'

# print (decimal output) — compare stdout, not exit code
run_print() {
    name="$1"; src="$2"; want="$3"
    printf '%s\n' "$src" > "$TMP/in.yac"
    if ! $BIN "$TMP/run.yac" >/dev/null 2>&1; then
        fail=$((fail + 1)); echo "FAIL: $name (compile error)"
        return
    fi
    chmod +x "$TMP/out.bin"
    actual=$("$TMP/out.bin")
    check "$name" "$want" "$actual"
}
run_print "e2e print int" 'print 123' '123'
run_print "e2e print fact10" 'let f(n) = if n <= 1 then 1 else n * f(n - 1) in print f(10)' '3628800'

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

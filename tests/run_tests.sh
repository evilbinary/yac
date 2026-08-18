#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

BIN=./yac
pass=0
fail=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1))
        echo "PASS: $desc"
    else
        fail=$((fail + 1))
        echo "FAIL: $desc"
        echo "  expected: $expected"
        echo "  actual:   $actual"
    fi
}

actual=$($BIN tests/fact.yac);      check "factorial"         "3628800" "$actual"
actual=$($BIN tests/fib.yac);       check "fibonacci"         "832040" "$actual"
actual=$($BIN tests/higher.yac);    check "higher-order"      "7" "$actual"
actual=$($BIN tests/types.yac | tr -d '\r');  check "types"             $'hello\n3.5\ntrue\n()\n42' "$actual"
actual=$($BIN tests/tco.yac);       check "tco"               "0" "$actual"
actual=$($BIN tests/closure.yac);   check "lexical closure"   "2" "$actual"
actual=$($BIN tests/float.yac);     check "float arithmetic"  "1" "$actual"

out=$($BIN tests/unbound.yac 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "unbound variable 'y'"; then
    pass=$((pass + 1)); echo "PASS: unbound variable detected"
else
    fail=$((fail + 1)); echo "FAIL: unbound variable not detected (rc=$rc)"
    echo "  $out"
fi

out=$($BIN tests/callcc.yac 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "CPS mode"; then
    pass=$((pass + 1)); echo "PASS: callcc rejected by ANF machine"
else
    fail=$((fail + 1)); echo "FAIL: callcc should be rejected by ANF machine (rc=$rc)"
    echo "  $out"
fi

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
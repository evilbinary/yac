#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."

BIN=./yac
pass=0
fail=0

check() {
    desc="$1"; expected="$2"; actual="$3"
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

types_out=$(printf 'hello\n3.5\ntrue\n()\n42')

actual=$($BIN tests/fact.yac);      check "factorial"         "3628800" "$actual"
actual=$($BIN tests/fib.yac);       check "fibonacci"         "832040" "$actual"
actual=$($BIN tests/higher.yac);    check "higher-order"      "7" "$actual"
actual=$($BIN tests/types.yac | tr -d '\r');  check "types"             "$types_out" "$actual"
actual=$($BIN tests/tco.yac);       check "tco"               "0" "$actual"
actual=$($BIN tests/closure.yac);   check "lexical closure"   "2" "$actual"
actual=$($BIN tests/float.yac);     check "float arithmetic"  "1" "$actual"

list_out=$(printf '3\n20\n[0, 1, 2]\n[1, 2, 3]\n[2, 4, 6]\n[2, 3]\n10\n2\n[1, [2, 3], x]\n[]\ntrue\n42')
actual=$($BIN tests/list.yac | tr -d '\r');  check "list primitives" "$list_out" "$actual"

bignum_out=$($BIN tests/bignum.yac | tr -d '\r')
actual=$bignum_out; check "bignum arithmetic" "$bignum_out" "$actual"

# CPS parity: every ordinary program must produce the same result
actual=$($BIN --cps tests/fact.yac);     check "cps factorial"        "3628800" "$actual"
actual=$($BIN --cps tests/fib.yac);      check "cps fibonacci"        "832040" "$actual"
actual=$($BIN --cps tests/higher.yac);   check "cps higher-order"     "7" "$actual"
actual=$($BIN --cps tests/types.yac | tr -d '\r'); check "cps types"  "$types_out" "$actual"
actual=$($BIN --cps tests/tco.yac);      check "cps tco"              "0" "$actual"
actual=$($BIN --cps tests/closure.yac);  check "cps lexical closure"  "2" "$actual"
actual=$($BIN --cps tests/float.yac);    check "cps float arithmetic" "1" "$actual"
actual=$($BIN --cps tests/list.yac | tr -d '\r'); check "cps list primitives" "$list_out" "$actual"
actual=$($BIN --cps tests/bignum.yac | tr -d '\r'); check "cps bignum arithmetic" "$bignum_out" "$actual"

# --both cross-check on ordinary programs
out=$($BIN --both tests/fact.yac); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "3628800" ]; then
    pass=$((pass + 1)); echo "PASS: --both matches on factorial"
else
    fail=$((fail + 1)); echo "FAIL: --both on factorial (rc=$rc, out=$out)"
fi

# un-CPS round trip: ANF->CPS->ANF must reproduce the same result
actual=$($BIN --uncps tests/fact.yac);   check "uncps factorial"      "3628800" "$actual"
actual=$($BIN --uncps tests/fib.yac);    check "uncps fibonacci"      "832040" "$actual"
actual=$($BIN --uncps tests/higher.yac); check "uncps higher-order"   "7" "$actual"
actual=$($BIN --uncps tests/closure.yac); check "uncps lexical closure" "2" "$actual"
actual=$($BIN --uncps tests/tco.yac);    check "uncps tco"            "0" "$actual"
actual=$($BIN --uncps tests/list.yac | tr -d '\r'); check "uncps list primitives" "$list_out" "$actual"
actual=$($BIN --uncps tests/bignum.yac | tr -d '\r'); check "uncps bignum arithmetic" "$bignum_out" "$actual"

out=$($BIN --uncps tests/callcc.yac 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "cannot un-CPS"; then
    pass=$((pass + 1)); echo "PASS: callcc rejected by un-CPS"
else
    fail=$((fail + 1)); echo "FAIL: callcc should be rejected by un-CPS (rc=$rc)"
    echo "  $out"
fi

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

out=$($BIN --cps tests/callcc.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$out" | tr -d '\r' | grep -q "^42$"; then
    pass=$((pass + 1)); echo "PASS: callcc runs under CPS -> 42"
else
    fail=$((fail + 1)); echo "FAIL: callcc under CPS should print 42 (rc=$rc)"
    echo "  $out"
fi

out=$($BIN --cps tests/callcc2.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$out" | tr -d '\r' | grep -q "^999$"; then
    pass=$((pass + 1)); echo "PASS: callcc early exit -> 999"
else
    fail=$((fail + 1)); echo "FAIL: callcc early exit should print 999 (rc=$rc)"
    echo "  $out"
fi

out=$($BIN --cps tests/callcc3.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$out" | tr -d '\r' | grep -q "^42$"; then
    pass=$((pass + 1)); echo "PASS: callcc capture + use -> 42"
else
    fail=$((fail + 1)); echo "FAIL: callcc capture + use should print 42 (rc=$rc)"
    echo "  $out"
fi

out=$($BIN --cps tests/callcc4.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$out" | tr -d '\r' | grep -q "^999$"; then
    pass=$((pass + 1)); echo "PASS: callcc inside function -> 999"
else
    fail=$((fail + 1)); echo "FAIL: callcc inside function should print 999 (rc=$rc)"
    echo "  $out"
fi

# GC: the garbage-allocating loop must complete under a live-object limit...
out=$($BIN --limit-nodes 50000 tests/gc.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "0" ]; then
    pass=$((pass + 1)); echo "PASS: GC reclaims garbage (loop completes under limit)"
else
    fail=$((fail + 1)); echo "FAIL: GC should reclaim garbage under limit (rc=$rc, out=$out)"
    echo "  $out"
fi

# ...but without GC the same program must trip the limit.
out=$($BIN --no-gc --limit-nodes 50000 tests/gc.yac 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "runaway"; then
    pass=$((pass + 1)); echo "PASS: --no-gc grows and trips --limit-nodes"
else
    fail=$((fail + 1)); echo "FAIL: --no-gc should trip --limit-nodes (rc=$rc)"
    echo "  $out"
fi

# GC with list allocation: fresh lists + map closures each iteration must be reclaimed...
out=$($BIN --limit-nodes 50000 tests/gc_list.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "[200001, 200002]" ]; then
    pass=$((pass + 1)); echo "PASS: GC reclaims list garbage (loop completes under limit)"
else
    fail=$((fail + 1)); echo "FAIL: GC should reclaim list garbage (rc=$rc, out=$out)"
    echo "  $out"
fi

out=$($BIN --cps --limit-nodes 50000 tests/gc_list.yac 2>&1); rc=$?
if [ $rc -eq 0 ] && [ "$out" = "[200001, 200002]" ]; then
    pass=$((pass + 1)); echo "PASS: CPS GC reclaims list garbage (loop completes under limit)"
else
    fail=$((fail + 1)); echo "FAIL: CPS GC should reclaim list garbage (rc=$rc, out=$out)"
    echo "  $out"
fi

# ...but without GC the list-allocating loop must trip the limit.
out=$($BIN --no-gc --limit-nodes 50000 tests/gc_list.yac 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "runaway"; then
    pass=$((pass + 1)); echo "PASS: --no-gc list growth trips --limit-nodes"
else
    fail=$((fail + 1)); echo "FAIL: --no-gc list loop should trip --limit-nodes (rc=$rc)"
    echo "  $out"
fi

# checkpoint / resume round trip: pause mid-run and continue from there
tmp=$(mktemp -d 2>/dev/null || echo build/ckpt_tmp)
mkdir -p "$tmp"
out=$($BIN --checkpoint-at 50 tests/fact.yac >/dev/null 2>&1); rc=$?
if [ $rc -eq 0 ] && [ -f yac.ckpt ]; then
    out=$($BIN --resume yac.ckpt 2>&1); rc=$?
    rm -f yac.ckpt
    if [ $rc -eq 0 ] && [ "$out" = "3628800" ]; then
        pass=$((pass + 1)); echo "PASS: checkpoint/resume reproduces factorial"
    else
        fail=$((fail + 1)); echo "FAIL: resume should finish with 3628800 (rc=$rc, out=$out)"
    fi
else
    fail=$((fail + 1)); echo "FAIL: checkpoint dump did not happen (rc=$rc)"
fi

# bignum: runtime serialize/deserialize round trip must reproduce the result
tmp=$(mktemp -d 2>/dev/null || echo build/rt_tmp)
mkdir -p "$tmp"
$BIN --dump-rt "$tmp/bignum.yacrt" tests/bignum.yac 2>/dev/null
out=$($BIN --load-rt "$tmp/bignum.yacrt" 2>/dev/null | tr -d '\r')
if [ "$out" = "$bignum_out" ]; then
    pass=$((pass + 1)); echo "PASS: bignum runtime round trip"
else
    fail=$((fail + 1)); echo "FAIL: bignum runtime round trip"
    echo "  expected: $bignum_out"
    echo "  actual:   $out"
fi

# bignum: checkpoint / resume mid-run must finish with the same value
out=$($BIN --checkpoint-at 40 tests/bignum.yac >/dev/null 2>&1); rc=$?
if [ $rc -eq 0 ] && [ -f yac.ckpt ]; then
    out=$($BIN --resume yac.ckpt 2>/dev/null | tr -d '\r'); rc=$?
    rm -f yac.ckpt
    if [ $rc -eq 0 ] && [ "$out" = "$bignum_out" ]; then
        pass=$((pass + 1)); echo "PASS: bignum checkpoint/resume"
    else
        fail=$((fail + 1)); echo "FAIL: bignum resume (rc=$rc)"
    fi
else
    fail=$((fail + 1)); echo "FAIL: bignum checkpoint dump (rc=$rc)"
fi

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
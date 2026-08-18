#! /bin/sh
# Property tests: random programs must behave identically under ANF and CPS.
# Programs with callcc run only under the CPS machine.
# Usage: prop.sh [N ordinary] [M callcc]
set -u
cd "$(dirname "$0")/.."

BIN=./yac
GEN=./build/genyac
N=${1:-200}
M=${2:-40}

mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -o build/genyac tools/genyac.c || { echo "genyac build failed"; exit 1; }

tmp=$(mktemp -d 2>/dev/null || echo build/prop_tmp)
mkdir -p "$tmp"
pass=0
fail=0

i=0
while [ $i -lt $N ]; do
    i=$((i + 1))
    seed=$((i * 10007 + 1))
    $GEN $seed > "$tmp/p.yac" 2>/dev/null
    o1=$($BIN "$tmp/p.yac" 2>/dev/null); r1=$?
    e1=$($BIN "$tmp/p.yac" 2>&1 1>/dev/null)
    o2=$($BIN --cps "$tmp/p.yac" 2>/dev/null); r2=$?
    e2=$($BIN --cps "$tmp/p.yac" 2>&1 1>/dev/null)
    o3=$($BIN --opt --cps "$tmp/p.yac" 2>/dev/null); r3=$?
    e3=$($BIN --opt --cps "$tmp/p.yac" 2>&1 1>/dev/null)
    if [ "$o1" = "$o2" ] && [ "$e1" = "$e2" ] && [ $r1 -eq $r2 ] &&
       [ "$o2" = "$o3" ] && [ "$e2" = "$e3" ] && [ $r2 -eq $r3 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL seed=$seed"
        echo "  src: $(cat "$tmp/p.yac")"
        echo "  anf(rc=$r1) out=[$o1] err=[$e1]"
        echo "  cps(rc=$r2) out=[$o2] err=[$e2]"
        echo "  opt(rc=$r3) out=[$o3] err=[$e3]"
    fi
    # dump-rt / load-rt round trip must reproduce the same result
    $BIN --dump-rt "$tmp/p.yacrt" "$tmp/p.yac" 2>/dev/null
    o4=$($BIN --load-rt "$tmp/p.yacrt" 2>/dev/null); r4=$?
    e4=$($BIN --load-rt "$tmp/p.yacrt" 2>&1 1>/dev/null)
    if [ "$o1" = "$o4" ] && [ "$e1" = "$e4" ] && [ $r1 -eq $r4 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL(rt) seed=$seed"
        echo "  direct(rc=$r1) out=[$o1] err=[$e1]"
        echo "  loaded(rc=$r4) out=[$o4] err=[$e4]"
    fi
done

# callcc programs: only the CPS machine runs them; they must terminate
# normally (rc 0 = success, rc 1 = a plain runtime error in the random body).
i=0
while [ $i -lt $M ]; do
    i=$((i + 1))
    seed=$((i * 9999 + 7))
    $GEN --callcc $seed > "$tmp/c.yac" 2>/dev/null
    $BIN --cps "$tmp/c.yac" >/dev/null 2>&1
    rc=$?
    if [ $rc -le 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL(callcc) seed=$seed rc=$rc src=$(cat "$tmp/c.yac")"
    fi
done

rm -rf "$tmp"
echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
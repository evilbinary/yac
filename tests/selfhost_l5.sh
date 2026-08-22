#!/usr/bin/env bash
# L5: native yc_A compiles yc.yac → yc_B; A and B emit identical fact/42 ELFs.
set -u
cd "$(dirname "$0")/.."
TMP=build/yc_tmp
mkdir -p "$TMP"

cat src-self/log.yac src-self/lexer.yac src-self/parser.yac src-self/anf.yac \
    src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac \
    src-self/emit.yac src-self/backend.yac src-self/lower.yac \
    src-self/yc.yac > "$TMP/yc_bundle.yac"

./yac "$TMP/yc_bundle.yac" "$TMP/yc_bundle.yac" -o "$TMP/yc_A" >/dev/null 2>&1 || true
if [ ! -f "$TMP/yc_A" ]; then
  echo "FAIL: L4 did not write yc_A"
  exit 1
fi
chmod +x "$TMP/yc_A"

"$TMP/yc_A" "$TMP/yc_bundle.yac" -o "$TMP/yc_B" >/dev/null 2>&1 || true
if [ ! -f "$TMP/yc_B" ]; then
  echo "FAIL: L5 yc_A did not write yc_B"
  exit 1
fi
chmod +x "$TMP/yc_B"

printf '%s\n' '42' > "$TMP/p42.yac"
printf '%s\n' 'let f(n) = if n <= 1 then 1 else n * f(n - 1) in f(5)' > "$TMP/fact.yac"

"$TMP/yc_A" "$TMP/p42.yac" -o "$TMP/p42_A.bin" >/dev/null 2>&1
"$TMP/yc_B" "$TMP/p42.yac" -o "$TMP/p42_B.bin" >/dev/null 2>&1
"$TMP/yc_A" "$TMP/fact.yac" -o "$TMP/fact_A.bin" >/dev/null 2>&1
"$TMP/yc_B" "$TMP/fact.yac" -o "$TMP/fact_B.bin" >/dev/null 2>&1
chmod +x "$TMP/p42_A.bin" "$TMP/p42_B.bin" "$TMP/fact_A.bin" "$TMP/fact_B.bin"

fail=0
"$TMP/p42_B.bin"; rc=$?
if [ "$rc" != "42" ]; then echo "FAIL: yc_B 42 got $rc"; fail=1; fi
"$TMP/fact_B.bin"; rc=$?
if [ "$rc" != "120" ]; then echo "FAIL: yc_B fact got $rc"; fail=1; fi
if ! cmp -s "$TMP/p42_A.bin" "$TMP/p42_B.bin"; then
  echo "FAIL: 42.bin differs between yc_A and yc_B"; fail=1
fi
if ! cmp -s "$TMP/fact_A.bin" "$TMP/fact_B.bin"; then
  echo "FAIL: fact.bin differs between yc_A and yc_B"; fail=1
fi
if [ "$fail" != "0" ]; then exit 1; fi
echo "PASS: L5 yc_B fact(5)=120 and A/B outputs identical"
exit 0

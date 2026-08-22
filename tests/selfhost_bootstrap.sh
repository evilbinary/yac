#!/usr/bin/env bash
# L4/L5 bootstrap:
#   yac compiles bundle → yc_A
#   yc_A compiles bundle → yc_B
# Same e2e cases run on yac (interpreter pipeline), yc_A, and yc_B.
# yc_A and yc_B must emit byte-identical programs (no .bin suffix).
set -u
cd "$(dirname "$0")/.."
TMP=build/yc_tmp
mkdir -p "$TMP" "$TMP/iso_A" "$TMP/iso_B"

cat src-self/log.yac src-self/lexer.yac src-self/parser.yac src-self/anf.yac \
    src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac \
    src-self/emit.yac src-self/backend.yac src-self/lower.yac \
    src-self/yc.yac > "$TMP/yc_bundle.yac"

echo "==> yac → yc_A"
./yac "$TMP/yc_bundle.yac" "$TMP/yc_bundle.yac" -o "$TMP/yc_A" >/dev/null 2>&1 || true
if [ ! -f "$TMP/yc_A" ]; then
  echo "FAIL: yac did not write yc_A"
  exit 1
fi
if [ -f "$TMP/yc_A.bin" ]; then
  echo "FAIL: wrote yc_A.bin; output should be named yc_A"
  exit 1
fi
chmod +x "$TMP/yc_A"

if [ "${SKIP_YAC_E2E:-}" != "1" ]; then
  echo "==> e2e via yac"
  sh tests/selfhost_e2e.sh || exit 1
fi

echo "==> e2e via yc_A"
sh tests/selfhost_e2e.sh "$TMP/yc_A" || exit 1

echo "==> yc_A → yc_B"
"$TMP/yc_A" "$TMP/yc_bundle.yac" -o "$TMP/yc_B" >/dev/null 2>&1 || true
if [ ! -f "$TMP/yc_B" ]; then
  echo "FAIL: yc_A did not write yc_B"
  exit 1
fi
if [ -f "$TMP/yc_B.bin" ]; then
  echo "FAIL: wrote yc_B.bin; output should be named yc_B"
  exit 1
fi
chmod +x "$TMP/yc_B"

echo "==> e2e via yc_B"
sh tests/selfhost_e2e.sh "$TMP/yc_B" || exit 1

echo "==> default output name (no .bin)"
printf '%s\n' '42' > "$TMP/p42.yac"
rm -f "$TMP/p42" "$TMP/p42.bin"
"$TMP/yc_A" "$TMP/p42.yac" >/dev/null 2>&1
if [ -f "$TMP/p42.bin" ] || [ ! -f "$TMP/p42" ]; then
  echo "FAIL: default output should be p42, not p42.bin"
  exit 1
fi
chmod +x "$TMP/p42"
"$TMP/p42"; rc=$?
if [ "$rc" != "42" ]; then
  echo "FAIL: default-named p42 got $rc"
  exit 1
fi
echo "PASS: default output p42 (no .bin)"

echo "==> yc_A vs yc_B identical ELFs"
iso_fail=0
e2e_case() {
  kind="$1"; name="$2"; src="$3"; want="$4"
  slug=$(printf '%s' "$name" | tr ' /' '__')
  printf '%s\n' "$src" > "$TMP/iso.yac"
  "$TMP/yc_A" "$TMP/iso.yac" -o "$TMP/iso_A/$slug" >/dev/null 2>&1
  "$TMP/yc_B" "$TMP/iso.yac" -o "$TMP/iso_B/$slug" >/dev/null 2>&1
  if ! cmp -s "$TMP/iso_A/$slug" "$TMP/iso_B/$slug"; then
    echo "FAIL: $name ELF differs between yc_A and yc_B"
    iso_fail=1
  fi
}
E2E_TMP=build/lower_tmp
# shellcheck disable=SC1091
. tests/selfhost_e2e_cases.sh
if [ "$iso_fail" != "0" ]; then
  exit 1
fi
echo "PASS: yc_A and yc_B emit identical programs"

echo "PASS: L5 yac / yc_A / yc_B e2e + A/B isomorphism"
exit 0

#!/usr/bin/env bash
# M4: yc.yac under the C interpreter compiles fact → native ELF.
set -u
cd "$(dirname "$0")/.."
TMP=build/yc_tmp
mkdir -p "$TMP"

cat src-self/log.yac src-self/lexer.yac src-self/parser.yac src-self/anf.yac \
    src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac \
    src-self/emit.yac src-self/backend.yac src-self/lower.yac \
    src-self/yc.yac > "$TMP/yc_bundle.yac"

printf '%s\n' 'let f(n) = if n <= 1 then 1 else n * f(n - 1) in f(5)' > "$TMP/in.yac"

./yac "$TMP/yc_bundle.yac" "$TMP/in.yac" -o "$TMP/out.bin" >/dev/null 2>&1 || true
if [ ! -x "$TMP/out.bin" ] && [ ! -f "$TMP/out.bin" ]; then
  echo "FAIL: yc.yac did not write out.bin"
  exit 1
fi
chmod +x "$TMP/out.bin"
"$TMP/out.bin"
rc=$?
if [ "$rc" = "120" ]; then
  echo "PASS: yc.yac fact(5)=120"
  exit 0
fi
echo "FAIL: got $rc want 120"
exit 1

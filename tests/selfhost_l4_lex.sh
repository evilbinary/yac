#!/usr/bin/env bash
# M4 L4 smoke: native ELF of lexer pieces via yc pipeline.
set -u
cd "$(dirname "$0")/.."
TMP=build/l4_iskw_tmp
mkdir -p "$TMP"
fail=0

SRC='src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac src-self/emit.yac src-self/backend.yac src-self/lower.yac'

run_case() {
  local name="$1" prog="$2" want="$3"
  printf '%s\n' "$prog" > "$TMP/prog.yac"
  cat $SRC > "$TMP/compiler.yac"
  cat >> "$TMP/compiler.yac" <<EOF
let src = read_file("$TMP/prog.yac");
let toks = tokenize(src);
let prog = parse_program(toks, 0, []);
let anf_item(item) = let r = anf_expr(item, 0) in [nth(r, 0), nth(r, 1)];
let elf = backend_compile(compile_top(map(anf_item, prog)));
let _ = write_file("$TMP/out.bin", elf) in
0
EOF
  ./yac "$TMP/compiler.yac" >/dev/null 2>&1 || {
    echo "FAIL: $name compile error"
    fail=1
    return
  }
  chmod +x "$TMP/out.bin"
  "$TMP/out.bin"
  local rc=$?
  if [ "$rc" = "$want" ]; then
    echo "PASS: L4 native $name=$want"
  else
    echo "FAIL: $name got $rc want $want"
    fail=1
  fi
}

# foldl + str == + capture
run_case is_kw \
  'let w = "let" in foldl(fun(a, k) -> if a then true else w == k, false, ["if", "let", "in"])' \
  1

# Full lexer.yac linked: native tokenize
cat src-self/lexer.yac > "$TMP/prog.yac"
cat >> "$TMP/prog.yac" <<'EOF'
let main = len(tokenize("let x = 1")) in main
EOF
cat $SRC > "$TMP/compiler.yac"
cat >> "$TMP/compiler.yac" <<EOF
let src = read_file("$TMP/prog.yac");
let toks = tokenize(src);
let prog = parse_program(toks, 0, []);
let anf_item(item) = let r = anf_expr(item, 0) in [nth(r, 0), nth(r, 1)];
let elf = backend_compile(compile_top(map(anf_item, prog)));
let _ = write_file("$TMP/out.bin", elf) in
0
EOF
./yac "$TMP/compiler.yac" >/dev/null 2>&1 || {
  echo "FAIL: tokenize compile error"
  exit 1
}
chmod +x "$TMP/out.bin"
"$TMP/out.bin"
rc=$?
if [ "$rc" = "5" ]; then
  echo "PASS: L4 native tokenize(let x = 1)=5"
else
  echo "FAIL: tokenize got $rc want 5"
  fail=1
fi

if [ "$fail" = "0" ]; then
  exit 0
fi
exit 1

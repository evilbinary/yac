#!/usr/bin/env bash
# M4 L4 smoke: native ELF of is_kw (foldl + str == + capture) via yc pipeline.
set -u
cd "$(dirname "$0")/.."
TMP=build/l4_iskw_tmp
mkdir -p "$TMP"

# Same shape as e2e is_kw (known good), exit 1 on match.
printf '%s\n' \
  'let w = "let" in foldl(fun(a, k) -> if a then true else w == k, false, ["if", "let", "in"])' \
  > "$TMP/prog.yac"

SRC='src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac src-self/emit.yac src-self/backend.yac src-self/lower.yac'
cat $SRC > "$TMP/compiler.yac"
cat >> "$TMP/compiler.yac" <<EOF
let src = read_file("$TMP/prog.yac");
let toks = tokenize(src);
let prog = parse_program(toks, 0, []);
let anf_item(item) = let r = anf_expr(item, 0) in [nth(r, 0), nth(r, 1)];
let elf = backend_compile(compile_top(map(anf_item, prog)));
let _ = write_file("$TMP/out.bin", elf); 0
EOF

./yac "$TMP/compiler.yac" >/dev/null 2>&1 || {
  echo "FAIL: compile error"
  exit 1
}
chmod +x "$TMP/out.bin"
"$TMP/out.bin"
rc=$?
if [ "$rc" = "1" ]; then
  echo "PASS: L4 native is_kw=1"
  exit 0
fi
echo "FAIL: got $rc want 1"
exit 1

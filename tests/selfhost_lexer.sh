#!/usr/bin/env bash
# Self-hosted compiler M2 lexer test.
# Concatenates the yac lexer + driver, tokenizes test inputs, and checks
# the token stream. Run from the repo root: sh tests/selfhost_lexer.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
LEX=src-self/lexer.yac
DRV=src-self/driver_lex.yac
TMP=build/shlex_tmp
mkdir -p "$TMP"
cat "$LEX" "$DRV" > "$TMP/lexer_run.yac"

pass=0
fail=0

check() {
    desc="$1"; expected="$2"; actual="$3"
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1)); echo "PASS: $desc"
    else
        fail=$((fail + 1)); echo "FAIL: $desc"
        echo "--- expected ---"; printf '%s\n' "$expected"
        echo "--- actual ---"; printf '%s\n' "$actual"
    fi
}

# run_case name  --  reads build/shlex_tmp/lexin.yac (input) and
# build/shlex_tmp/lex_expected.txt (expected), compares.
run_case() {
    name="$1"
    expected=$(cat "$TMP/lex_expected.txt")
    actual=$($BIN "$TMP/lexer_run.yac" 2>/dev/null | grep -v '^0$' | tr -d '\r')
    check "$name" "$expected" "$actual"
}

# --- basic let ---
cat > "$TMP/lexin.yac" <<'IN'
let x = 5 in
x + 1
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
kw let
ident x
op =
num 5
kw in
ident x
op +
num 1
EXP
run_case "basic let"

# --- keyword/ident/fun ---
cat > "$TMP/lexin.yac" <<'IN'
let f(x) = if x == 0 then 0 else f(x - 1)
print "hi"
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
kw let
ident f
lparen (
ident x
rparen )
op =
kw if
ident x
op ==
num 0
kw then
num 0
kw else
ident f
lparen (
ident x
op -
num 1
rparen )
kw print
str hi
EXP
run_case "keyword/ident/fun"

# --- scientific notation ---
cat > "$TMP/lexin.yac" <<'IN'
2.5e-3
1e10
1E5
1e2-3
2e-0
1.5e+3
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
num 2.5e-3
num 1e10
num 1E5
num 1e2
op -
num 3
num 2e-0
num 1.5e+3
EXP
run_case "scientific notation"

# --- exponent-not-identifier boundary ---
cat > "$TMP/lexin.yac" <<'IN'
e
5e
1e+
1e
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
ident e
num 5
ident e
num 1
ident e
op +
num 1
ident e
EXP
run_case "exponent-not-identifier boundary"

# --- comments ---
cat > "$TMP/lexin.yac" <<'IN'
-- line comment
let a = 1 -- trailing
/* block
   comment */
let b = 2
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
kw let
ident a
op =
num 1
kw let
ident b
op =
num 2
EXP
run_case "comments and block comment"

# --- lambda arrow + comparison ---
cat > "$TMP/lexin.yac" <<'IN'
let g = fun (a, b) -> a >= 3.14 and not (a < b)
IN
cat > "$TMP/lex_expected.txt" <<'EXP'
kw let
ident g
op =
kw fun
lparen (
ident a
comma ,
ident b
rparen )
op ->
ident a
op >=
num 3.14
kw and
kw not
lparen (
ident a
op <
ident b
rparen )
EXP
run_case "lambda arrow and comparison"

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

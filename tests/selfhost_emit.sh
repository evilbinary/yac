#!/usr/bin/env bash
# Self-hosted compiler M3.2: LIR -> x86-64 ELF codegen test.
# Compiles LIR programs via backend.yac (emit.yac + elf.yac) and runs them,
# checking the exit code.
# Run from the repo root: sh tests/selfhost_emit.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
TMP=build/emit_tmp
mkdir -p "$TMP"

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

# compile a LIR program (embedded in the runner) and run it, echo exit code
run_compile() {
    name="$1"; lir="$2"; want="$3"
    cat > "$TMP/gen.yac" <<EOF
let prog = $lir;
let elf = backend_compile(prog);
let _ = write_file("$TMP/out.bin", elf);
0;
EOF
    cat src-self/encode_x64.yac src-self/elf.yac src-self/emit.yac \
        src-self/backend.yac "$TMP/gen.yac" > "$TMP/run.yac"
    if ! $BIN "$TMP/run.yac" >/dev/null 2>&1; then
        fail=$((fail + 1)); echo "FAIL: $name (compile error)"
        return
    fi
    chmod +x "$TMP/out.bin"
    "$TMP/out.bin"
    rc=$?
    check "$name" "$want" "$rc"
}

MUL5='["prog",[["fun","_start",0,[["prologue",2],["mov_imm",1,5],["mov_imm",2,6],["binop","*",1,1,2],["exit",1]]]],"_start"]'
run_compile "mul 5*6" "$MUL5" "30"

DIV='["prog",[["fun","_start",0,[["prologue",3],["mov_imm",1,17],["mov_imm",2,5],["binop","/",3,1,2],["exit",3]]]],"_start"]'
run_compile "div 17/5" "$DIV" "3"

MOD='["prog",[["fun","_start",0,[["prologue",3],["mov_imm",1,17],["mov_imm",2,5],["binop","%",3,1,2],["exit",3]]]],"_start"]'
run_compile "mod 17%5" "$MOD" "2"

EXPR='["prog",[["fun","_start",0,[["prologue",5],["mov_imm",1,100],["mov_imm",2,30],["binop","-",3,1,2],["mov_imm",4,2],["mov_imm",5,4],["binop","*",4,4,5],["binop","+",3,3,4],["exit",3]]]],"_start"]'
run_compile "expr (100-30)+2*4" "$EXPR" "78"

# if/br: 5 < 10 -> L1 (exit 1); false path -> L2 (exit 2)
IF_TRUE='["prog",[["fun","_start",0,[["prologue",4],["mov_imm",1,5],["mov_imm",2,10],["cmp","<",3,1,2],["br",3,"L1","L2"],["label","L1"],["mov_imm",4,1],["exit",4],["label","L2"],["mov_imm",4,2],["exit",4]]]],"_start"]'
run_compile "branch 5<10 (true)" "$IF_TRUE" "1"

IF_FALSE='["prog",[["fun","_start",0,[["prologue",4],["mov_imm",1,20],["mov_imm",2,10],["cmp","<",3,1,2],["br",3,"L1","L2"],["label","L1"],["mov_imm",4,1],["exit",4],["label","L2"],["mov_imm",4,2],["exit",4]]]],"_start"]'
run_compile "branch 20<10 (false)" "$IF_FALSE" "2"

IF_EQ='["prog",[["fun","_start",0,[["prologue",4],["mov_imm",1,7],["mov_imm",2,7],["cmp","==",3,1,2],["br",3,"L1","L2"],["label","L1"],["mov_imm",4,1],["exit",4],["label","L2"],["mov_imm",4,2],["exit",4]]]],"_start"]'
run_compile "branch 7==7 (true)" "$IF_EQ" "1"

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

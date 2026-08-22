#!/usr/bin/env bash
# Self-hosted compiler: source → ELF, check exit code / stdout.
#
#   sh tests/selfhost_e2e.sh              # C interpreter runs driver_lower.yac
#   sh tests/selfhost_e2e.sh /path/to/yc  # native yc (default output: in, no .bin)
#
# Run from the repo root.
set -u
cd "$(dirname "$0")/.."

BIN=./yac
YC="${1:-}"
TMP=build/lower_tmp
mkdir -p "$TMP"
E2E_TMP="$TMP"

if [ -z "$YC" ]; then
    SRC='src-self/log.yac src-self/lexer.yac src-self/parser.yac src-self/anf.yac src-self/encode_x64.yac src-self/elf.yac src-self/lir.yac src-self/emit.yac src-self/backend.yac src-self/lower.yac src-self/driver_lower.yac'
    cat $SRC > "$TMP/run.yac"
fi

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

compile_in() {
    src="$1"
    slug="$2"
    printf '%s\n' "$src" > "$TMP/in.yac"
    if [ -n "$YC" ]; then
        outpath="$TMP/prog_$slug"
        rm -f "$outpath" "$outpath.bin"
        if ! "$YC" "$TMP/in.yac" -o "$outpath" >/dev/null 2>&1; then
            return 1
        fi
        if [ -f "$outpath.bin" ]; then
            echo "FAIL: yc wrote ${outpath}.bin (want no .bin suffix)" >&2
            return 1
        fi
        if [ ! -f "$outpath" ]; then
            return 1
        fi
        chmod +x "$outpath"
        echo "$outpath"
        return 0
    fi
    if ! $BIN "$TMP/run.yac" >/dev/null 2>&1; then
        return 1
    fi
    chmod +x "$TMP/out.bin"
    echo "$TMP/out.bin"
    return 0
}

e2e_case() {
    kind="$1"; name="$2"; src="$3"; want="$4"
    tag="$name"
    if [ -n "$YC" ]; then tag="$name [$(basename "$YC")]"; fi
    slug=$(printf '%s' "$name" | tr ' /[]' '____')
    out=$(compile_in "$src" "$slug") || {
        fail=$((fail + 1)); echo "FAIL: $tag (compile error)"
        return
    }
    if [ "$kind" = "print" ]; then
        actual=$("$out")
        check "$tag" "$want" "$actual"
    else
        "$out"
        rc=$?
        check "$tag" "$want" "$rc"
    fi
}

python3 -c "open('$TMP/big70k.txt','wb').write(b'x'*70000)"
# shellcheck disable=SC1091
. tests/selfhost_e2e_cases.sh

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

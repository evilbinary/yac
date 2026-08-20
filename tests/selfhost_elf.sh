#!/usr/bin/env bash
# Self-hosted compiler M3: ELF packer test.
# The yac-written pack_elf must produce a runnable ELF that exits with 42.
# Run from the repo root: sh tests/selfhost_elf.sh
set -u
cd "$(dirname "$0")/.."

BIN=./yac
TMP=build/elf_tmp
mkdir -p "$TMP"
cat src-self/elf.yac src-self/driver_elf.yac > "$TMP/run.yac"

pass=0
fail=0

# run the yac driver, which writes build/elf_tmp/out.bin
$BIN "$TMP/run.yac" >/dev/null 2>&1

# 1. must be a valid ELF (magic + machine)
magic=$(xxd -p -l 4 "$TMP/out.bin" 2>/dev/null)
if [ "$magic" = "7f454c46" ]; then
    pass=$((pass + 1)); echo "PASS: ELF magic"
else
    fail=$((fail + 1)); echo "FAIL: ELF magic (got $magic)"
fi

# 2. must be executable and exit with 42
chmod +x "$TMP/out.bin"
"$TMP/out.bin"
rc=$?
if [ $rc -eq 42 ]; then
    pass=$((pass + 1)); echo "PASS: ELF runs and exits 42"
else
    fail=$((fail + 1)); echo "FAIL: ELF exit code (got $rc, want 42)"
fi

# 3. file size (120 header + 16 code = 136)
sz=$(stat -c%s "$TMP/out.bin" 2>/dev/null || wc -c < "$TMP/out.bin")
if [ "$sz" = "136" ]; then
    pass=$((pass + 1)); echo "PASS: ELF size 136"
else
    fail=$((fail + 1)); echo "FAIL: ELF size (got $sz, want 136)"
fi

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]

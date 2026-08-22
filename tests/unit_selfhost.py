#!/usr/bin/env python3
"""Unit tests for the src-self yac modules, driven by the C interpreter.

Each test concatenates the needed src-self files with a small yac driver that
calls the function under test and prints its result; we run it via ./yac and
assert on stdout.

Run:  python3 tests/unit_selfhost.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELF = os.path.join(ROOT, "src-self")
YAC = os.path.join(ROOT, "yac")

SRC = {
    "lexer": ["lexer.yac"],
    "parser": ["lexer.yac", "parser.yac"],
    "anf": ["lexer.yac", "parser.yac", "anf.yac"],
    "lower": ["lexer.yac", "parser.yac", "anf.yac", "lower.yac"],
    "lir": ["lir.yac"],
    "elf": ["elf.yac"],
    "encode": ["encode_x64.yac"],
}

passed = 0
failed = 0
failures = []


def load(name):
    with open(os.path.join(SELF, name), "r") as f:
        return f.read()


def run_yac(files, driver):
    src = "".join(load(f) for f in files) + "\n" + driver
    fd, path = tempfile.mkstemp(suffix=".yac")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(src)
        p = subprocess.run([YAC, path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           cwd=ROOT)
    finally:
        os.unlink(path)
    if p.returncode != 0:
        return ("ERROR", p.stderr.decode().strip())
    return ("OK", p.stdout.decode().strip())


def check(name, files, driver, expected):
    global passed, failed
    status, out = run_yac(files, driver)
    if status == "OK" and out == expected:
        passed += 1
    else:
        failed += 1
        failures.append((name, expected, out))
        print(f"FAIL {name}\n  expected: {expected!r}\n  actual:   {out!r}")


def multi(name, cases):
    for c in cases:
        check(f"{name}: {c[0]}", c[1], c[2], c[3])


# ---------------------------------------------------------------------------
# lir.yac — LIR constructors
# ---------------------------------------------------------------------------

def test_lir():
    multi("lir", [
        ("mov_imm", SRC["lir"], """
print l_mov_imm(3, 5);
0;
""", '[mov_imm, 3, 5]\n0'),
        ("binop", SRC["lir"], """
print l_binop("+", 1, 2, 3);
0;
""", '[binop, +, 1, 2, 3]\n0'),
        ("call", SRC["lir"], """
print l_call(4, "f", [1, 2]);
0;
""", '[call, 4, f, [1, 2]]\n0'),
    ])


# ---------------------------------------------------------------------------
# lexer.yac — tokenizer
# ---------------------------------------------------------------------------

def test_lexer():
    d1 = """
print tokenize("let x = 5 in x + 1");
print "";
0;
"""
    d2 = """
print tokenize("2.5e-3 + 1e10");
print "";
0;
"""
    multi("lexer", [
        ("tokenize", SRC["lexer"], d1,
         '[[kw, let, 1], [ident, x, 1], [op, =, 1], [num, 5, 1], '
         '[kw, in, 1], [ident, x, 1], [op, +, 1], [num, 1, 1], '
         '[eof, , 1]]\n\n0'),
        ("scientific", SRC["lexer"], d2,
         '[[num, 2.5e-3, 1], [op, +, 1], [num, 1e10, 1], [eof, , 1]]\n\n0'),
    ])


# ---------------------------------------------------------------------------
# elf.yac — ELF packing
# ---------------------------------------------------------------------------

def test_elf():
    d = """
let b = bytes_new();
let _ = bytes_append(b, 144);
let elf = pack_elf(b);
print bytes_len(elf);
print "";
/* magic */
print bytes_ref(elf, 0);
print "";
print bytes_ref(elf, 1);
print "";
print bytes_ref(elf, 4);
print "";
0;
"""
    multi("elf", [
        ("pack_elf size", SRC["elf"], d, '121\n\n127\n\n69\n\n2\n\n0'),
    ])


# ---------------------------------------------------------------------------



# ---------------------------------------------------------------------------
# parser.yac — AST parsing
# ---------------------------------------------------------------------------

def test_parser():
    # parse_program on tokenized source
    d = """
let src = "1 + 2 * 3";
let toks = tokenize(src);
let prog = parse_program(toks, 0, []);
print prog;
0;
"""
    check("parser: precedence", SRC["parser"], d,
          '[[binop, +, [int, 1], [binop, *, [int, 2], [int, 3]]]]\n0')
    d2 = """
let src = "let f(x) = if x == 0 then 0 else f(x - 1)";
let toks = tokenize(src);
let prog = parse_program(toks, 0, []);
print prog;
0;
"""
    check("parser: let fun", SRC["parser"], d2,
          '[[let, f, [fun, [x], [if, [binop, ==, [var, x], [int, 0]], [int, 0], [call, [var, f], [[binop, -, [var, x], [int, 1]]]]]], [unit]]]\n0')


# ---------------------------------------------------------------------------
# anf.yac — ANF normalization
# ---------------------------------------------------------------------------

def test_anf():
    d = """
let ast = ["binop", "+", ["int", "1"], ["binop", "*", ["int", "2"], ["int", "3"]]];
let r = anf_expr(ast, 0);
print [nth(r, 0), nth(r, 1)];
0;
"""
    check("anf: precedence", SRC["anf"], d,
          '[[[letbin, t0, *, [int, 2], [int, 3]], [letbin, t1, +, [int, 1], [var, t0]]], [var, t1]]\n0')


# ---------------------------------------------------------------------------



# ---------------------------------------------------------------------------
# lower.yac — free-variable analysis + codegen
# ---------------------------------------------------------------------------

def test_lower():
    # free_vars of fun(n)->n+x  (x free, n param)
    d = """
let body = [ [["letbin", "t0", "+", ["var","n"], ["var","x"]]], ["var","t0"] ];
print free_vars(body, ["n"], []);
0;
"""
    check("lower: free_vars", SRC["lower"], d, '[x]\n0')
    # nested: fun(n)->fun(m)->n+m  (nothing free; n param, m bound)
    d2 = """
let body = [ [["letfun","g",["m"], [ [["letbin","s0","+",["var","n"],["var","m"]]], ["var","s0"] ]]],
              ["var","g"] ];
print free_vars(body, ["n"], []);
0;
"""
    check("lower: free_vars nested", SRC["lower"], d2, '[]\n0')
    # str_to_int
    d3 = """
print str_to_int("42");
0;
"""
    check("lower: str_to_int", SRC["lower"], d3, '42\n0')


# ---------------------------------------------------------------------------



# ---------------------------------------------------------------------------
# encode_x64.yac — instruction encoders (assert byte sequences)
# ---------------------------------------------------------------------------

def test_encode():
    def enc(name, body, expected):
        d = "let b = bytes_new();\n" + body + "\nlet dump(n) = if n >= bytes_len(b) then 0 else\n  let _ = print bytes_ref(b, n) in\n  dump(n + 1);\nlet _ = dump(0);\n0;\n"
        check("encode: " + name, SRC["encode"], d, expected + "\n0")
    enc("mov_r64_imm rax", "let _ = mov_r64_imm(b, 0, 60);",
        "72\n199\n192\n60\n0\n0\n0")
    enc("push/pop rbp", "let _ = push_rbp(b); let _ = pop_rbp(b);",
        "85\n93")
    enc("ret", "let _ = ret(b);", "195")
    enc("syscall", "let _ = syscall(b);", "15\n5")
    enc("mov_rbp_rsp", "let _ = mov_rbp_rsp(b);", "72\n137\n229")
    enc("jmp_rel32", "let _ = jmp_rel32(b, 0);", "233\n0\n0\n0\n0")
    enc("mov_r64_r64", "let _ = mov_r64_r64(b, 0, 1);", "72\n137\n200")


def main():
    test_lir()
    test_lexer()
    test_elf()
    test_parser()
    test_anf()
    test_lower()
    test_encode()
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

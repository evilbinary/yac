#!/usr/bin/env python3
"""Fill Yac `import` / `export` and set one package name per file.

Walks .yac files under package roots. Import path is the file path relative
to a root, `/` → `.` (`src-self/back/pack/elf.yac` → `back.pack.elf`).
`package` is rewritten to that path (one file, one package). Identifiers
that are not local lets / prims / keywords are matched to another file's
`export` or top-level `let`, then `import a.b.c { names }` is inserted.

  python3 tools/fill_imports.py              # dry-run
  python3 tools/fill_imports.py --apply      # write files
  python3 tools/fill_imports.py --apply pkg/compiler.yac
  python3 tools/test_fill_imports.py         # unit tests
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

KEYWORDS = {
    "let", "in", "fun", "if", "then", "else", "not", "and", "or",
    "true", "false", "callcc", "throw", "package", "export", "import",
    "as", "print",
}

PRIMS = {
    "cons", "len", "nth", "tail", "append", "print", "map", "filter",
    "foldl", "foldr", "drop", "head", "str_len", "str_hash", "ident_hash",
    "str_cat", "str_ref", "str_slice", "str_chr", "int_to_str",
    "time_ms", "time_ns", "time_str", "argc", "argv",
    "bytes_new", "bytes_len", "bytes_append", "bytes_extend", "bytes_ref",
    "bytes_put", "mem_ref", "mem_put", "bytes_to_str",
    "list_new", "list_push", "list_set", "list_rev",
    "box_new", "box_get", "box_set", "yac_prof_cell",
    "read_file", "write_file", "system", "read_line", "jit_run", "popen",
    "exit", "band", "bor", "bxor", "bnot", "bshl", "bshr", "gc_collect",
    "is_int", "iadd", "isub", "imul", "idiv", "irem", "ccall",
}

# Kernel image; not a guest package. Main program keeps an anonymous package.
SKIP_PROVIDERS = {"rt/runtime.yac"}
SKIP_PROVIDER_PREFIX = ("drivers/",)
SKIP_PACKAGE = {"yc.yac", "rt/runtime.yac"}
SKIP_PACKAGE_PREFIX = ("drivers/",)

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
IMPORT_LINE = re.compile(
    r"^[ \t]*import[ \t]+([A-Za-z_][A-Za-z0-9_.]*)"
    r"(?:[ \t]+as[ \t]+([A-Za-z_][A-Za-z0-9_]*))?"
    r"(?:[ \t]*\{([^}]*)\})?"
    r"[ \t]*$",
    re.M,
)


def strip_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            # Same as lexer: /*. is a path glob, not a nested opener.
            # */. (as in rt/*.yac) is also a glob, not a closer.
            depth = 1
            j = i + 2
            while j + 1 < n and depth:
                if src[j] == "/" and src[j + 1] == "*":
                    if j + 2 < n and src[j + 2] == ".":
                        j += 1
                        continue
                    depth += 1
                    j += 2
                    continue
                if src[j] == "*" and src[j + 1] == "/":
                    if j + 2 < n and src[j + 2] == ".":
                        j += 1
                        continue
                    depth -= 1
                    j += 2
                    continue
                j += 1
            out.append(" " * (j - i))
            i = j
            continue
        if src[i] == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(" " * (j - i))
            i = j
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def idents_with_dots(src: str) -> List[Tuple[str, bool]]:
    """Return (ident, is_qvar_field) in order. Field after `.` is not a free name."""
    found = []
    prev_dot = False
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == ".":
            prev_dot = True
            i += 1
            continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            found.append((src[i:j], prev_dot))
            prev_dot = False
            i = j
            continue
        if not c.isspace():
            prev_dot = False
        i += 1
    return found


def rel_to_import_path(rel: str) -> str:
    if rel.endswith(".yac"):
        rel = rel[:-4]
    return rel.replace("\\", "/").replace("/", ".")


def skip_package_rel(rel: str) -> bool:
    rel = rel.replace("\\", "/")
    if rel in SKIP_PACKAGE:
        return True
    return any(rel.startswith(p) for p in SKIP_PACKAGE_PREFIX)


def wanted_package(rel: str, import_path: str) -> Optional[str]:
    if skip_package_rel(rel):
        return None
    return import_path


@dataclass
class Imp:
    path: str
    alias: Optional[str] = None
    names: Optional[Set[str]] = None  # None = all exports
    star: bool = False

    def render(self) -> str:
        if self.alias:
            return f"import {self.path} as {self.alias}"
        if self.star or self.names is None:
            return f"import {self.path}"
        xs = sorted(self.names)
        return f"import {self.path} {{{', '.join(xs)}}}"


@dataclass
class FileInfo:
    path: str
    rel: str
    import_path: str
    text: str
    package: str = ""
    exports: List[str] = field(default_factory=list)
    defs: Set[str] = field(default_factory=set)
    provide: Set[str] = field(default_factory=set)
    uses: Set[str] = field(default_factory=set)
    imps: List[Imp] = field(default_factory=list)
    imported_names: Set[str] = field(default_factory=set)


def parse_import_line(line: str) -> Optional[Imp]:
    m = IMPORT_LINE.match(line.strip())
    if not m:
        return None
    path, alias, brace = m.group(1), m.group(2), m.group(3)
    if alias:
        return Imp(path, alias=alias)
    if brace is None:
        return Imp(path, star=True, names=None)
    names = set()
    for part in brace.split(","):
        part = part.strip()
        if not part:
            continue
        bits = part.split()
        if len(bits) >= 3 and bits[1] == "as":
            names.add(bits[0])
        else:
            names.add(bits[0])
    return Imp(path, names=names)


def _params(blob: str) -> Set[str]:
    names = set()
    for p in blob.split(","):
        p = p.strip()
        if p and IDENT.fullmatch(p):
            names.add(p)
    return names


def defs_in(plain: str) -> Tuple[Set[str], Set[str]]:
    """local (any let/params) and provide (top-level let names only)."""
    local: Set[str] = set()
    provide: Set[str] = set()
    for m in re.finditer(
        r"(?m)^([ \t]*)let\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\(([^)]*)\))?",
        plain,
    ):
        indent, name, args = m.group(1), m.group(2), m.group(3)
        local.add(name)
        if len(indent) <= 1:
            provide.add(name)
        if args:
            local |= _params(args)
    for m in re.finditer(r"\bfun\s*\(([^)]*)\)", plain):
        local |= _params(m.group(1))
    local.discard("_")
    provide.discard("_")
    local.add("_")
    return local, provide


def parse_header_meta(plain: str) -> Tuple[str, List[str]]:
    pkg = ""
    exports: List[str] = []
    seen: Set[str] = set()
    m = re.search(r"(?m)^[ \t]*package[ \t]+([A-Za-z_][A-Za-z0-9_.]*)", plain)
    if m:
        pkg = m.group(1)
    for em in re.finditer(r"(?m)^[ \t]*export[ \t]+(.+?)$", plain):
        for x in em.group(1).split(","):
            x = x.strip()
            if x and x not in seen:
                seen.add(x)
                exports.append(x)
    return pkg, exports


def load_file(path: str, rel: str, import_path: str) -> FileInfo:
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    plain = strip_comments(text)
    info = FileInfo(path=path, rel=rel, import_path=import_path, text=text)
    info.package, info.exports = parse_header_meta(plain)
    local, provide = defs_in(plain)
    info.defs = local
    info.provide = provide
    for m in re.finditer(r"(?m)^[ \t]*import[ \t].+$", plain):
        imp = parse_import_line(m.group(0))
        if imp:
            info.imps.append(imp)
            if imp.names:
                info.imported_names |= imp.names
            elif imp.star:
                info.imported_names.add("*:" + imp.path)
            if imp.alias:
                info.imported_names.add(imp.alias)
    skip_kw = KEYWORDS | {"export", "package", "import"}
    header_names = set(info.exports)
    if info.package:
        header_names.update(info.package.split("."))
    header_names.update(import_path.split("."))
    for imp in info.imps:
        header_names.update(imp.path.split("."))
        if imp.alias:
            header_names.add(imp.alias)
        if imp.names:
            header_names |= imp.names
    plain_body = re.sub(
        r"(?m)^[ \t]*(package|export|import)\b.*$",
        "",
        plain,
    )
    for ident, is_field in idents_with_dots(plain_body):
        if is_field or ident in skip_kw or ident in PRIMS:
            continue
        if ident == "_" or ident in header_names:
            continue
        info.uses.add(ident)
    info.uses -= info.defs
    return info


def walk_roots(roots: List[str]) -> List[FileInfo]:
    files = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in {".git", "build"}]
            for fn in filenames:
                if not fn.endswith(".yac"):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                if rel in SKIP_PROVIDERS:
                    continue
                ip = rel_to_import_path(rel)
                files.append(load_file(path, rel, ip))
    return files


def is_provider(f: FileInfo) -> bool:
    if f.rel in SKIP_PROVIDERS:
        return False
    return not any(f.rel.startswith(p) for p in SKIP_PROVIDER_PREFIX)


def _in_pkg_root(f: FileInfo) -> bool:
    p = f.path.replace("\\", "/")
    return "/pkg/" in p or f.rel.startswith("pkg/")


def providers(files: List[FileInfo]) -> Dict[str, List[FileInfo]]:
    by: Dict[str, List[FileInfo]] = defaultdict(list)
    for f in files:
        if not is_provider(f):
            continue
        exported = set(f.exports) | f.provide
        for n in exported:
            by[n].append(f)
    return by


def star_covers(f: FileInfo, name: str, by_path: Dict[str, FileInfo]) -> bool:
    for imp in f.imps:
        if imp.alias:
            continue
        if imp.star:
            other = by_path.get(imp.path)
            if other and (name in other.exports or name in other.provide):
                return True
        elif imp.names and name in imp.names:
            return True
    return False


def pick_provider(
    name: str, consumer: FileInfo, cands: List[FileInfo]
) -> Optional[FileInfo]:
    cands = [c for c in cands if c.path != consumer.path]
    if not cands:
        return None
    exp = [c for c in cands if name in c.exports]
    pool = exp or cands
    have = {i.path for i in consumer.imps}
    for c in pool:
        if c.import_path in have:
            return c
    # Compiler sources must not pick pkg/ shims (profile vs back.profile).
    cons_pkg = _in_pkg_root(consumer)
    if not cons_pkg:
        src = [c for c in pool if "/src-self/" in c.path.replace("\\", "/")]
        if len(src) == 1:
            return src[0]
        if src:
            pool = src
    else:
        same_tree = [c for c in pool if _in_pkg_root(c)]
        if len(same_tree) == 1:
            return same_tree[0]
        if same_tree:
            pool = same_tree
    if len(pool) == 1:
        return pool[0]
    pool.sort(key=lambda c: (0 if not _in_pkg_root(c) else 1,
                             len(c.import_path), c.import_path))
    defs = [c for c in pool if name in c.provide]
    uniq = {c.import_path for c in (defs or pool)}
    if len(uniq) > 1 and not exp:
        return None
    return pool[0]


def needed_imports(
    files: List[FileInfo],
) -> Tuple[Dict[str, Dict[str, Set[str]]], Dict[str, Set[str]], List[str]]:
    """file.path -> import_path -> names; file.path -> extra exports; warnings."""
    by_path = {f.import_path: f for f in files}
    by_name = providers(files)
    add: Dict[str, Dict[str, Set[str]]] = defaultdict(lambda: defaultdict(set))
    extra_exp: Dict[str, Set[str]] = defaultdict(set)
    warns = []
    for f in files:
        missing = []
        for n in sorted(f.uses):
            if n in f.imported_names:
                continue
            if star_covers(f, n, by_path):
                continue
            cands = by_name.get(n, [])
            p = pick_provider(n, f, cands)
            if p is None:
                if n not in f.defs:
                    uniq = {c.import_path for c in cands if c.path != f.path}
                    if len(uniq) > 1:
                        warns.append(
                            f"{f.rel}: ambiguous {n} -> {sorted(uniq)}"
                        )
                    elif not uniq:
                        missing.append(n)
                continue
            add[f.path][p.import_path].add(n)
            if n not in p.exports and n in p.provide:
                extra_exp[p.path].add(n)
        if missing:
            sample = missing[:12]
            more = "" if len(missing) <= 12 else f" (+{len(missing) - 12})"
            warns.append(f"{f.rel}: unresolved {sample}{more}")
    return add, extra_exp, warns


def merge_imps(f: FileInfo, new: Dict[str, Set[str]]) -> List[Imp]:
    by: Dict[str, Imp] = {}
    order: List[str] = []
    for imp in f.imps:
        if imp.path not in by:
            order.append(imp.path)
            by[imp.path] = Imp(
                imp.path,
                alias=imp.alias,
                names=None if imp.star or imp.names is None else set(imp.names),
                star=imp.star,
            )
        else:
            cur = by[imp.path]
            if cur.alias or imp.alias:
                continue
            if cur.star or imp.star:
                cur.star = True
                cur.names = None
            elif cur.names is not None and imp.names:
                cur.names |= imp.names
    for path, names in sorted(new.items()):
        if path == f.import_path:
            continue
        if path not in by:
            order.append(path)
            by[path] = Imp(path, names=set(names))
        else:
            cur = by[path]
            if cur.alias:
                continue
            if cur.star:
                continue
            if cur.names is None:
                cur.names = set(names)
            else:
                cur.names |= names
    return [by[p] for p in order]


def strip_kind_lines(text: str, kind: str) -> str:
    lines = text.splitlines(keepends=True)
    out = []
    for ln in lines:
        if re.match(r"^[ \t]*" + kind + r"\b", ln):
            continue
        out.append(ln)
    return "".join(out)


def leading_code_index(text: str) -> int:
    m = re.match(r"(?s)^(?:[ \t]*\n|/\*.*?\*/[ \t]*\n?)*", text)
    return m.end() if m else 0


def set_package(text: str, pkg: str) -> str:
    lines = text.splitlines(keepends=True)
    for i, ln in enumerate(lines):
        if re.match(r"^[ \t]*package\b", ln):
            lead = re.match(r"^[ \t]*", ln).group(0)
            lines[i] = f"{lead}package {pkg}\n"
            return "".join(lines)
    at = leading_code_index(text)
    return text[:at] + f"package {pkg}\n\n" + text[at:]


def insert_block(text: str, imports: List[Imp], new_exports: Optional[List[str]]) -> str:
    lines = text.splitlines(keepends=True)
    pkg_i = exp_i = None
    for i, ln in enumerate(lines):
        if pkg_i is None and re.match(r"^[ \t]*package\b", ln):
            pkg_i = i
        if re.match(r"^[ \t]*export\b", ln):
            exp_i = i
    if new_exports is not None:
        line = "export " + ", ".join(new_exports) + "\n"
        if exp_i is not None:
            lead = re.match(r"^[ \t]*", lines[exp_i]).group(0)
            lines[exp_i] = lead + line
        elif pkg_i is not None:
            lines.insert(pkg_i + 1, line)
            exp_i = pkg_i + 1
        else:
            lines.insert(0, line)
            exp_i = 0
    at = exp_i if exp_i is not None else pkg_i
    if at is None:
        block = "".join(imp.render() + "\n" for imp in imports)
        return block + ("\n" if text and not text.startswith("\n") else "") + text
    block = [imp.render() + "\n" for imp in imports]
    j = at + 1
    while j < len(lines) and lines[j].strip() == "":
        j += 1
    new_lines = lines[: at + 1] + block
    if j < len(lines) and lines[j].strip() != "":
        new_lines.append("\n")
    new_lines.extend(lines[j:])
    return "".join(new_lines)


def apply_file(
    f: FileInfo,
    new: Dict[str, Set[str]],
    extra_exp: Set[str],
    *,
    fix_package: bool = True,
) -> Optional[str]:
    want = wanted_package(f.rel, f.import_path) if fix_package else None
    pkg_changed = want is not None and want != f.package
    imps = merge_imps(f, new)
    exports = list(f.exports)
    changed_exp = False
    for n in sorted(extra_exp):
        if n not in exports:
            exports.append(n)
            changed_exp = True
    old_render = [i.render() for i in f.imps]
    new_render = [i.render() for i in imps]
    if old_render == new_render and not changed_exp and not pkg_changed:
        return None
    text = f.text
    if pkg_changed:
        text = set_package(text, want)
    body = strip_kind_lines(text, "import")
    return insert_block(body, imps, exports if changed_exp else None)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--root",
        action="append",
        dest="roots",
        help="package root (repeatable). Default: src-self, pkg",
    )
    ap.add_argument("--apply", action="store_true", help="write files")
    ap.add_argument(
        "--no-export",
        action="store_true",
        help="do not add missing names to export lists",
    )
    ap.add_argument(
        "--no-package",
        action="store_true",
        help="do not rewrite package to the file path",
    )
    ap.add_argument("only", nargs="*", help="limit to these files (paths)")
    args = ap.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(repo)
    roots = args.roots or ["src-self", "pkg"]
    files = walk_roots(roots)
    add, extra_exp, warns = needed_imports(files)
    if args.no_export:
        extra_exp = {}
    only = None
    if args.only:
        only = {os.path.abspath(p) for p in args.only}

    nfile = 0
    for f in files:
        if only and os.path.abspath(f.path) not in only:
            continue
        new = add.get(f.path, {})
        ex = extra_exp.get(f.path, set())
        want = None if args.no_package else wanted_package(f.rel, f.import_path)
        pkg_changed = want is not None and want != f.package
        if not new and not ex and not pkg_changed:
            continue
        nfile += 1
        print(f"## {f.rel}")
        if pkg_changed:
            print(f"  package {f.package or '(none)'} -> {want}")
        for path, names in sorted(new.items()):
            print(f"  import {path} {{{', '.join(sorted(names))}}}")
        if ex:
            print(f"  export += {', '.join(sorted(ex))}")
        if args.apply:
            out = apply_file(
                f, new, ex, fix_package=not args.no_package
            )
            if out is not None and out != f.text:
                with open(f.path, "w", encoding="utf-8") as fh:
                    fh.write(out)

    if warns:
        print("\n# unresolved / ambiguous")
        for w in warns:
            print(" ", w)

    print(f"\n{nfile} file(s) with package/import/export updates"
          f"{' [written]' if args.apply else ' [dry-run]'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

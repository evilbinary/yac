#!/usr/bin/env python3
"""Unit tests for tools/fill_imports.py (no compiler rebuild)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fill_imports as fi  # noqa: E402


class PathTests(unittest.TestCase):
    def test_rel_to_import_path(self):
        self.assertEqual(
            fi.rel_to_import_path("back/emit/emit_x86_64.yac"),
            "back.emit.emit_x86_64",
        )

    def test_wanted_package(self):
        self.assertEqual(
            fi.wanted_package("back/emit/emit.yac", "back.emit.emit"),
            "back.emit.emit",
        )
        self.assertIsNone(fi.wanted_package("yc.yac", "yc"))
        self.assertIsNone(fi.wanted_package("rt/runtime.yac", "rt.runtime"))
        self.assertIsNone(fi.wanted_package("drivers/x.yac", "drivers.x"))

    def test_glob_in_block_comment(self):
        src = "/* Stdlib from rt/*.yac is kept. */\nlet later(x) = x\n"
        plain = fi.strip_comments(src)
        self.assertIn("later", plain)
        local, provide = fi.defs_in(plain)
        self.assertIn("later", provide)

    def test_union_exports(self):
        plain = "package a\nexport x, y\nexport z\n"
        pkg, xs = fi.parse_header_meta(plain)
        self.assertEqual(pkg, "a")
        self.assertEqual(xs, ["x", "y", "z"])


class ApplyTests(unittest.TestCase):
    def test_src_self_prefers_compiler_over_pkg(self):
        with tempfile.TemporaryDirectory() as td:
            ss = os.path.join(td, "src-self")
            pk = os.path.join(td, "pkg")
            os.makedirs(os.path.join(ss, "back"))
            os.makedirs(pk)
            a = os.path.join(ss, "back", "profile.yac")
            b = os.path.join(pk, "profile.yac")
            c = os.path.join(ss, "yc.yac")
            with open(a, "w") as fh:
                fh.write("package back.profile\nexport yac_prof_enable\nlet yac_prof_enable(x) = x\n")
            with open(b, "w") as fh:
                fh.write("package profile\nexport yac_prof_enable\nlet yac_prof_enable(x) = x\n")
            with open(c, "w") as fh:
                fh.write("let go(_) = yac_prof_enable(1)\n")
            files = [
                fi.load_file(a, "back/profile.yac", "back.profile"),
                fi.load_file(b, "profile.yac", "profile"),
                fi.load_file(c, "yc.yac", "yc"),
            ]
            add, extra, _ = fi.needed_imports(files)
            self.assertEqual(list(add[c].keys()), ["back.profile"])

    def test_two_files_import_and_rename(self):
        with tempfile.TemporaryDirectory() as td:
            a = os.path.join(td, "util.yac")
            b = os.path.join(td, "main.yac")
            with open(a, "w") as fh:
                fh.write("package util\nexport helper\nlet helper(x) = x\n")
            with open(b, "w") as fh:
                fh.write("package coarse\nlet go(x) = helper(x)\n")
            fa = fi.load_file(a, "util.yac", "util")
            fb = fi.load_file(b, "main.yac", "main")
            add, extra, warns = fi.needed_imports([fa, fb])
            self.assertEqual(add[b]["util"], {"helper"})
            self.assertEqual(extra, {})
            out = fi.apply_file(fb, add[b], set())
            self.assertIn("package main\n", out)
            self.assertIn("import util {helper}", out)

    def test_set_package_after_comment(self):
        src = "/* hi */\n\npackage emit\nlet f(x) = x\n"
        out = fi.set_package(src, "back.emit.win_x86_64")
        self.assertIn("package back.emit.win_x86_64\n", out)
        self.assertNotIn("package emit\n", out)


if __name__ == "__main__":
    unittest.main()

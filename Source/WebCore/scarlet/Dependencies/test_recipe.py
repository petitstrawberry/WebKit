#!/usr/bin/env python3
"""Archive parser tests; actual compilation is verified by the Nix build."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("scarlet_dependency_recipe", HERE / "build.py")
recipe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recipe)


def archive(payload):
    header = ("object.o/".ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6)
              + "644".ljust(8) + str(len(payload)).ljust(10) + "`\n").encode()
    return b"!<arch>\n" + header + payload + (b"\n" if len(payload) % 2 else b"")


def elf(machine=183):
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", identity, 1, machine, 1, 0, 0, 64, 0,
                         64, 0, 0, 64, 2, 1)
    null = bytes(64)
    strings = struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, 192, 1, 0, 0, 1, 0)
    return header + null + strings + b"\0"


class RecipeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="scarlet-dependency-recipe-")
        self.root = Path(self.temporary.name)
        self.lock = json.loads((HERE / "dependencies.lock.json").read_text())

    def tearDown(self):
        self.temporary.cleanup()

    def test_structural_native_elf_fixture(self):
        # This is a parser fixture, not evidence of real native compilation.
        path = self.root / "fixture.a"
        path.write_bytes(archive(elf()))
        self.assertEqual(recipe.native_members(path)["native_object_count"], 1)

    def test_wrong_architecture_archive_fails(self):
        path = self.root / "foreign.a"
        path.write_bytes(archive(elf(machine=62)))
        with self.assertRaisesRegex(recipe.RecipeError, "AArch64"):
            recipe.native_members(path)

    def test_truncated_elf_section_table_fails(self):
        path = self.root / "truncated.a"
        path.write_bytes(archive(elf()[:64]))
        with self.assertRaisesRegex(recipe.RecipeError, "section table"):
            recipe.native_members(path)

    def test_truncated_elf_section_payload_fails(self):
        path = self.root / "payload.a"
        path.write_bytes(archive(elf()[:-1]))
        with self.assertRaisesRegex(recipe.RecipeError, "section payload"):
            recipe.native_members(path)

    def test_empty_and_thin_archives_fail(self):
        path = self.root / "invalid.a"
        for data in (b"!<arch>\n", b"!<thin>\n"):
            path.write_bytes(data)
            with self.subTest(data=data), self.assertRaises(recipe.RecipeError):
                recipe.native_members(path)


if __name__ == "__main__":
    unittest.main()

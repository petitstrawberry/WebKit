#!/usr/bin/env python3
"""Source-lock/path/archive failure tests; no fetch, target build or guest run."""

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

    def args(self, **overrides):
        values = dict(build_dir=self.root / "build", sdk=self.root / "sdk", prefix=None)
        values.update(overrides)
        return argparse.Namespace(**values)

    def check_lock(self, lock):
        path = self.root / "lock.json"
        path.write_text(json.dumps(lock))
        return recipe.load_lock(path)

    def test_published_lock_is_complete(self):
        self.assertEqual(recipe.load_lock(HERE / "dependencies.lock.json")["target"], "aarch64-unknown-scarlet")

    def test_missing_sdk_source_pin_fails(self):
        self.lock["sdk"]["llvm_source"]["commit"] = None
        with self.assertRaisesRegex(recipe.RecipeError, "publish the source pin first"):
            self.check_lock(self.lock)

    def test_moving_git_branch_fails(self):
        self.lock["dependencies"][0]["source"]["commit"] = "scarlet"
        with self.assertRaisesRegex(recipe.RecipeError, "immutable"):
            self.check_lock(self.lock)

    def test_missing_release_hash_fails(self):
        self.lock["dependencies"][1]["source"].pop("sha256")
        with self.assertRaisesRegex(recipe.RecipeError, "archive SHA-256"):
            self.check_lock(self.lock)

    def test_patch_replay_fails(self):
        self.lock["source_policy"]["apply_local_patches"] = True
        with self.assertRaisesRegex(recipe.RecipeError, "patch or overlay"):
            self.check_lock(self.lock)

    def test_unpinned_submodule_fails(self):
        source = next(item["source"] for item in self.lock["dependencies"] if item["name"] == "libpsl")
        source["submodules"][0]["commit"] = "main"
        with self.assertRaisesRegex(recipe.RecipeError, "immutable"):
            self.check_lock(self.lock)

    def test_build_directory_cannot_overlap_sdk(self):
        for build, sdk in ((self.root / "sdk", self.root / "sdk"),
                           (self.root / "sdk/cache", self.root / "sdk"),
                           (self.root / "cache", self.root / "cache/sdk")):
            with self.subTest(build=build, sdk=sdk), self.assertRaisesRegex(recipe.RecipeError, "must not overlap"):
                recipe.Builder(self.args(build_dir=build, sdk=sdk, prefix=self.root / "prefix"), self.lock)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_source_checkout_cannot_be_output(self):
        with self.assertRaisesRegex(recipe.RecipeError, "outside the WebKit source"):
            recipe.external_path(HERE / "cache", "Build directory")

    def test_unsupported_paths_fail_before_writes(self):
        for name in ("native prefix", "日本語", "dollar$", 'quote"', "native:prefix"):
            with self.subTest(name=name), self.assertRaisesRegex(recipe.RecipeError, "Unsupported character"):
                recipe.external_path(self.root / name, "Dependency prefix")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_bootstrap_sdk_override_is_removed(self):
        with patch.dict(os.environ, SCARLET_BOOTSTRAP_CXX_HEADERS="/tmp/foreign-headers", SCARLET_PRINT_COMMAND="1"):
            builder = recipe.Builder(self.args(), self.lock)
        self.assertNotIn("SCARLET_BOOTSTRAP_CXX_HEADERS", builder.env)
        self.assertNotIn("SCARLET_PRINT_COMMAND", builder.env)

    def test_install_prefix_cannot_overlap_reserved_cache(self):
        for prefix in (self.root / "build/sources/icu", self.root / "build/build/icu", self.root / "build/control"):
            with self.subTest(prefix=prefix), self.assertRaisesRegex(recipe.RecipeError, "reserved"):
                recipe.Builder(self.args(prefix=prefix), self.lock)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_sdk_wrong_scarlet_revision_fails_before_compiler(self):
        builder = recipe.Builder(self.args(), self.lock)
        manifest = {"schema_version": 1, "target": self.lock["target"], "state": "built-and-native-link-audited",
                    "sources": {"llvm": {"git": self.lock["sdk"]["llvm_source"]["git_url"],
                                         "rev": self.lock["sdk"]["llvm_source"]["commit"]},
                                "scarlet": {"rev": "0" * 40}}}
        path = builder.sdk / "share/scarlet/native-sdk.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(recipe.RecipeError, "Scarlet native source differs"):
            builder.check_sdk()
        self.assertFalse(builder.root.exists())

    def test_build_source_changes_do_not_change_fetch_cache(self):
        source = self.root / "source"
        source.mkdir()
        (source / "zconf.h").write_text("exact upstream bytes\n")
        builder = recipe.Builder(self.args(), self.lock)
        work, _, _ = builder.work_source("zlib", source)
        (work / "zconf.h").rename(work / "zconf.h.included")
        self.assertEqual((source / "zconf.h").read_text(), "exact upstream bytes\n")
        with self.assertRaisesRegex(recipe.RecipeError, "modified outside"):
            builder.work_source("zlib", source)

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

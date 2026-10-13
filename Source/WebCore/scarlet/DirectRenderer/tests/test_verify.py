"""Synthetic host protocol tests. Nothing here runs WebCore or proves rendering."""

from copy import deepcopy
from dataclasses import FrozenInstanceError, replace
import contextlib
import io
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify as verifier


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_bytes(width, height, rgba, channels=3, filters=None):
    """Encode test-only pixels without using any renderer or imaging library."""
    rows, previous = [], bytes(width * channels)
    for y in range(height):
        raw = rgba[y * width * 4:(y + 1) * width * 4]
        if channels == 3:
            raw = b"".join(raw[x:x + 3] for x in range(0, len(raw), 4))
        filtering = filters[y % len(filters)] if filters else 0
        encoded = bytearray(raw)
        for x in range(len(raw)):
            left = raw[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            predictors = (0, left, up, (left + up) // 2)
            if filtering == 4:
                prediction = left + up - upper_left
                distances = (abs(prediction - left), abs(prediction - up), abs(prediction - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
            else:
                predictor = predictors[filtering]
            encoded[x] = (raw[x] - predictor) & 255
        rows.append(bytes([filtering]) + encoded)
        previous = raw
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2 if channels == 3 else 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")


class SyntheticRun:
    """Protocol fixture; deliberately synthetic and never native-run evidence."""
    def __init__(self, root):
        self.request = verifier.Request(root / "frame", root / "input.html", "test-only-001", root / "native.log",
                                        "/tmp/test-only/frame", "/share/test-only/input.html")
        self.request.input.write_bytes(b"<!doctype html><body><div id='probe'>synthetic protocol text</div></body>")
        self.width, self.height = 8, 5
        self.rgba = bytes((20, 40, 60, 255)) * self.width * self.height
        self.bgra = bytes((60, 40, 20, 255)) * self.width * self.height
        self.png = png_bytes(self.width, self.height, self.rgba)
        self.tree = b"TEST ONLY: not rendering evidence\nRenderView at (0,0) size 8x5\n  RenderBlock {BODY}\n    RenderText {#text} synthetic protocol text\n"
        self.metadata = {
            "schema": 2, "engine": "WebCore", "port": "Scarlet", "mode": "offline-direct-cpu",
            "revision": verifier.REVISION, "run_id": self.request.run_id, "input": self.request.native_input,
            "output_prefix": self.request.native_prefix, "completion": "native-render-success",
            "input_bytes": len(self.request.input.read_bytes()), "input_fnv1a64": verifier.fnv1a64(self.request.input.read_bytes()),
            "viewport": {"width": self.width, "height": self.height}, "stride": self.width * 4,
            "pixel_bytes": len(self.bgra), "pixel_fnv1a64": verifier.fnv1a64(self.bgra),
            "pixel_format": "BGRA8-premultiplied-little-endian",
            "png_bytes": len(self.png), "png_fnv1a64": verifier.fnv1a64(self.png),
            "render_tree_bytes": len(self.tree), "render_tree_fnv1a64": verifier.fnv1a64(self.tree),
            "render_tree_present": True, "layout_complete": True, "scripts_executed": 0,
            "script_execution_entries": 0, "script_permission_attempts": 0, "script_enabled": False,
            "subresources_enabled": False, "virtual_display_equals_viewport": True,
            "font_matches": [{"requested": family, "family": "Test Only Font", "file": "/share/test-only/font.ttf"}
                             for family in ("serif", "sans-serif", "monospace")],
            "body_text": "synthetic protocol text",
            "elements": [{"id": "probe", "found": True, "bounds": {"x": 0, "y": 0, "width": 8, "height": 5},
                          "text": "synthetic protocol text"}],
            "files": {"bgra": self.request.native_prefix + ".bgra", "png": self.request.native_prefix + ".png",
                      "render_tree": self.request.native_prefix + ".render-tree.txt"},
            "test_only": "synthetic; not native rendering evidence",
        }
        self.publish()

    def artifact_path(self, suffix):
        return Path(str(self.request.prefix) + suffix)

    def log_bytes(self, metadata_bytes):
        m = self.metadata
        return (f"NATIVE_WEBCORE_CASE_BEGIN run_id={m['run_id']} case=protocol_test\n"
                f"SCARLET_WEBCORE_BEGIN revision={m['revision']} run_id={m['run_id']} input_bytes={m['input_bytes']} "
                f"viewport={self.width}x{self.height} input_fnv1a64={m['input_fnv1a64']} output={m['output_prefix']}\n"
                "SCARLET_FONT family=serif matched=Test Only Font file=/share/test-only/font.ttf\n"
                f"SCARLET_SCRIPT_GUARD run_id={m['run_id']} execution_entries={m['script_execution_entries']} "
                f"permission_attempts={m['script_permission_attempts']} enabled={str(m['script_enabled']).lower()}\n"
                f"SCARLET_WEBCORE_PASS run_id={m['run_id']} viewport={self.width}x{self.height} stride={m['stride']} "
                f"bytes={m['pixel_bytes']} pixel_fnv1a64={m['pixel_fnv1a64']} scripts=0 output={m['output_prefix']} "
                f"metadata_fnv1a64={verifier.fnv1a64(metadata_bytes)}\n"
                f"NATIVE_WEBCORE_CASE_END run_id={m['run_id']} case=protocol_test success=true code=0\n").encode()

    def publish(self):
        metadata_bytes = (json.dumps(self.metadata, ensure_ascii=False, allow_nan=True, indent=2) + "\n").encode()
        for suffix, data in ((".bgra", self.bgra), (".png", self.png), (".render-tree.txt", self.tree), (".json", metadata_bytes)):
            self.artifact_path(suffix).write_bytes(data)
        self.request.log.write_bytes(self.log_bytes(metadata_bytes))

    def replace_artifact(self, kind, data):
        suffix, length_key, hash_key = {
            "bgra": (".bgra", "pixel_bytes", "pixel_fnv1a64"),
            "png": (".png", "png_bytes", "png_fnv1a64"),
            "tree": (".render-tree.txt", "render_tree_bytes", "render_tree_fnv1a64"),
        }[kind]
        setattr(self, kind, data)
        self.metadata[length_key] = len(data)
        self.metadata[hash_key] = verifier.fnv1a64(data)
        self.publish()


class VerifyProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="scarlet-verifier-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.run = SyntheticRun(Path(self.temporary.name))

    def rejected(self, message=None, **kwargs):
        with self.assertRaises(verifier.VerificationError) as raised:
            verifier.verify(self.run.request, **kwargs)
        if message:
            self.assertIn(message, str(raised.exception))

    def test_known_fnv_vectors(self):
        self.assertEqual(verifier.fnv1a64(b""), "cbf29ce484222325")
        self.assertEqual(verifier.fnv1a64(b"hello"), "a430d84680aabd0b")

    def test_valid_synthetic_protocol_and_cli(self):
        artifact = verifier.verify(self.run.request, ["probe"], {"probe": "synthetic protocol text"}, (8, 5))
        self.assertEqual(artifact.rgba, self.run.rgba)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            result = verifier.main(["verify", "--prefix", str(self.run.request.prefix), "--input", str(self.run.request.input),
                                    "--run-id", self.run.request.run_id, "--log", str(self.run.request.log),
                                    "--native-prefix", self.run.request.native_prefix, "--native-input", self.run.request.native_input,
                                    "--expect-element", "probe", "--expect-text", "probe=synthetic protocol text", "--size", "8x5"])
        self.assertEqual(result, 0)
        self.assertIn("SCARLET_ARTIFACT_VERIFIED", stdout.getvalue())

    def test_cli_rejects_and_returns_failure(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = verifier.main(["verify", "--prefix", str(self.run.request.prefix), "--input", str(self.run.request.input),
                                    "--run-id", "new-run", "--log", str(self.run.request.log)])
        self.assertEqual(result, 1)
        self.assertIn("SCARLET_ARTIFACT_REJECTED", stderr.getvalue())

    def test_input_bytes_changed(self):
        self.run.request.input.write_bytes(self.run.request.input.read_bytes().replace(b"synthetic", b"different"))
        self.rejected("input_fnv1a64")

    def test_run_id_is_external_expectation(self):
        self.run.request.run_id = "new-run"
        self.rejected("run_id")

    def test_input_and_output_paths_are_external_expectations(self):
        for key in ("input", "output_prefix"):
            with self.subTest(key=key):
                saved = self.run.metadata[key]
                self.run.metadata[key] = "/tmp/stale"
                self.run.publish()
                self.rejected(key)
                self.run.metadata[key] = saved

    def test_missing_commit_rejects_even_if_staged_metadata_exists(self):
        self.run.artifact_path(".json").rename(self.run.artifact_path(".test-only-001.json.tmp"))
        self.rejected("metadata: cannot read")

    def test_nul_host_path_fails_closed(self):
        self.run.request.input = Path("bad\x00path")
        self.rejected("input: cannot read")

    def test_metadata_exact_bytes_must_match_success_log(self):
        with self.run.artifact_path(".json").open("ab") as stream:
            stream.write(b" \n")
        self.rejected("metadata_fnv1a64")

    def test_old_log_rejects_updated_metadata(self):
        old_log = self.run.request.log.read_bytes()
        self.run.metadata["elements"][0]["bounds"]["x"] = 1
        self.run.publish()
        self.run.request.log.write_bytes(old_log)
        self.rejected("metadata_fnv1a64")

    def test_all_artifact_corruption_and_missing_files_reject(self):
        for suffix, hash_key in ((".bgra", "pixel_fnv1a64"), (".png", "png_fnv1a64"), (".render-tree.txt", "render_tree_fnv1a64")):
            with self.subTest(suffix=suffix):
                self.run.publish()
                path = self.run.artifact_path(suffix)
                data = bytearray(path.read_bytes())
                data[-1] ^= 1
                path.write_bytes(data)
                self.rejected(hash_key)
                path.unlink()
                self.rejected("cannot read")

    def test_json_duplicate_keys_and_nonfinite_numbers_reject(self):
        original = self.run.artifact_path(".json").read_bytes()
        self.run.artifact_path(".json").write_bytes(original.replace(b'"schema": 2', b'"schema": 2, "schema": 2'))
        self.rejected("duplicate JSON key")
        self.run.metadata["elements"][0]["bounds"]["x"] = float("nan")
        self.run.publish()
        self.rejected("non-finite JSON number")

    def test_json_invalid_unicode_and_extreme_numbers_fail_closed(self):
        self.run.metadata["elements"][0]["id"] = "\ud800"
        metadata = json.dumps(self.run.metadata, ensure_ascii=True).encode()
        self.run.artifact_path(".json").write_bytes(metadata)
        self.rejected("invalid Unicode")
        self.run.metadata["elements"][0]["id"] = "probe"
        self.run.metadata["elements"][0]["bounds"]["x"] = 10 ** 400
        self.run.publish()
        self.rejected("invalid x")
        self.run.metadata["elements"][0]["bounds"]["x"] = 0
        self.run.publish()
        metadata = self.run.artifact_path(".json").read_bytes().replace(b'"x": 0', b'"x": 1e999')
        self.run.artifact_path(".json").write_bytes(metadata)
        self.rejected("non-finite JSON number")

    def test_excessively_nested_json_rejects_without_python_traceback(self):
        self.run.artifact_path(".json").write_bytes(b'{"nested":' + b'[' * 2000 + b'0' + b']' * 2000 + b'}')
        # Shared unittest discovery may change Python's recursion limit. Either
        # parser rejection or contract rejection is valid; a traceback is not.
        self.rejected()

    def test_booleans_do_not_pass_as_integers(self):
        for key in ("schema", "input_bytes", "scripts_executed", "script_execution_entries", "script_permission_attempts", "pixel_bytes", "stride"):
            with self.subTest(key=key):
                saved = self.run.metadata[key]
                self.run.metadata[key] = False if saved == 0 else True
                self.run.publish()
                self.rejected()
                self.run.metadata[key] = saved

    def test_invalid_profile_flags_reject(self):
        for key, wrong in (("completion", "partial"), ("mode", "host-browser"), ("scripts_executed", 1),
                           ("script_execution_entries", 1), ("script_enabled", True),
                           ("subresources_enabled", True), ("layout_complete", False), ("render_tree_present", False)):
            with self.subTest(key=key):
                saved = self.run.metadata[key]
                self.run.metadata[key] = wrong
                self.run.publish()
                self.rejected(key)
                self.run.metadata[key] = saved

    def test_byte_count_stride_and_files_reject(self):
        for change in (lambda m: m.update(pixel_bytes=m["pixel_bytes"] - 1),
                       lambda m: m.update(stride=33), lambda m: m["files"].update(png="/tmp/other.png"),
                       lambda m: m.update(png_bytes=m["png_bytes"] + 1),
                       lambda m: m.update(render_tree_bytes=m["render_tree_bytes"] + 1)):
            with self.subTest(change=change):
                original = deepcopy(self.run.metadata)
                change(self.run.metadata)
                self.run.publish()
                self.rejected()
                self.run.metadata = original

    def test_foreign_or_duplicate_native_records_and_error_reject(self):
        original = self.run.request.log.read_text()
        begin = original.splitlines()[1]
        success = original.splitlines()[-2]
        for log in (original.replace("run_id=test-only-001", "run_id=other-run"),
                    original + success + "\n", begin + "\n" + original,
                    success + "\n" + begin + "\n", original + "SCARLET_WEBCORE_ERROR write\n",
                    original.replace(" scripts=0", " scripts=0 scripts=0"),
                    original.replace(" input_bytes=", " missing_input_bytes="),
                    original.replace("metadata_fnv1a64=", "missing_metadata_fnv1a64="),
                    original.replace(" success=true code=0", " success=false code=1"),
                    original.replace(" success=true code=0", " success=true code=7"),
                    original.replace("NATIVE_WEBCORE_CASE_END", "MISSING_NATIVE_WEBCORE_CASE_END")):
            with self.subTest(log=log):
                self.run.request.log.write_text(log)
                self.rejected("log")

    def test_log_order_uses_actual_anchored_records_not_noise(self):
        lines = self.run.request.log.read_text().splitlines()
        markers = [line for line in lines if line.startswith(("NATIVE_WEBCORE_CASE_", "SCARLET_WEBCORE_", "SCARLET_SCRIPT_GUARD"))]
        noise = "noise NATIVE_WEBCORE_CASE_BEGIN SCARLET_WEBCORE_BEGIN SCARLET_SCRIPT_GUARD SCARLET_WEBCORE_PASS NATIVE_WEBCORE_CASE_END\n"
        self.run.request.log.write_text(noise + "\n".join(reversed(markers)) + "\n")
        self.rejected("out of order")

    def test_outer_case_is_an_external_expectation_and_markers_must_match(self):
        self.run.request.case = "other_case"
        self.rejected("case/run ID mismatch")
        self.run.request.case = "protocol_test"
        log = self.run.request.log.read_text()
        self.run.request.log.write_text(log.replace("case=protocol_test success=", "case=other_case success="))
        self.rejected("case/run ID mismatch")

    def test_script_guard_new_fields_are_required(self):
        for key in ("script_execution_entries", "script_permission_attempts", "script_enabled"):
            with self.subTest(key=key):
                saved = self.run.metadata.pop(key)
                metadata = json.dumps(self.run.metadata).encode()
                self.run.artifact_path(".json").write_bytes(metadata)
                self.rejected(key)
                self.run.metadata[key] = saved

    def test_script_permission_attempt_counter_strict_types_and_range(self):
        for attempts in (-1, True, 0.0, "0", None, 2 ** 64):
            with self.subTest(attempts=attempts):
                self.run.metadata["script_permission_attempts"] = attempts
                self.run.publish()
                self.rejected("script_permission_attempts")

    def test_actual_script_entry_and_enabled_flag_cannot_hide_behind_legacy_zero(self):
        for key, value in (("script_execution_entries", 1), ("script_execution_entries", False),
                           ("script_execution_entries", 0.0), ("script_execution_entries", "0"),
                           ("script_enabled", True), ("script_enabled", 0), ("script_enabled", "false")):
            with self.subTest(key=key, value=value):
                saved = self.run.metadata[key]
                self.run.metadata[key] = value
                self.run.publish()
                self.rejected(key)
                self.run.metadata[key] = saved

    def test_offline_attempts_require_positive_permission_attempts_but_zero_entries(self):
        self.run.request.case = "offline_attempts"
        self.run.metadata["script_permission_attempts"] = 0
        self.run.publish()
        self.rejected("did not exercise a permission inquiry")
        for attempts in (1, 2):
            with self.subTest(attempts=attempts):
                self.run.metadata["script_permission_attempts"] = attempts
                self.run.publish()
                log = self.run.request.log.read_text().replace("case=protocol_test", "case=offline_attempts")
                self.run.request.log.write_text(log)
                artifact = verifier.verify(self.run.request)
                self.assertEqual(artifact.metadata["script_execution_entries"], 0)
                self.assertEqual(artifact.metadata["script_permission_attempts"], attempts)

    def test_normal_fixtures_must_not_exercise_script_permission_attempts(self):
        self.run.request.case = "baseline"
        self.run.metadata["script_permission_attempts"] = 1
        self.run.publish()
        self.rejected("unexpected permission inquiry in baseline")

    def test_arbitrary_case_allows_nonnegative_permission_attempts_when_disabled(self):
        self.run.metadata["script_permission_attempts"] = 2
        self.run.publish()
        verifier.verify(self.run.request)

    def test_script_guard_log_missing_duplicate_and_mismatch_reject(self):
        original = self.run.request.log.read_text()
        guard = next(line for line in original.splitlines() if line.startswith("SCARLET_SCRIPT_GUARD "))
        alternatives = (
            original.replace(guard + "\n", ""),
            original + guard + "\n",
            original.replace(guard, guard.replace("run_id=test-only-001", "run_id=other-run")),
            original.replace(guard, guard.replace("execution_entries=0", "execution_entries=1")),
            original.replace(guard, guard.replace("permission_attempts=0", "permission_attempts=1")),
            original.replace(guard, guard.replace("enabled=false", "enabled=true")),
            guard + "\n" + original.replace(guard + "\n", ""),
            original.replace(guard + "\n", "") + guard + "\n",
        )
        for log in alternatives:
            with self.subTest(log=log):
                self.run.request.log.write_text(log)
                self.rejected("native log")

    def test_png_dimension_crc_truncation_and_trailing_bytes_reject(self):
        original = self.run.png
        corrupted_crc = bytearray(original)
        corrupted_crc[29] ^= 1
        alternatives = (png_bytes(4, 10, self.run.rgba), bytes(corrupted_crc), original[:-2], original + b"old-run")
        for data in alternatives:
            with self.subTest(data=data[:32]):
                self.run.replace_artifact("png", data)
                self.rejected("PNG")

    def test_same_dimensions_different_png_cannot_pass_using_new_hashes(self):
        self.run.replace_artifact("png", png_bytes(8, 5, bytes((90, 40, 60, 255)) * 40))
        self.rejected("PNG/BGRA: color differs")

    def test_png_decompression_limit(self):
        ihdr = struct.pack(">IIBBBBB", 8, 5, 8, 2, 0, 0, 0)
        image = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\x00" * 100000)) + chunk(b"IEND", b"")
        self.run.replace_artifact("png", image)
        self.rejected("decompressed byte count")

    def test_all_png_filters_and_rgba_are_decoded(self):
        rgba = bytes(channel for x in range(40) for channel in (x * 7 % 256, x * 13 % 256, x * 19 % 256, 255))
        for channels in (3, 4):
            with self.subTest(channels=channels):
                image = png_bytes(8, 5, rgba, channels, [0, 1, 2, 3, 4])
                self.assertEqual(verifier.decode_png(image, 8, 5), rgba)

    def test_premultiplied_alpha_comparison(self):
        self.run.rgba = bytes((100, 60, 20, 128)) * 40
        self.run.bgra = bytes((10, 30, 50, 128)) * 40
        self.run.metadata["pixel_fnv1a64"] = verifier.fnv1a64(self.run.bgra)
        self.run.replace_artifact("png", png_bytes(8, 5, self.run.rgba, 4))
        verifier.verify(self.run.request)

    def test_render_tree_placeholder_utf8_and_structure_reject(self):
        for tree in (b"rendered successfully", b"RenderView only", b"RenderView\nRenderBlock\xff", b"RenderView\x00\nRenderBlock"):
            with self.subTest(tree=tree):
                self.run.replace_artifact("tree", tree)
                self.rejected("render tree")

    def test_element_duplicate_missing_or_invalid_metrics_reject(self):
        original = deepcopy(self.run.metadata)
        changes = [lambda m: m["elements"].append(deepcopy(m["elements"][0])),
                   lambda m: m["elements"][0].update(found=1),
                   lambda m: m["elements"][0]["bounds"].update(width=-1),
                   lambda m: m["elements"][0]["bounds"].update(x=True),
                   lambda m: m["elements"][0].update(text="old DOM text"),
                   lambda m: m.update(elements=[{"id": "probe", "found": False}])]
        for change in changes:
            with self.subTest(change=change):
                self.run.metadata = deepcopy(original)
                change(self.run.metadata)
                self.run.publish()
                self.rejected("element", expected_elements=["probe"])

    def test_caller_text_and_viewport_expectations(self):
        self.rejected("text differs", expected_text={"probe": "new DOM text"})
        self.rejected("expected element missing", expected_elements=["missing"])
        self.rejected("viewport", expected_size=(800, 600))

    def test_font_missing_and_duplicate_family_reject(self):
        self.run.metadata["font_matches"][0]["file"] = "host-relative.ttf"
        self.run.publish()
        self.rejected("font_matches")
        self.run.metadata["font_matches"][0]["file"] = "/share/test-only/font.ttf"
        self.run.metadata["font_matches"][1]["requested"] = "serif"
        self.run.publish()
        self.rejected("font_matches")


def synthetic_suite():
    """Synthetic causal measurements, solely for exercising comparison failures."""
    cases = {}
    for index, (name, filename) in enumerate(verifier.FIXTURE_CASES.items()):
        input_bytes = (verifier.FIXTURE_DIRECTORY / filename).read_bytes()
        parser = verifier.FixtureParser(input_bytes)
        width, height = (400 if name == "viewport_narrow" else 640), 480
        probe_width = 480 if name == "css_width" else 320
        probe_height = 70 if name == "css_width" else 94
        label_height = 33 if name == "font_size" else 27
        flow_height = 200 if name == "viewport_narrow" else (120 if name == "font_size" else 100)
        bounds = {
            "probe": {"x": 24, "y": 100, "width": probe_width, "height": probe_height},
            "flow": {"x": 24, "y": 214, "width": width - 48, "height": flow_height},
            "label": {"x": 24, "y": 60, "width": 440 if name == "text_change" else 280, "height": label_height},
            "tile-a": {"x": 24, "y": 330, "width": (width - 60) / 2, "height": 51},
            "script-marker": {"x": 24, "y": 397, "width": width - 48, "height": 19.5},
        }
        elements = {element_id: {"id": element_id, "found": True, "bounds": bounds[element_id],
                                 "text": "".join(parser.texts[element_id])} for element_id in verifier.REQUIRED_ELEMENTS}
        color = (242, 170, 96, 255) if name == "css_color" else (128, 201, 232, 255)
        rgba = bytearray(b"\xff" * width * height * 4)
        for y in range(100, 100 + probe_height):
            start = (y * width + 24) * 4
            rgba[start:start + probe_width * 4] = bytes(color) * probe_width
        if name in ("text_change", "font_size"):
            rgba[:4] = bytes((index, 0, 0, 255))
        bgra = b"".join(bytes((rgba[i + 2], rgba[i + 1], rgba[i], rgba[i + 3])) for i in range(0, len(rgba), 4))
        metadata = {"viewport": {"width": width, "height": height}, "stride": width * 4,
                    "scripts_executed": 0, "script_execution_entries": 0,
                    "script_permission_attempts": 1 if name == "offline_attempts" else 0, "script_enabled": False,
                    "body_text": "".join(parser.body), "test_only": "synthetic causal fixture; not rendering evidence"}
        request = verifier.Request(Path("/test-only") / name, Path(filename), "test-only-" + name, Path("unused.log"))
        tree = f"TEST ONLY\nRenderView\nRenderBlock\nRenderText script-disabled {name}"
        cases[name] = verifier.VerifiedArtifact(request, metadata, bgra, bytes(rgba), tree, input_bytes, elements)
    return cases


class CausalComparisonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = synthetic_suite()

    def test_synthetic_comparison_contract_accepts_expected_changes(self):
        verifier.compare_suite(self.cases)

    def test_unchanged_baseline_cannot_be_used_for_every_case(self):
        cases = deepcopy(self.cases)
        cases["css_width"].elements = deepcopy(cases["baseline"].elements)
        with self.assertRaisesRegex(verifier.VerificationError, "probe width"):
            verifier.compare_suite(cases)

    def test_color_change_requires_actual_expected_pixel(self):
        cases = deepcopy(self.cases)
        cases["css_color"].bgra = cases["baseline"].bgra
        with self.assertRaisesRegex(verifier.VerificationError, "pixels did not change"):
            verifier.compare_suite(cases)

    def test_color_change_cannot_alter_unrelated_pixels(self):
        cases = deepcopy(self.cases)
        pixels = bytearray(cases["css_color"].bgra)
        pixels[:4] = b"\x00\x00\x00\xff"
        cases["css_color"].bgra = bytes(pixels)
        with self.assertRaisesRegex(verifier.VerificationError, "unrelated pixel"):
            verifier.compare_suite(cases)

    def test_viewport_change_requires_real_reflow_metrics(self):
        cases = deepcopy(self.cases)
        cases["viewport_narrow"].elements["flow"]["bounds"]["height"] = cases["baseline"].elements["flow"]["bounds"]["height"]
        with self.assertRaisesRegex(verifier.VerificationError, "greater height"):
            verifier.compare_suite(cases)

    def test_font_change_requires_measured_line_height(self):
        cases = deepcopy(self.cases)
        cases["font_size"].elements["label"]["bounds"]["height"] = 27
        with self.assertRaisesRegex(verifier.VerificationError, "label line height"):
            verifier.compare_suite(cases)

    def test_causal_changes_require_actual_decoded_pixels_not_only_raw_bytes(self):
        cases = deepcopy(self.cases)
        cases["font_size"].rgba = cases["baseline"].rgba
        with self.assertRaisesRegex(verifier.VerificationError, "decoded PNG pixels did not change"):
            verifier.compare_suite(cases)

    def test_script_source_text_is_not_script_execution(self):
        offline = self.cases["offline_attempts"]
        self.assertIn("SCRIPT-RAN", offline.metadata["body_text"])
        verifier.fixture_expectations(offline)
        changed = deepcopy(offline)
        changed.elements["script-marker"]["text"] = "SCRIPT-RAN"
        with self.assertRaises(verifier.VerificationError):
            verifier.fixture_expectations(changed)

    def test_offline_suite_counter_requires_attempted_script_and_zero_actual_entries(self):
        for key, value in (("script_permission_attempts", 0), ("script_execution_entries", 1), ("script_enabled", True)):
            with self.subTest(key=key, value=value):
                cases = deepcopy(self.cases)
                cases["offline_attempts"].metadata[key] = value
                with self.assertRaises(verifier.VerificationError):
                    verifier.compare_suite(cases)

    def test_normal_suite_counter_rejects_unexpected_permission_attempt(self):
        cases = deepcopy(self.cases)
        cases["baseline"].metadata["script_permission_attempts"] = 1
        with self.assertRaisesRegex(verifier.VerificationError, "unexpected permission inquiry"):
            verifier.compare_suite(cases)

    def test_suite_requires_unique_native_execution_ids(self):
        cases = deepcopy(self.cases)
        cases["css_width"].request.run_id = cases["baseline"].request.run_id
        with self.assertRaisesRegex(verifier.VerificationError, "distinct run ID"):
            verifier.compare_suite(cases)

    def test_suite_manifest_rejects_missing_cases(self):
        with tempfile.TemporaryDirectory(prefix="scarlet-verifier-tests-") as directory:
            path = Path(directory) / "suite.json"
            path.write_text('{"cases":{"baseline":{}}}')
            with self.assertRaisesRegex(verifier.VerificationError, "seven named cases"):
                verifier.verify_suite(path)

    def test_full_synthetic_suite_manifest_with_transferred_paths(self):
        with tempfile.TemporaryDirectory(prefix="scarlet-verifier-tests-") as directory:
            root = Path(directory)
            entries = {}
            for name, artifact in self.cases.items():
                case_root = root / name
                case_root.mkdir()
                run = SyntheticRun(case_root)
                run.request.run_id = artifact.request.run_id
                run.request.native_prefix = f"/tmp/test-only/{name}/frame"
                run.request.native_input = "/share/test-only/" + verifier.FIXTURE_CASES[name]
                run.request.input.write_bytes(artifact.input_bytes)
                run.width, run.height = artifact.width, artifact.height
                run.bgra = artifact.bgra
                run.png = png_bytes(run.width, run.height, artifact.rgba)
                run.tree = artifact.tree.encode()
                run.metadata.update({
                    "run_id": run.request.run_id, "input": run.request.native_input,
                    "output_prefix": run.request.native_prefix, "input_bytes": len(artifact.input_bytes),
                    "input_fnv1a64": verifier.fnv1a64(artifact.input_bytes), "viewport": artifact.metadata["viewport"],
                    "stride": artifact.metadata["stride"], "pixel_bytes": len(run.bgra),
                    "pixel_fnv1a64": verifier.fnv1a64(run.bgra), "png_bytes": len(run.png),
                    "png_fnv1a64": verifier.fnv1a64(run.png), "render_tree_bytes": len(run.tree),
                    "render_tree_fnv1a64": verifier.fnv1a64(run.tree), "body_text": artifact.metadata["body_text"],
                    "elements": list(artifact.elements.values()),
                    "script_execution_entries": artifact.metadata["script_execution_entries"],
                    "script_permission_attempts": artifact.metadata["script_permission_attempts"],
                    "script_enabled": artifact.metadata["script_enabled"],
                    "files": {"bgra": run.request.native_prefix + ".bgra", "png": run.request.native_prefix + ".png",
                              "render_tree": run.request.native_prefix + ".render-tree.txt"},
                })
                run.publish()
                run.request.log.write_text(run.request.log.read_text().replace("case=protocol_test", "case=" + name))
                entries[name] = {"prefix": f"{name}/frame", "input": f"{name}/input.html", "run_id": run.request.run_id,
                                 "log": f"{name}/native.log", "native_prefix": run.request.native_prefix,
                                 "native_input": run.request.native_input, "size": f"{run.width}x{run.height}"}
            manifest = root / "suite.json"
            manifest.write_text(json.dumps({"cases": entries}))
            self.assertEqual(set(verifier.verify_suite(manifest)), set(verifier.FIXTURE_CASES))
            entries["baseline"]["size"] = "800x600"
            manifest.write_text(json.dumps({"cases": entries}))
            with self.assertRaisesRegex(verifier.VerificationError, "expected size"):
                verifier.verify_suite(manifest)


class SnapshotProfileTests(unittest.TestCase):
    """Synthetic imported-API tests; they are not snapshot rendering evidence."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="scarlet-verifier-snapshot-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.run = SyntheticRun(Path(self.temporary.name))
        self.css = b"/* TEST ONLY */ body { color: black; }\n"
        self.receipt = {"url": "https://snapshot.test/style.css", "outcome": "loaded", "mime": "text/css",
                        "bytes": len(self.css), "sha256": hashlib.sha256(self.css).hexdigest(),
                        "fnv1a64": verifier.fnv1a64(self.css)}
        self.run.metadata.update({
            "mode": "snapshot-resources-cpu", "resource_profile": "snapshotresources",
            "document_url": "https://snapshot.test/index.html", "snapshot_index": "/share/test-only/index.tsv",
            "snapshot_resource_root": "/share/test-only/assets", "subresources_enabled": True,
            "resources": {"transport": "exact-https-snapshot-bytes", "requests": 1, "loaded": 1,
                          "loaded_bytes": len(self.css), "blocked": 0, "failed": 0, "cancelled": 0, "pending": 0,
                          "resources": [self.receipt]},
        })
        self.publish()

    def publish(self):
        self.run.publish()
        marker = (f"SCARLET_SNAPSHOT_RESOURCE outcome=loaded url={self.receipt['url']} mime=text/css "
                  f"bytes={len(self.css)} sha256={self.receipt['sha256']} fnv1a64={self.receipt['fnv1a64']}\n")
        self.run.request.log.write_text(self.run.request.log.read_text().replace("SCARLET_SCRIPT_GUARD ", marker + "SCARLET_SCRIPT_GUARD "))

    def profile(self, validator):
        return verifier.SnapshotProfile("https://snapshot.test/index.html", "/share/test-only/index.tsv",
                                        "/share/test-only/assets", self.run.request.input.read_bytes(), validator)

    def site_validator(self, artifact):
        verifier.require(artifact.metadata["resources"]["resources"] == [self.receipt],
                         "test-only site: exact resource receipt differs")
        verifier.require(self.receipt["sha256"] in artifact.native_log and self.receipt["url"] in artifact.native_log,
                         "test-only site: actual native receipt log missing")
        verifier.require(artifact.elements["probe"]["text"] == "synthetic protocol text",
                         "test-only site: expected layout text missing")

    def test_explicit_snapshot_profile_calls_site_validator_after_common_checks(self):
        observed = []
        def validator(artifact):
            self.site_validator(artifact)
            observed.append((artifact.request.run_id, artifact.native_log))
        artifact = verifier.verify(self.run.request, snapshot_profile=self.profile(validator))
        self.assertEqual(artifact.metadata["mode"], "snapshot-resources-cpu")
        self.assertEqual(observed, [(self.run.request.run_id, artifact.native_log)])

    def test_metadata_never_selects_snapshot_profile_for_default_verify(self):
        with self.assertRaisesRegex(verifier.VerificationError, "unexpected mode"):
            verifier.verify(self.run.request)

    def test_snapshot_permission_attempts_do_not_inherit_offline_fixture_case_policy(self):
        self.run.request.case = "baseline"
        self.run.metadata["script_permission_attempts"] = 2
        self.publish()
        self.run.request.log.write_text(self.run.request.log.read_text().replace("case=protocol_test", "case=baseline"))
        verifier.verify(self.run.request, snapshot_profile=self.profile(self.site_validator))

    def test_snapshot_profile_is_frozen_and_has_exact_document_bytes(self):
        profile = self.profile(self.site_validator)
        with self.assertRaises(FrozenInstanceError):
            profile.document_url = "https://other.test/"
        for wrong_bytes in (b"other snapshot", "not bytes", b""):
            with self.subTest(wrong_bytes=wrong_bytes):
                with self.assertRaisesRegex(verifier.VerificationError, "exact document bytes"):
                    verifier.verify(self.run.request, snapshot_profile=replace(profile, input_bytes=wrong_bytes))

    def test_explicit_profile_type_and_validator_are_required(self):
        for invalid in ({"mode": "snapshot-resources-cpu"}, True, "snapshotresources"):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(verifier.VerificationError, "explicit SnapshotProfile"):
                    verifier.verify(self.run.request, snapshot_profile=invalid)
        with self.assertRaisesRegex(verifier.VerificationError, "site validator is required"):
            verifier.verify(self.run.request, snapshot_profile=self.profile(None))

    def test_snapshot_identity_fields_cannot_be_overridden_by_metadata(self):
        profile = self.profile(self.site_validator)
        for key, value in (("mode", "arbitrary-mode"), ("resource_profile", "host-browser"),
                           ("document_url", "https://other.test/"), ("snapshot_index", "/tmp/other-index.tsv"),
                           ("snapshot_resource_root", "/tmp/other-assets"), ("subresources_enabled", False)):
            with self.subTest(key=key):
                original = self.run.metadata[key]
                self.run.metadata[key] = value
                self.publish()
                with self.assertRaisesRegex(verifier.VerificationError, "unexpected " + key):
                    verifier.verify(self.run.request, snapshot_profile=profile)
                self.run.metadata[key] = original

    def test_invalid_profile_urls_and_native_paths_fail_closed(self):
        profile = self.profile(self.site_validator)
        for key, value in (("document_url", "http://snapshot.test/"), ("document_url", "https://user@host.test/"),
                           ("document_url", "https://snapshot.test/#fragment"), ("document_url", "https://[bad"),
                           ("document_url", "https://snapshot.test:bad/"), ("document_url", "https://snapshot.test:70000/"),
                           ("document_url", "https://snapshot.test:0/"), ("document_url", "https://snapshot.test/path\\child"),
                           ("snapshot_index", "relative/index.tsv"), ("snapshot_resource_root", "/tmp/bad\x00root")):
            with self.subTest(key=key, value=value):
                with self.assertRaises(verifier.VerificationError):
                    verifier.verify(self.run.request, snapshot_profile=replace(profile, **{key: value}))

    def test_snapshot_native_evidence_structure_and_counter_types(self):
        original = deepcopy(self.run.metadata["resources"])
        for wrong in (None, {}, {**original, "transport": "host-renderer"}, {**original, "pending": True},
                      {**original, "failed": -1}, {**original, "loaded_bytes": 2 ** 64},
                      {**original, "resources": {}}, {**original, "resources": ["untrusted receipt"]}):
            with self.subTest(wrong=wrong):
                self.run.metadata["resources"] = wrong
                self.publish()
                with self.assertRaisesRegex(verifier.VerificationError, "snapshot resources"):
                    verifier.verify(self.run.request, snapshot_profile=self.profile(self.site_validator))

    def test_site_validator_rejection_and_exception_never_fall_back(self):
        def rejected(artifact):
            raise verifier.VerificationError("site exact byte mismatch")
        def crashed(artifact):
            raise KeyError("missing trusted asset")
        for validator, message in ((rejected, "site exact byte mismatch"), (crashed, "site validator failed"),
                                   (lambda artifact: True, "must return None"), (lambda artifact: False, "must return None")):
            with self.subTest(validator=validator):
                with self.assertRaisesRegex(verifier.VerificationError, message):
                    verifier.verify(self.run.request, snapshot_profile=self.profile(validator))

    def test_callback_cannot_mutate_returned_common_evidence(self):
        def validator(artifact):
            artifact.metadata["scripts_executed"] = 999
            artifact.elements["probe"]["text"] = "mutated callback copy"
            artifact.request.run_id = "mutated-callback"
        artifact = verifier.verify(self.run.request, snapshot_profile=self.profile(validator))
        self.assertEqual(artifact.metadata["scripts_executed"], 0)
        self.assertEqual(artifact.elements["probe"]["text"], "synthetic protocol text")
        self.assertEqual(artifact.request.run_id, "test-only-001")

    def test_corrupted_common_artifact_never_reaches_callback(self):
        observed = []
        def validator(artifact):
            observed.append(artifact)
        data = bytearray(self.run.bgra)
        data[0] ^= 1
        self.run.artifact_path(".bgra").write_bytes(data)
        with self.assertRaisesRegex(verifier.VerificationError, "pixel_fnv1a64"):
            verifier.verify(self.run.request, snapshot_profile=self.profile(validator))
        self.assertEqual(observed, [])

    def test_site_callback_rejects_missing_or_wrong_actual_asset_receipts(self):
        for receipts in ([], [{**self.receipt, "url": "https://other.test/style.css"}],
                         [{**self.receipt, "sha256": hashlib.sha256(b"same size other bytes").hexdigest()}]):
            with self.subTest(receipts=receipts):
                self.run.metadata["resources"]["resources"] = receipts
                self.publish()
                with self.assertRaisesRegex(verifier.VerificationError, "exact resource receipt differs"):
                    verifier.verify(self.run.request, snapshot_profile=self.profile(self.site_validator))


if __name__ == "__main__":
    unittest.main()

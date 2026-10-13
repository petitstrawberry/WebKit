#!/usr/bin/env python3
"""Verify transferred schema-2 native runner artifacts, never invoke a renderer.

Examples:
  python3 verify.py verify --prefix ./out/base --input fixtures/offline-proof.html \
    --native-prefix /tmp/base --native-input /share/webcore/offline-proof.html \
    --run-id base-20261013 --log ./out/base.log --expect-element probe
  python3 verify.py suite ./out/suite.json

A suite manifest has a `cases` object with baseline, css_width, css_color,
text_change, font_size, viewport_narrow and offline_attempts entries. Each entry
contains prefix, input, run_id and log, plus optional native_prefix/native_input
and size (for example "800x600"). Matching outer CASE_BEGIN/CASE_END records
with native exit code zero are mandatory; a renderer PASS alone is insufficient.
The script guard must report zero actual VM execution entries and disabled
scripts. The offline-attempts fixture must still trigger permission inquiries.
Host paths are relative to the manifest; native paths retain their exact spelling.
Each input must match the corresponding bundled fixture byte for byte. The suite
compares actual selected DOM metrics and decoded pixels, not filename labels.

FNV is only an integrity/correlation checksum. Matching artifacts and a supplied
log do not independently establish where an executable ran. Unit tests generate
explicitly synthetic protocol data and are not native rendering evidence.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
from dataclasses import dataclass
from html.parser import HTMLParser
import json
import math
from pathlib import Path
import re
import struct
import sys
from typing import Callable
from urllib.parse import urlsplit
import zlib


REVISION = "7024b36074b0584a72f9d91406ed3b13548fb4e0"
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
MAX_INPUT_BYTES = 1024 * 1024
MAX_METADATA_BYTES = 8 * 1024 * 1024
HASH_PATTERN = re.compile(r"[0-9a-f]{16}\Z")
RUN_PATTERN = re.compile(r"[A-Za-z0-9_-]{1,96}\Z")
FIXTURE_DIRECTORY = Path(__file__).resolve().parent / "fixtures"
FIXTURE_CASES = {
    "baseline": "offline-proof.html", "css_width": "css-width.html",
    "css_color": "css-color.html", "text_change": "text-change.html",
    "font_size": "font-size.html", "viewport_narrow": "offline-proof.html",
    "offline_attempts": "offline-attempts.html",
}
REQUIRED_ELEMENTS = ("probe", "flow", "label", "tile-a", "script-marker")


class VerificationError(Exception):
    """The provided evidence is missing, inconsistent or outside the contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def fnv1a64(data: bytes) -> str:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def read_bounded(path: Path, limit: int, label: str) -> bytes:
    try:
        with path.open("rb") as stream:
            data = stream.read(limit + 1)
    except (OSError, ValueError) as error:
        raise VerificationError(f"{label}: cannot read {path}: {error}") from error
    require(len(data) <= limit, f"{label}: exceeds {limit} bytes")
    require(bool(data), f"{label}: empty file")
    return data


def strict_json(data: bytes, label: str) -> dict:
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f"{label}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    def constant(value):
        raise VerificationError(f"{label}: non-finite JSON number {value}")

    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                            parse_constant=constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise VerificationError(f"{label}: invalid UTF-8 JSON: {error}") from error
    require(type(result) is dict, f"{label}: expected JSON object")
    pending = [result]
    while pending:
        value = pending.pop()
        if type(value) is str:
            try:
                value.encode("utf-8")
            except UnicodeError as error:
                raise VerificationError(f"{label}: invalid Unicode string") from error
        elif type(value) is float:
            require(math.isfinite(value), f"{label}: non-finite JSON number")
        elif type(value) is dict:
            pending.extend(value.keys())
            pending.extend(value.values())
        elif type(value) is list:
            pending.extend(value)
    return result


def integer(value, name: str, minimum: int = 0, maximum: int | None = None) -> int:
    require(type(value) is int, f"{name}: expected integer")
    require(value >= minimum and (maximum is None or value <= maximum),
            f"{name}: integer outside allowed range")
    return value


def finite_number(value) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def matching_hash(data: bytes, value, name: str) -> None:
    require(type(value) is str and HASH_PATTERN.fullmatch(value) is not None,
            f"{name}: expected lowercase 16-digit FNV-1a-64")
    require(fnv1a64(data) == value, f"{name}: bytes do not match checksum")


def decode_png(data: bytes, width: int, height: int) -> bytes:
    """Validate Cairo RGB/RGBA PNG structure/CRCs and return straight RGBA bytes."""
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), "PNG: invalid signature")
    offset, chunks, compressed = 8, [], []
    color_type = None
    finished = False
    idat_finished = False
    while offset < len(data):
        require(offset + 12 <= len(data), "PNG: truncated chunk header")
        length, kind = struct.unpack_from(">I4s", data, offset)
        require(length <= MAX_ARTIFACT_BYTES and offset + 12 + length <= len(data),
                "PNG: truncated or oversized chunk")
        payload = data[offset + 8:offset + 8 + length]
        checksum, = struct.unpack_from(">I", data, offset + 8 + length)
        require(zlib.crc32(kind + payload) & 0xFFFFFFFF == checksum,
                f"PNG: invalid {kind!r} CRC")
        require(re.fullmatch(rb"[A-Za-z]{4}", kind) is not None, "PNG: invalid chunk type")
        if not chunks:
            require(kind == b"IHDR", "PNG: IHDR must be first")
        if kind == b"IHDR":
            require(not chunks and length == 13, "PNG: duplicate or malformed IHDR")
            png_width, png_height, depth, color_type, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            require((png_width, png_height) == (width, height), "PNG: viewport dimensions differ")
            require(depth == 8 and color_type in (2, 6) and compression == 0
                    and filtering == 0 and interlace == 0,
                    "PNG: expected noninterlaced 8-bit Cairo RGB/RGBA")
        elif kind == b"IDAT":
            require(not idat_finished, "PNG: noncontiguous IDAT chunks")
            compressed.append(payload)
        elif kind == b"IEND":
            require(length == 0 and bool(compressed), "PNG: malformed or premature IEND")
            finished = True
            offset += 12
            require(offset == len(data), "PNG: bytes after IEND")
            break
        else:
            if compressed:
                idat_finished = True
            # Cairo can emit optional color-space metadata. Palette/transparent
            # color encodings are outside this opaque CPU runner's RGB contract.
            require(kind[0] & 32 and kind != b"tRNS", f"PNG: unsupported chunk {kind!r}")
        chunks.append(kind)
        offset += 12 + length
    require(finished, "PNG: IEND missing")
    channels = 3 if color_type == 2 else 4
    row_bytes = width * channels
    expected = (row_bytes + 1) * height
    decoder = zlib.decompressobj()
    try:
        scanlines = decoder.decompress(b"".join(compressed), expected + 1)
    except zlib.error as error:
        raise VerificationError(f"PNG: invalid zlib data: {error}") from error
    require(len(scanlines) == expected and decoder.eof and not decoder.unused_data
            and not decoder.unconsumed_tail, "PNG: decompressed byte count or stream termination differs")
    rgba = bytearray(width * height * 4)
    previous = bytearray(row_bytes)
    for y in range(height):
        start = y * (row_bytes + 1)
        filtering = scanlines[start]
        require(filtering <= 4, "PNG: invalid row filter")
        row = bytearray(scanlines[start + 1:start + row_bytes + 1])
        for x in range(row_bytes):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if filtering == 1:
                predictor = left
            elif filtering == 2:
                predictor = above
            elif filtering == 3:
                predictor = (left + above) // 2
            elif filtering == 4:
                prediction = left + above - upper_left
                distances = (abs(prediction - left), abs(prediction - above), abs(prediction - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        base = y * width * 4
        if channels == 4:
            rgba[base:base + width * 4] = row
        else:
            for x in range(width):
                rgba[base + x * 4:base + x * 4 + 4] = row[x * 3:x * 3 + 3] + b"\xff"
        previous = row
    return bytes(rgba)


def match_pixels(bgra: bytes, rgba: bytes, width: int, height: int, stride: int) -> None:
    for y in range(height):
        for x in range(width):
            pixel = y * stride + x * 4
            image = (y * width + x) * 4
            b, g, r, a = bgra[pixel:pixel + 4]
            png_r, png_g, png_b, png_a = rgba[image:image + 4]
            require(a == png_a, f"PNG/BGRA: alpha differs at {x},{y}")
            # Cairo unpremultiplication can round one channel by one unit.
            tolerance = 0 if a == 255 else 1
            for actual, unmultiplied in ((r, png_r), (g, png_g), (b, png_b)):
                require(abs(actual - (unmultiplied * a + 127) // 255) <= tolerance,
                        f"PNG/BGRA: color differs at {x},{y}")


def check_script_profile(metadata: dict, case: str | None = None) -> int:
    for name in ("scripts_executed", "script_execution_entries"):
        require(type(metadata.get(name)) is int and metadata[name] == 0,
                f"script guard: expected zero {name}")
    require(metadata.get("script_enabled") is False, "script guard: scripts must be disabled")
    attempts = integer(metadata.get("script_permission_attempts"), "script_permission_attempts", 0, 0xFFFFFFFFFFFFFFFF)
    if case == "offline_attempts":
        require(attempts > 0, "script guard: offline attempts did not exercise a permission inquiry")
    elif case in FIXTURE_CASES:
        require(attempts == 0, f"script guard: unexpected permission inquiry in {case}")
    return attempts


def parse_native_log(log: str, run_id: str, expected_case: str | None,
                     metadata: dict, *, enforce_fixture_permissions: bool = True) -> tuple[dict[str, str], dict[str, str]]:
    require("SCARLET_WEBCORE_ERROR" not in log, "native log contains runner failure")
    records = {}
    offsets = {}
    markers = ("NATIVE_WEBCORE_CASE_BEGIN", "SCARLET_WEBCORE_BEGIN", "SCARLET_SCRIPT_GUARD",
               "SCARLET_WEBCORE_PASS", "NATIVE_WEBCORE_CASE_END")
    for marker in markers:
        lines = list(re.finditer(rf"^{marker}[ \t]+([^\r\n]*)\r?$", log, re.MULTILINE))
        require(len(lines) == 1, f"native log: expected exactly one {marker} record")
        offsets[marker] = lines[0].start()
        record = {}
        for token in lines[0].group(1).split():
            require("=" in token, f"native log: malformed {marker} token")
            key, value = token.split("=", 1)
            require(bool(key) and bool(value) and key not in record,
                    f"native log: empty or duplicate {marker} field")
            record[key] = value
        records[marker] = record
    positions = [offsets[marker] for marker in markers]
    require(positions == sorted(positions), "native log: execution markers out of order")
    outer_begin, outer_end = records[markers[0]], records[markers[-1]]
    case = expected_case if expected_case is not None else outer_begin.get("case")
    require(type(case) is str and RUN_PATTERN.fullmatch(case) is not None,
            "native log: invalid expected case")
    for record in (outer_begin, outer_end):
        require(record.get("run_id") == run_id and record.get("case") == case,
                "native log: outer case/run ID mismatch")
    require(outer_end.get("success") == "true" and outer_end.get("code") == "0",
            "native log: native child did not exit successfully")
    attempts = check_script_profile(metadata, case if enforce_fixture_permissions else None)
    guard = records["SCARLET_SCRIPT_GUARD"]
    for name, expected in (("run_id", run_id), ("execution_entries", "0"),
                           ("permission_attempts", str(attempts)), ("enabled", "false")):
        require(guard.get(name) == expected, f"native log SCRIPT_GUARD: unexpected {name}")
    return records["SCARLET_WEBCORE_BEGIN"], records["SCARLET_WEBCORE_PASS"]


@dataclass
class Request:
    prefix: Path
    input: Path
    run_id: str
    log: Path
    native_prefix: str | None = None
    native_input: str | None = None
    case: str | None = None


@dataclass
class VerifiedArtifact:
    request: Request
    metadata: dict
    bgra: bytes
    rgba: bytes
    tree: str
    input_bytes: bytes
    elements: dict[str, dict]
    native_log: str = ""

    @property
    def width(self) -> int:
        return self.metadata["viewport"]["width"]

    @property
    def height(self) -> int:
        return self.metadata["viewport"]["height"]


@dataclass(frozen=True)
class SnapshotProfile:
    """Explicit imported-API contract for a trusted site verification function.

    The site verifier freezes its manifest/index expectations independently and
    supplies the exact expected document bytes. `validator` must validate native
    receipts against those expectations and validate site layout. It receives
    an independent artifact copy after the shared checks pass, returns None on
    success and raises VerificationError on failure. It is trusted verifier code;
    registering a callback alone does not establish snapshot validity.
    """
    document_url: str
    snapshot_index: str
    snapshot_resource_root: str
    input_bytes: bytes
    validator: Callable[[VerifiedArtifact], None]


def snapshot_expectations(profile: SnapshotProfile, input_bytes: bytes) -> dict:
    require(type(profile) is SnapshotProfile, "snapshot profile: expected explicit SnapshotProfile")
    require(type(profile.input_bytes) is bytes and bool(profile.input_bytes)
            and profile.input_bytes == input_bytes, "snapshot profile: exact document bytes differ")
    require(callable(profile.validator), "snapshot profile: site validator is required")
    require(type(profile.document_url) is str and len(profile.document_url) <= 4096
            and "\\" not in profile.document_url
            and not any(character.isspace() or ord(character) < 32 for character in profile.document_url),
            "snapshot profile: invalid document URL")
    try:
        url = urlsplit(profile.document_url)
        port = url.port
    except ValueError as error:
        raise VerificationError("snapshot profile: invalid document URL") from error
    require(url.scheme == "https" and bool(url.hostname) and url.username is None
            and url.password is None and not url.fragment and (port is None or 1 <= port <= 65535),
            "snapshot profile: expected absolute HTTPS document URL")
    for name in ("snapshot_index", "snapshot_resource_root"):
        value = getattr(profile, name)
        require(type(value) is str and 0 < len(value) <= 512 and value.startswith("/")
                and not any(ord(character) < 32 for character in value), f"snapshot profile: invalid {name}")
    return {"mode": "snapshot-resources-cpu", "resource_profile": "snapshotresources",
            "document_url": profile.document_url, "snapshot_index": profile.snapshot_index,
            "snapshot_resource_root": profile.snapshot_resource_root, "subresources_enabled": True}


def snapshot_evidence_shape(metadata: dict) -> None:
    evidence = metadata.get("resources")
    require(type(evidence) is dict and evidence.get("transport") == "exact-https-snapshot-bytes",
            "snapshot resources: expected exact HTTPS snapshot evidence")
    for name in ("requests", "loaded", "loaded_bytes", "blocked", "failed", "cancelled", "pending"):
        integer(evidence.get(name), "snapshot resources." + name, 0, 0xFFFFFFFFFFFFFFFF)
    require(type(evidence.get("resources")) is list, "snapshot resources: native receipt array missing")
    require(all(type(receipt) is dict for receipt in evidence["resources"]),
            "snapshot resources: invalid native receipt object")


def verify(request: Request, expected_elements=(), expected_text=None,
           expected_size: tuple[int, int] | None = None, *,
           snapshot_profile: SnapshotProfile | None = None) -> VerifiedArtifact:
    require(type(request.run_id) is str and RUN_PATTERN.fullmatch(request.run_id) is not None,
            "expected run ID: invalid token")
    native_prefix = request.native_prefix if request.native_prefix is not None else str(request.prefix)
    native_input = request.native_input if request.native_input is not None else str(request.input)
    require(type(native_prefix) is str and bool(native_prefix), "expected native prefix is empty")
    require(type(native_input) is str and bool(native_input), "expected native input is empty")
    input_bytes = read_bounded(request.input, MAX_INPUT_BYTES, "input")
    try:
        input_bytes.decode("utf-8")
    except UnicodeError as error:
        raise VerificationError("input: not UTF-8") from error
    metadata_bytes = read_bounded(Path(str(request.prefix) + ".json"), MAX_METADATA_BYTES, "metadata")
    metadata = strict_json(metadata_bytes, "metadata")
    expected = {
        "schema": 2, "engine": "WebCore", "port": "Scarlet", "mode": "offline-direct-cpu",
        "revision": REVISION, "run_id": request.run_id, "input": native_input,
        "output_prefix": native_prefix, "completion": "native-render-success",
        "pixel_format": "BGRA8-premultiplied-little-endian", "render_tree_present": True,
        "layout_complete": True, "scripts_executed": 0, "script_execution_entries": 0,
        "script_enabled": False, "subresources_enabled": False,
        "virtual_display_equals_viewport": True,
    }
    if snapshot_profile is not None:
        expected.update(snapshot_expectations(snapshot_profile, input_bytes))
    for name, value in expected.items():
        require(type(metadata.get(name)) is type(value) and metadata[name] == value,
                f"metadata: unexpected {name}")
    check_script_profile(metadata, request.case if snapshot_profile is None else None)
    if snapshot_profile is not None:
        snapshot_evidence_shape(metadata)
    require(integer(metadata.get("input_bytes"), "input_bytes", 1, MAX_INPUT_BYTES) == len(input_bytes),
            "input: byte count differs")
    matching_hash(input_bytes, metadata.get("input_fnv1a64"), "input_fnv1a64")
    viewport = metadata.get("viewport")
    require(type(viewport) is dict, "viewport: expected object")
    width = integer(viewport.get("width"), "viewport.width", 1, 4096)
    height = integer(viewport.get("height"), "viewport.height", 1, 4096)
    if expected_size is not None:
        require((width, height) == expected_size, "viewport: differs from expected size")
    stride = integer(metadata.get("stride"), "stride", width * 4, MAX_ARTIFACT_BYTES)
    require(stride % 4 == 0, "stride: not aligned to four bytes")
    pixel_bytes = integer(metadata.get("pixel_bytes"), "pixel_bytes", 1, MAX_ARTIFACT_BYTES)
    require(pixel_bytes == stride * height, "pixel_bytes: differs from stride times height")
    files = metadata.get("files")
    require(type(files) is dict, "files: expected object")
    for name, suffix in (("bgra", ".bgra"), ("png", ".png"), ("render_tree", ".render-tree.txt")):
        require(files.get(name) == native_prefix + suffix, f"files.{name}: unexpected native output path")
    bgra = read_bounded(Path(str(request.prefix) + ".bgra"), MAX_ARTIFACT_BYTES, "BGRA")
    png = read_bounded(Path(str(request.prefix) + ".png"), MAX_ARTIFACT_BYTES, "PNG")
    tree_bytes = read_bounded(Path(str(request.prefix) + ".render-tree.txt"), MAX_ARTIFACT_BYTES, "render tree")
    for data, length_key, hash_key in (
            (bgra, "pixel_bytes", "pixel_fnv1a64"),
            (png, "png_bytes", "png_fnv1a64"),
            (tree_bytes, "render_tree_bytes", "render_tree_fnv1a64")):
        require(integer(metadata.get(length_key), length_key, 1, MAX_ARTIFACT_BYTES) == len(data),
                f"{length_key}: byte count differs")
        matching_hash(data, metadata.get(hash_key), hash_key)
    try:
        tree = tree_bytes.decode("utf-8")
    except UnicodeError as error:
        raise VerificationError("render tree: not UTF-8") from error
    require("\x00" not in tree and re.search(r"\bRenderView\b", tree) is not None
            and re.search(r"\bRender(?:Block|Box|Text|FlexibleBox|Inline)", tree) is not None,
            "render tree: no recognizable WebCore RenderView/content structure")
    rgba = decode_png(png, width, height)
    match_pixels(bgra, rgba, width, height, stride)
    body_text = metadata.get("body_text")
    require(type(body_text) is str and bool(body_text.strip()), "body_text: empty or invalid")
    raw_elements = metadata.get("elements")
    require(type(raw_elements) is list and 0 < len(raw_elements) <= 16,
            "elements: expected one to sixteen selected elements")
    elements = {}
    for item in raw_elements:
        require(type(item) is dict, "elements: expected element object")
        element_id = item.get("id")
        require(type(element_id) is str and 0 < len(element_id.encode("utf-8")) <= 128
                and element_id not in elements, "elements: invalid or duplicate ID")
        require(type(item.get("found")) is bool, f"element {element_id}: invalid found flag")
        if item["found"]:
            bounds = item.get("bounds")
            require(type(bounds) is dict, f"element {element_id}: bounds missing")
            for name in ("x", "y", "width", "height"):
                value = bounds.get(name)
                require(finite_number(value)
                        and (name in ("x", "y") or value >= 0),
                        f"element {element_id}: invalid {name}")
            require(type(item.get("text")) is str, f"element {element_id}: text missing")
            require(item["text"] in body_text, f"element {element_id}: text absent from body_text")
        else:
            require("bounds" not in item and "text" not in item,
                    f"element {element_id}: missing element has measured data")
        elements[element_id] = item
    for element_id in expected_elements:
        require(element_id in elements and elements[element_id]["found"],
                f"expected element {element_id}: not found")
        rect = elements[element_id]["bounds"]
        require(rect["width"] > 0 and rect["height"] > 0,
                f"expected element {element_id}: empty bounds")
    for element_id, text in (expected_text or {}).items():
        require(element_id in elements and elements[element_id]["found"]
                and elements[element_id]["text"] == text,
                f"expected element {element_id}: text differs")
    fonts = metadata.get("font_matches")
    require(type(fonts) is list and len(fonts) == 3, "font_matches: expected three native generic-family matches")
    families = set()
    for font in fonts:
        require(type(font) is dict and font.get("requested") in ("serif", "sans-serif", "monospace")
                and font["requested"] not in families, "font_matches: invalid or duplicate requested family")
        families.add(font["requested"])
        require(type(font.get("family")) is str and bool(font["family"])
                and type(font.get("file")) is str and font["file"].startswith("/"),
                "font_matches: native family/file missing")
    try:
        log = read_bounded(request.log, MAX_METADATA_BYTES, "native log").decode("utf-8")
    except UnicodeError as error:
        raise VerificationError("native log: not UTF-8") from error
    begin, success = parse_native_log(log, request.run_id, request.case, metadata,
                                      enforce_fixture_permissions=snapshot_profile is None)
    begin_expected = {
        "revision": REVISION, "run_id": request.run_id,
        "input_bytes": str(len(input_bytes)), "input_fnv1a64": metadata["input_fnv1a64"],
        "viewport": f"{width}x{height}", "output": native_prefix,
    }
    pass_expected = {
        "run_id": request.run_id, "viewport": f"{width}x{height}", "stride": str(stride),
        "bytes": str(pixel_bytes), "pixel_fnv1a64": metadata["pixel_fnv1a64"],
        "scripts": "0", "output": native_prefix, "metadata_fnv1a64": fnv1a64(metadata_bytes),
    }
    for record, expected_fields, label in ((begin, begin_expected, "BEGIN"), (success, pass_expected, "PASS")):
        for name, value in expected_fields.items():
            require(record.get(name) == value, f"native log {label}: unexpected {name}")
    for name in ("revision", "input_bytes", "input_fnv1a64"):
        if name in success:
            require(success[name] == begin_expected[name], f"native log PASS: unexpected {name}")
    artifact = VerifiedArtifact(request, metadata, bgra, rgba, tree, input_bytes, elements, log)
    if snapshot_profile is not None:
        try:
            result = snapshot_profile.validator(deepcopy(artifact))
        except VerificationError:
            raise
        except Exception as error:
            raise VerificationError(f"snapshot site validator failed: {error}") from error
        require(result is None, "snapshot site validator must return None after verification")
    return artifact


class FixtureParser(HTMLParser):
    """Collect fixture textContent; deliberately limited to the bundled HTML."""
    VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}

    def __init__(self, data: bytes):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.texts = {}
        self.body = []
        self.feed(data.decode("utf-8"))
        self.close()

    def handle_starttag(self, tag, attrs):
        element_id = dict(attrs).get("id")
        if element_id:
            require(element_id not in self.texts, "fixture: duplicate ID")
            self.texts[element_id] = []
        if tag not in self.VOID:
            self.stack.append((tag, element_id))

    def handle_endtag(self, tag):
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index][0] == tag:
                del self.stack[index:]
                break

    def handle_data(self, data):
        if any(tag == "body" for tag, _ in self.stack):
            self.body.append(data)
        for _, element_id in self.stack:
            if element_id:
                self.texts[element_id].append(data)


def fixture_expectations(artifact: VerifiedArtifact) -> None:
    check_script_profile(artifact.metadata, artifact.request.case)
    parser = FixtureParser(artifact.input_bytes)
    require(" ".join(artifact.metadata["body_text"].split()) == " ".join("".join(parser.body).split()),
            "fixture: native body text differs from HTML textContent")
    for element_id in REQUIRED_ELEMENTS:
        element = artifact.elements.get(element_id)
        require(element is not None and element["found"], f"fixture: {element_id} not found")
        require(element["text"] == "".join(parser.texts[element_id]),
                f"fixture: native {element_id} text differs from input")
        rect = element["bounds"]
        require(rect["width"] > 0 and rect["height"] > 0, f"fixture: {element_id} empty bounds")
    require(artifact.elements["script-marker"]["text"] == "script-disabled",
            "fixture: script marker was changed")
    require("script-disabled" in artifact.tree, "fixture: script marker absent from actual render tree")
    require(all(alpha == 255 for alpha in artifact.bgra[3::4]), "fixture: expected opaque CPU frame")


def close_to(actual: float, expected: float, label: str) -> None:
    require(abs(actual - expected) <= 0.01, f"{label}: expected {expected}, got {actual}")


def rect(artifact: VerifiedArtifact, element_id: str) -> dict:
    return artifact.elements[element_id]["bounds"]


def changed(base: VerifiedArtifact, variant: VerifiedArtifact, label: str) -> None:
    require(base.bgra != variant.bgra, f"{label}: actual pixels did not change")
    require(base.rgba != variant.rgba, f"{label}: decoded PNG pixels did not change")
    require(base.tree != variant.tree, f"{label}: actual render tree did not change")


def color_at(artifact: VerifiedArtifact, x: int, y: int) -> tuple[int, int, int, int]:
    require(0 <= x < artifact.width and 0 <= y < artifact.height, "color sample outside viewport")
    offset = y * artifact.metadata["stride"] + x * 4
    b, g, r, a = artifact.bgra[offset:offset + 4]
    return r, g, b, a


def compare_suite(cases: dict[str, VerifiedArtifact]) -> None:
    require(set(cases) == set(FIXTURE_CASES), "suite: exactly the seven named cases are required")
    base = cases["baseline"]
    require(base.width >= 640 and base.height >= 480, "suite: baseline viewport must be at least 640x480")
    for name, artifact in cases.items():
        expected_input = read_bounded(FIXTURE_DIRECTORY / FIXTURE_CASES[name], MAX_INPUT_BYTES, "bundled fixture")
        require(artifact.input_bytes == expected_input, f"suite {name}: input differs from bundled fixture")
        check_script_profile(artifact.metadata, name)
        fixture_expectations(artifact)
        if name != "viewport_narrow":
            require((artifact.width, artifact.height) == (base.width, base.height),
                    f"suite {name}: viewport must match baseline")
        close_to(rect(artifact, "probe")["width"], 480 if name == "css_width" else 320,
                 f"suite {name} probe width")
        close_to(rect(artifact, "flow")["width"], artifact.width - 48, f"suite {name} flow width")
    require(len({item.request.run_id for item in cases.values()}) == len(cases),
            "suite: each native execution needs a distinct run ID")
    require(len({item.request.prefix.resolve() for item in cases.values()}) == len(cases),
            "suite: each case needs a distinct artifact prefix")
    width = cases["css_width"]
    changed(base, width, "CSS width")
    for axis in ("x", "y"):
        close_to(rect(width, "probe")[axis], rect(base, "probe")[axis], f"CSS width probe {axis}")
    require(rect(width, "probe")["height"] <= rect(base, "probe")["height"],
            "CSS width: wider probe unexpectedly became taller")
    color = cases["css_color"]
    require(base.bgra != color.bgra, "CSS color: actual pixels did not change")
    require(base.rgba != color.rgba, "CSS color: decoded PNG pixels did not change")
    for element_id in REQUIRED_ELEMENTS:
        require(base.elements[element_id] == color.elements[element_id],
                f"CSS color: {element_id} layout or text changed")
    probe = rect(base, "probe")
    sample_x, sample_y = math.ceil(probe["x"]) + 5, math.ceil(probe["y"]) + 5
    require(color_at(base, sample_x, sample_y) == (128, 201, 232, 255),
            "CSS color: baseline probe padding is not the fixture color")
    require(color_at(color, sample_x, sample_y) == (242, 170, 96, 255),
            "CSS color: changed probe padding is not the fixture color")
    # A background-only CSS edit must leave pixels outside the box identical.
    left, top = math.floor(probe["x"]), math.floor(probe["y"])
    right, bottom = math.ceil(probe["x"] + probe["width"]), math.ceil(probe["y"] + probe["height"])
    for y in range(base.height):
        for x in range(base.width):
            if left <= x < right and top <= y < bottom:
                continue
            offset = y * base.metadata["stride"] + x * 4
            color_offset = y * color.metadata["stride"] + x * 4
            require(base.bgra[offset:offset + 4] == color.bgra[color_offset:color_offset + 4],
                    f"CSS color: unrelated pixel changed at {x},{y}")
    text = cases["text_change"]
    changed(base, text, "DOM text")
    require(text.elements["label"]["text"] != base.elements["label"]["text"],
            "DOM text: label did not change")
    require(rect(text, "label")["width"] != rect(base, "label")["width"],
            "DOM text: label measurement did not change")
    font = cases["font_size"]
    changed(base, font, "font size")
    close_to(rect(base, "label")["height"], 27, "baseline label line height")
    close_to(rect(font, "label")["height"], 33, "font-size label line height")
    require(rect(font, "flow")["height"] > rect(base, "flow")["height"],
            "font size: flow height did not increase")
    narrow = cases["viewport_narrow"]
    require(320 <= narrow.width <= 480 and narrow.width < base.width and narrow.height == base.height,
            "viewport: expected a 320..480 wide case at the baseline height")
    changed(base, narrow, "viewport")
    require(rect(narrow, "flow")["height"] > rect(base, "flow")["height"],
            "viewport: flow did not wrap to a greater height")
    require(rect(narrow, "tile-a")["width"] < rect(base, "tile-a")["width"],
            "viewport: flex item did not become narrower")
    offline = cases["offline_attempts"]
    for element_id in REQUIRED_ELEMENTS:
        require(offline.elements[element_id] == base.elements[element_id],
                f"offline attempts: external attempts changed {element_id}")


def verify_suite(manifest_path: Path) -> dict[str, VerifiedArtifact]:
    manifest = strict_json(read_bounded(manifest_path, MAX_METADATA_BYTES, "suite manifest"), "suite manifest")
    entries = manifest.get("cases")
    require(type(entries) is dict and set(entries) == set(FIXTURE_CASES),
            "suite manifest: exactly the seven named cases are required")
    cases = {}
    for name, entry in entries.items():
        require(type(entry) is dict, f"suite {name}: expected object")
        for key in ("prefix", "input", "run_id", "log"):
            require(type(entry.get(key)) is str and bool(entry[key]), f"suite {name}: {key} missing")
        for key in ("native_prefix", "native_input"):
            require(key not in entry or (type(entry[key]) is str and bool(entry[key])),
                    f"suite {name}: invalid {key}")
        expected_size = None
        if "size" in entry:
            require(type(entry["size"]) is str, f"suite {name}: invalid size")
            try:
                expected_size = parse_size(entry["size"])
            except argparse.ArgumentTypeError as error:
                raise VerificationError(f"suite {name}: invalid size: {error}") from error
        def host_path(key):
            path = Path(entry[key])
            return path if path.is_absolute() else manifest_path.parent / path
        request = Request(host_path("prefix"), host_path("input"), entry["run_id"], host_path("log"),
                          entry.get("native_prefix", entry["prefix"]), entry.get("native_input", entry["input"]), name)
        try:
            cases[name] = verify(request, REQUIRED_ELEMENTS, expected_size=expected_size)
        except VerificationError as error:
            raise VerificationError(f"suite {name}: {error}") from error
    compare_suite(cases)
    return cases


def parse_size(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"([0-9]{1,4})x([0-9]{1,4})", value)
    if match is None or any(not 1 <= int(item) <= 4096 for item in match.groups()):
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT with dimensions 1..4096")
    return tuple(map(int, match.groups()))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    single = commands.add_parser("verify", help="verify one native run and transferred artifact set")
    for name in ("prefix", "input", "run-id", "log"):
        single.add_argument("--" + name, required=True)
    single.add_argument("--native-prefix")
    single.add_argument("--native-input")
    single.add_argument("--case", help="require the outer native harness case name (exit code zero is always required)")
    single.add_argument("--expect-element", action="append", default=[])
    single.add_argument("--expect-text", action="append", default=[], metavar="ID=TEXT")
    single.add_argument("--size", type=parse_size, help="require a specific native viewport")
    suite = commands.add_parser("suite", help="verify seven native runs and fixture causal comparisons")
    suite.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "suite":
            cases = verify_suite(args.manifest)
            print(f"SCARLET_ARTIFACT_SUITE_VERIFIED cases={len(cases)}")
        else:
            expected_text = {}
            for expression in args.expect_text:
                require("=" in expression, "--expect-text: expected ID=TEXT")
                element_id, text = expression.split("=", 1)
                require(bool(element_id) and element_id not in expected_text,
                        "--expect-text: empty or duplicate ID")
                expected_text[element_id] = text
            artifact = verify(Request(Path(args.prefix), Path(args.input), args.run_id, Path(args.log),
                                      args.native_prefix, args.native_input, args.case),
                              args.expect_element, expected_text, args.size)
            print(f"SCARLET_ARTIFACT_VERIFIED run_id={args.run_id} viewport={artifact.width}x{artifact.height}")
    except VerificationError as error:
        print(f"SCARLET_ARTIFACT_REJECTED {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

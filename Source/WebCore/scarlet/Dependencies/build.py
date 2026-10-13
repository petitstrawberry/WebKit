#!/usr/bin/env python3
"""Build the pinned Scarlet CPU dependencies using an installed native SDK."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request


HERE = Path(__file__).resolve().parent
SOURCE_ROOT = HERE.parents[3]
DEPENDENCIES = (
    "icu", "zlib", "libpng", "freetype", "expat", "pixman", "fontconfig",
    "cairo", "harfbuzz", "libxml2", "sqlite", "libjpeg-turbo", "libwebp",
    "curl", "libpsl", "openssl",
)
# Build changed forks and their native prerequisites first, so a new port's
# declaration/configuration gaps appear before the remaining release libraries.
BUILD_ORDER = (
    "icu", "zlib", "libpng", "freetype", "expat", "pixman", "fontconfig", "openssl", "curl", "libpsl",
    "cairo", "harfbuzz", "libxml2", "sqlite", "libjpeg-turbo", "libwebp",
)
STATIC_CMAKE_PROFILE = '''set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
if(PROJECT_NAME STREQUAL "zlib")
    # Both upstream zlib targets otherwise request libz.a after static conversion.
    cmake_language(DEFER CALL set_target_properties zlibstatic PROPERTIES OUTPUT_NAME zlibstatic)
endif()
'''
CM_OPTIONS = {
    "zlib": ["ZLIB_BUILD_EXAMPLES=OFF"],
    "libpng": ["PNG_SHARED=OFF", "PNG_STATIC=ON", "PNG_TESTS=OFF", "PNG_TOOLS=OFF",
               "PNG_EXECUTABLES=OFF", "PNG_HARDWARE_OPTIMIZATIONS=OFF"],
    "freetype": ["FT_DISABLE_BZIP2=ON", "FT_DISABLE_BROTLI=ON", "FT_DISABLE_HARFBUZZ=ON",
                 "FT_REQUIRE_ZLIB=ON", "FT_REQUIRE_PNG=ON"],
    "expat": ["EXPAT_SHARED_LIBS=OFF", "EXPAT_BUILD_TOOLS=OFF", "EXPAT_BUILD_EXAMPLES=OFF",
              "EXPAT_BUILD_TESTS=OFF", "EXPAT_BUILD_DOCS=OFF", "EXPAT_BUILD_PKGCONFIG=ON",
              "EXPAT_DEV_URANDOM=OFF", "EXPAT_WITH_GETRANDOM=ON", "EXPAT_WITH_SYS_GETRANDOM=OFF"],
    "libxml2": ["LIBXML2_WITH_PROGRAMS=OFF", "LIBXML2_WITH_TESTS=OFF", "LIBXML2_WITH_PYTHON=OFF",
                "LIBXML2_WITH_HTTP=OFF", "LIBXML2_WITH_ICONV=OFF", "LIBXML2_WITH_ICU=OFF",
                "LIBXML2_WITH_LZMA=OFF", "LIBXML2_WITH_MODULES=OFF", "LIBXML2_WITH_LEGACY=ON",
                "LIBXML2_WITH_TLS=OFF"],
    "libjpeg-turbo": ["ENABLE_SHARED=OFF", "ENABLE_STATIC=ON", "WITH_SIMD=OFF",
                      "WITH_TURBOJPEG=OFF", "WITH_TOOLS=OFF", "WITH_TESTS=OFF", "WITH_JAVA=OFF",
                      "CMAKE_C_FLAGS=-ffunction-sections -fdata-sections -DNO_PUTENV"],
    "libwebp": ["WEBP_ENABLE_SIMD=OFF", "WEBP_USE_THREAD=OFF", "WEBP_BUILD_ANIM_UTILS=OFF",
                "WEBP_BUILD_CWEBP=OFF", "WEBP_BUILD_DWEBP=OFF", "WEBP_BUILD_GIF2WEBP=OFF",
                "WEBP_BUILD_IMG2WEBP=OFF", "WEBP_BUILD_VWEBP=OFF", "WEBP_BUILD_WEBPINFO=OFF",
                "WEBP_BUILD_LIBWEBPMUX=OFF", "WEBP_BUILD_WEBPMUX=OFF", "WEBP_BUILD_EXTRAS=OFF",
                "WEBP_BUILD_WEBP_JS=OFF", "WEBP_BUILD_FUZZTEST=OFF"],
    "curl": ["BUILD_CURL_EXE=OFF", "BUILD_SHARED_LIBS=OFF", "BUILD_STATIC_LIBS=ON",
             "BUILD_TESTING=OFF", "BUILD_EXAMPLES=OFF", "BUILD_LIBCURL_DOCS=OFF",
             "BUILD_MISC_DOCS=OFF", "ENABLE_CURL_MANUAL=OFF", "CURL_ENABLE_SSL=OFF",
             "CURL_USE_OPENSSL=OFF", "CURL_USE_LIBPSL=OFF", "CURL_USE_LIBSSH2=OFF",
             "USE_LIBIDN2=OFF", "USE_NGHTTP2=OFF", "CURL_BROTLI=OFF", "CURL_ZSTD=OFF",
             "CURL_ZLIB=OFF", "ENABLE_ARES=OFF", "ENABLE_THREADED_RESOLVER=OFF",
             "ENABLE_UNIX_SOCKETS=OFF", "ENABLE_IPV6=OFF", "HAVE_GETADDRINFO_THREADSAFE=ON",
             "PICKY_COMPILER=OFF"] + [f"CURL_DISABLE_{name}=ON" for name in (
                 "ALTSVC", "AWS", "DICT", "DOH", "FTP", "GOPHER", "HSTS", "HTTP", "HTTP_AUTH",
                 "IMAP", "LDAP", "LDAPS", "MQTT", "NETRC", "NTLM", "POP3", "PROXY", "IPFS",
                 "RTSP", "SMB", "SMTP", "SOCKETPAIR", "TELNET", "TFTP", "WEBSOCKETS")],
}
MESON_OPTIONS = {
    "pixman": ["tests=disabled", "demos=disabled", "gtk=disabled", "libpng=disabled",
               "openmp=disabled", "a64-neon=disabled", "neon=disabled", "arm-simd=disabled",
               "mmx=disabled", "sse2=disabled", "ssse3=disabled", "vmx=disabled",
               "loongson-mmi=disabled", "mips-dspr2=disabled", "rvv=disabled", "tls=disabled"],
    "fontconfig": ["doc=disabled", "nls=disabled", "tests=disabled", "tools=disabled",
                   "cache-build=disabled", "iconv=disabled", "xml-backend=expat",
                   "default-fonts-dirs=/share/fonts", "additional-fonts-dirs=[]",
                   "cache-dir=/tmp/fontconfig", "baseconfig-dir=/etc/fonts",
                   "config-dir=/etc/fonts/conf.d", "template-dir=/share/fontconfig/conf.avail",
                   "xml-dir=/share/xml/fontconfig"],
    "cairo": ["auto_features=disabled", "fontconfig=enabled", "freetype=enabled", "png=enabled",
              "zlib=disabled", "tests=disabled", "gtk_doc=false"],
    "harfbuzz": ["auto_features=disabled", "freetype=enabled", "icu=enabled", "icu_builtin=false",
                 "tests=disabled", "utilities=disabled", "docs=disabled", "glib=disabled",
                 "gobject=disabled", "cairo=disabled", "introspection=disabled", "benchmark=disabled"],
    "libpsl": ["runtime=libicu", "tests=false", "tools=false", "docs=false"],
}
ICU_CPPFLAGS = (
    "-DU_STATIC_IMPLEMENTATION -DU_CHARSET_IS_UTF8=1 -DU_HAVE_NL_LANGINFO_CODESET=0 "
    "-DU_HAVE_TZNAME=0 -DU_HAVE_TZSET=0 -DU_HAVE_TIMEZONE=0 -DU_ENABLE_DYLOAD=0 "
    "-DU_WCHAR_IS_UTF32=1"
)
ARCHIVES = (
    "libicuuc.a", "libicui18n.a", "libicudata.a", "libz.a", "libpng16.a", "libfreetype.a",
    "libexpat.a", "libpixman-1.a", "libfontconfig.a", "libcairo.a", "libharfbuzz.a",
    "libharfbuzz-icu.a", "libxml2.a", "libsqlite3.a", "libjpeg.a", "libwebp.a",
    "libwebpdemux.a", "libcurl.a", "libpsl.a", "libssl.a", "libcrypto.a",
)


class RecipeError(Exception):
    pass


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def require_hash(value, length, label):
    if not isinstance(value, str) or not re.fullmatch(rf"[0-9a-f]{{{length}}}", value):
        raise RecipeError(f"Missing or invalid immutable {label}; publish the source pin first")


def validate_source(source, label):
    if source.get("kind") == "git":
        require_hash(source.get("commit"), 40, f"Git commit for {label}")
        url = source.get("git_url", "")
        if not url.startswith("https://github.com/") or not url.endswith(".git"):
            raise RecipeError(f"Expected an explicit GitHub HTTPS repository for {label}")
        for submodule in source.get("submodules", []):
            relative = Path(submodule["path"])
            if relative.is_absolute() or ".." in relative.parts or str(relative) == ".":
                raise RecipeError(f"Invalid submodule path for {label}")
            validate_source(dict(submodule, kind="git"), f"{label}/{relative}")
    elif source.get("kind") == "archive":
        require_hash(source.get("sha256"), 64, f"archive SHA-256 for {label}")
        if not source.get("url", "").startswith("https://"):
            raise RecipeError(f"Expected an explicit official HTTPS release for {label}")
        if Path(source.get("filename", "")).name != source.get("filename") or not source.get("filename"):
            raise RecipeError(f"Invalid archive filename for {label}")
    else:
        raise RecipeError(f"Unknown source kind for {label}")


def load_lock(path):
    lock = json.loads(path.read_text())
    if lock.get("schema_version") != 1 or lock.get("target") != "aarch64-unknown-scarlet":
        raise RecipeError("Unsupported dependency lock schema or target")
    policy = lock.get("source_policy", {})
    if policy.get("immutable_pins") is not True or policy.get("apply_local_patches") is not False or policy.get("copy_source_overlays") is not False:
        raise RecipeError("This recipe requires immutable sources without patch or overlay replay")
    if tuple(item["name"] for item in lock.get("dependencies", [])) != DEPENDENCIES:
        raise RecipeError("Lock dependency list/order differs from the recipe")
    for item in lock["dependencies"]:
        validate_source(item["source"], item["name"])
    validate_source(lock["sdk"]["llvm_source"], "SDK-owned LLVM")
    validate_source(lock["sdk"]["scarlet_source"], "SDK-owned Scarlet native runtime")
    validate_source(lock["icu_data"], "ICU data")
    require_hash(lock["icu_data"].get("payload_sha256"), 64, "ICU payload SHA-256")
    return lock


def external_path(path, label):
    result = path.expanduser().resolve()
    if result == SOURCE_ROOT or result.is_relative_to(SOURCE_ROOT):
        raise RecipeError(f"{label} must be outside the WebKit source checkout: {result}")
    if not re.fullmatch(r"[A-Za-z0-9_/.+@-]+", str(result)):
        raise RecipeError(f"Unsupported character in {label}: {result}")
    return result


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def tree_hash(root):
    value = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if ".git" in relative.parts:
            continue
        value.update(str(relative).encode() + b"\0")
        if path.is_symlink():
            value.update(b"L" + os.readlink(path).encode())
        elif path.is_file():
            value.update(b"F" + str(path.stat().st_mode & 0o777).encode() + b"\0" + digest(path).encode())
        elif path.is_dir():
            value.update(b"D")
        else:
            raise RecipeError(f"Unsupported source cache entry: {path}")
    return value.hexdigest()


def validate_native_elf(payload, label):
    if (len(payload) < 64 or payload[:7] != b"\x7fELF\x02\x01\x01" or payload[7] not in (0, 3, 83)
            or struct.unpack_from("<HHI", payload, 16) != (1, 183, 1)
            or struct.unpack_from("<H", payload, 52)[0] != 64):
        raise RecipeError(f"Archive member is not an AArch64 ELF64 relocatable: {label}")
    phoff, shoff = struct.unpack_from("<QQ", payload, 32)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", payload, 54)
    if shentsize != 64 or shoff < 64 or shoff + 64 > len(payload):
        raise RecipeError(f"Missing or truncated ELF section table: {label}")
    zero = struct.unpack_from("<IIQQQQIIQQ", payload, shoff)
    if zero[1] != 0:
        raise RecipeError(f"ELF section zero is not SHT_NULL: {label}")
    count = shnum or zero[5]
    string_index = zero[6] if shstrndx == 0xffff else shstrndx
    if count < 2 or shoff + count * 64 > len(payload) or string_index >= count:
        raise RecipeError(f"Invalid or truncated ELF section table: {label}")
    sections = []
    for index in range(count):
        section = struct.unpack_from("<IIQQQQIIQQ", payload, shoff + index * 64)
        if section[1] not in (0, 8) and section[4] + section[5] > len(payload):
            raise RecipeError(f"Truncated ELF section payload at index {index}: {label}")
        sections.append(section)
    if string_index:
        strings = sections[string_index]
        if strings[1] != 3 or any(section[0] >= strings[5] for section in sections):
            raise RecipeError(f"Invalid ELF section-name table: {label}")
    if phnum == 0xffff:
        phnum = zero[7]
    if phnum:
        if phentsize != 56 or phoff < 64 or phoff + phnum * 56 > len(payload):
            raise RecipeError(f"Invalid or truncated ELF program table: {label}")
        for index in range(phnum):
            entry = struct.unpack_from("<IIQQQQQQ", payload, phoff + index * 56)
            if entry[2] + entry[5] > len(payload):
                raise RecipeError(f"Truncated ELF program payload at index {index}: {label}")


def native_members(path):
    """Audit every real GNU/BSD ar member; reject thin archives and host objects."""
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise RecipeError(f"Expected a regular static archive: {path}")
    offset, names, count = 8, None, 0
    symbols = {"/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"}
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise RecipeError(f"Invalid archive header: {path}")
        try:
            name, size = header[:16].decode("ascii").rstrip(), int(header[48:58].decode("ascii").strip())
        except (UnicodeError, ValueError) as error:
            raise RecipeError(f"Invalid archive member: {path}") from error
        start, end = offset + 60, offset + 60 + size
        if size < 0 or end > len(data):
            raise RecipeError(f"Truncated archive: {path}")
        payload = data[start:end]
        offset = end + size % 2
        if size % 2 and data[end:end + 1] != b"\n":
            raise RecipeError(f"Invalid archive alignment: {path}")
        if name == "//":
            if names is not None:
                raise RecipeError(f"Duplicate GNU archive filename table: {path}")
            names = payload
            continue
        if name.startswith("#1/"):
            length = int(name[3:])
            if length < 0 or length > len(payload):
                raise RecipeError(f"Truncated BSD archive name: {path}")
            name, payload = payload[:length].rstrip(b"\0").decode(), payload[length:]
        elif name.startswith("/") and name[1:].isdigit():
            index = int(name[1:])
            if names is None or index >= len(names) or (index and names[index - 1:index] != b"\n"):
                raise RecipeError(f"Invalid GNU archive name: {path}")
            terminator = names.find(b"\n", index)
            if terminator < 0:
                raise RecipeError(f"Unterminated GNU archive name: {path}")
            name = names[index:terminator].removesuffix(b"/").decode()
        elif name not in symbols:
            name = name.removesuffix("/")
        if name in symbols:
            continue
        validate_native_elf(payload, f"{path}({name})")
        count += 1
    if not count:
        raise RecipeError(f"Archive contains no native objects: {path}")
    return {"sha256": digest(path), "bytes": len(data), "native_object_count": count}


class Builder:
    def __init__(self, args, lock):
        self.args, self.lock = args, lock
        self.root = external_path(args.build_dir, "Build directory")
        self.sdk = external_path(args.sdk, "Scarlet SDK")
        self.prefix = external_path(args.prefix or self.root / "prefix", "Dependency prefix")
        if self.root == self.sdk or self.root.is_relative_to(self.sdk) or self.sdk.is_relative_to(self.root):
            raise RecipeError("Build directory and SDK must not overlap")
        if self.root == self.prefix or self.root.is_relative_to(self.prefix):
            raise RecipeError("Dependency prefix must not contain the build directory")
        reserved = {"sources", "build-sources", "build", "control", "downloads", "inputs",
                    "install-stage", "logs", "source-state", "build-source-state",
                    "recipe-state.json", "dependency-build.json"}
        if self.prefix.is_relative_to(self.root) and self.prefix.relative_to(self.root).parts[0] in reserved:
            raise RecipeError("Dependency prefix overlaps a reserved build/cache directory")
        if self.prefix == self.sdk or self.prefix.is_relative_to(self.sdk) or self.sdk.is_relative_to(self.prefix):
            raise RecipeError("Dependency prefix and SDK must be separate; the SDK owns libc and the C++ runtime")
        self.cc = self.sdk / lock["sdk"]["c_driver"]
        self.cxx = self.sdk / lock["sdk"]["cxx_driver"]
        self.control = self.root / "control"
        self.sources = self.root / "sources"
        self.env = os.environ.copy()
        for key in ("CC", "CXX", "CPP", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                    "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "SDKROOT",
                    "MACOSX_DEPLOYMENT_TARGET", "NIX_CFLAGS_COMPILE", "NIX_LDFLAGS", "CMAKE_PREFIX_PATH",
                    "CMAKE_LIBRARY_PATH", "CMAKE_INCLUDE_PATH", "PKG_CONFIG_SYSROOT_DIR", "DESTDIR",
                    "SCARLET_BOOTSTRAP_CXX_HEADERS", "SCARLET_PRINT_COMMAND"):
            self.env.pop(key, None)
        self.env.update(PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=os.pathsep.join(
            str(prefix / part) for prefix in (self.prefix, self.sdk) for part in ("lib/pkgconfig", "share/pkgconfig")))
        self.tools = {}

    def run(self, command, label, cwd=None, env=None, capture=False):
        command = [str(part) for part in command]
        log = self.root / "logs" / f"{label}.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        print(f"[{label}] {shlex.join(command)}", flush=True)
        with log.open("a") as stream:
            stream.write(shlex.join(command) + "\n")
            stream.flush()
            result = subprocess.run(command, cwd=cwd, env=env or self.env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE if capture else stream,
                                    stderr=subprocess.STDOUT, text=True)
            if capture:
                stream.write(result.stdout)
        if result.returncode:
            raise RecipeError(f"{label} failed with status {result.returncode}; see {log}")
        return result.stdout if capture else None

    def tool(self, name):
        value = shutil.which(name)
        if not value:
            raise RecipeError(f"Required host build tool missing: {name}")
        return value

    def check_sdk(self):
        sdk = self.lock["sdk"]
        manifest_path = self.sdk / "share/scarlet/native-sdk.json"
        manifest = json.loads(manifest_path.read_text())
        if (manifest.get("schema_version") != 1 or manifest.get("target") != self.lock["target"]
                or manifest.get("state") != "built-and-native-link-audited"):
            raise RecipeError("SDK manifest must record a completed native link audit for the Scarlet target")
        llvm = manifest.get("sources", {}).get("llvm", {})
        pin = sdk["llvm_source"]
        if llvm.get("git") != pin["git_url"] or llvm.get("rev") != pin["commit"]:
            raise RecipeError("SDK-owned LLVM source differs from the published dependency lock pin")
        scarlet = manifest.get("sources", {}).get("scarlet", {})
        if scarlet.get("rev") != sdk["scarlet_source"]["commit"]:
            raise RecipeError("SDK-owned Scarlet native source differs from the published dependency lock pin")
        if "development_diff_sha256" in scarlet or "development_source_sha256" in scarlet:
            raise RecipeError("SDK manifest records development source changes; use the clean published Scarlet source")
        profile = manifest.get("runtime_profile", {})
        for name, expected in (("LIBCXX_ENABLE_FILESYSTEM", "ON"), ("LIBCXX_ENABLE_SHARED", "OFF"),
                               ("LIBCXXABI_ENABLE_SHARED", "OFF"), ("LIBCXX_ENABLE_EXCEPTIONS", "OFF")):
            if profile.get(name) != expected:
                raise RecipeError(f"SDK native runtime profile requires {name}={expected}")
        required = [sdk["toolchain"], sdk["c_driver"], sdk["cxx_driver"], *sdk["owned_archives"],
                    "include/c++/v1/filesystem", "lib/cmake/ScarletCXXSupport/ScarletCXXSupportConfig.cmake"]
        for relative in required:
            if not (self.sdk / relative).is_file():
                raise RecipeError(f"Required SDK input missing: {self.sdk / relative}")
        for driver in (self.cc, self.cxx):
            if not os.access(driver, os.X_OK):
                raise RecipeError(f"SDK driver is not executable: {driver}")
        for name in ("git", "cmake", "ninja", "meson", "make", "perl", "gperf", "pkg-config"):
            self.tools[name] = self.tool(name)
        self.tools["ar"] = self.tool(self.args.ar or manifest["tools"]["llvm-ar"])
        self.tools["ranlib"] = self.tool(self.args.ranlib or manifest["tools"]["llvm-ranlib"])
        self.tools["strip"] = self.tool(self.args.strip or str(Path(self.tools["ar"]).with_name("llvm-strip")))
        macros = self.run([self.cc, "-dM", "-E", "-x", "c", "-"], "sdk-macros", capture=True)
        if not re.search(r"^#define __scarlet__\b", macros, re.M) or not re.search(r"^#define __aarch64__\b", macros, re.M):
            raise RecipeError("SDK driver did not select the real Scarlet AArch64 target")
        if re.search(r"^#define (__linux__|__APPLE__|_WIN32)\b", macros, re.M):
            raise RecipeError("Host/Linux target macros detected in the native SDK")
        probe = self.control / "sdk-cxx-probe.cpp"
        write(probe, "#include <filesystem>\n#include <charconv>\n#include <complex>\n"
              "#ifndef __scarlet__\n#error Scarlet required\n#endif\n"
              "static_assert(sizeof(std::complex<float>) == 2 * sizeof(float));\n"
              "static_assert(sizeof(std::complex<double>) == 2 * sizeof(double));\n"
              "void checkNativeComplexHeaders() {\n"
              "    std::complex<float> f { 1.0f, 2.0f };\n"
              "    std::complex<double> d { 3.0, 4.0 };\n"
              "    auto product = f * f;\n"
              "    auto sum = d + d;\n"
              "    (void)product; (void)sum;\n}\n")
        self.run([self.cxx, "-std=c++23", "-fsyntax-only", probe], "sdk-cxx-headers")
        c_probe = self.control / "sdk-c11-probe.c"
        write(c_probe, "#include <uchar.h>\n"
              "_Static_assert(sizeof(char16_t) == 2, \"native char16_t width\");\n"
              "_Static_assert(sizeof(char32_t) == 4, \"native char32_t width\");\n")
        self.run([self.cc, "-std=c11", "-fsyntax-only", c_probe], "sdk-c11-headers")
        for relative in sdk["owned_archives"]:
            expected = manifest.get("libraries", {}).get(relative, {}).get("sha256")
            require_hash(expected, 64, f"SDK archive manifest hash for {relative}")
            if digest(self.sdk / relative) != expected:
                raise RecipeError(f"SDK archive bytes differ from its completed manifest: {relative}")
            native_members(self.sdk / relative)

    def prepare(self):
        self.root.mkdir(parents=True, exist_ok=True)
        state = {"lock_sha256": digest(self.args.lock), "recipe_sha256": digest(Path(__file__)),
                 "sdk": str(self.sdk), "prefix": str(self.prefix), "profile": self.lock["profile"]}
        state_path = self.root / "recipe-state.json"
        if state_path.exists() and json.loads(state_path.read_text()) != state:
            raise RecipeError("Build directory belongs to a different recipe, lock, SDK or prefix; use a fresh build directory")
        self.check_sdk()
        write(state_path, json.dumps(state, indent=2) + "\n")
        self.prefix.mkdir(parents=True, exist_ok=True)
        toolchain = f'''include({self.cmake_quote(self.sdk / self.lock['sdk']['toolchain'])})
list(PREPEND CMAKE_FIND_ROOT_PATH {self.cmake_quote(self.prefix)})
list(PREPEND CMAKE_PREFIX_PATH {self.cmake_quote(self.prefix)})
set(CMAKE_INSTALL_PREFIX {self.cmake_quote(self.prefix)} CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_AR {self.cmake_quote(self.tools['ar'])} CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB {self.cmake_quote(self.tools['ranlib'])} CACHE FILEPATH "" FORCE)
set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")
'''
        write(self.control / "native.cmake", toolchain)
        # zlib's release CMake declares and installs a shared target even when
        # BUILD_SHARED_LIBS=OFF. This dependency profile supports real static
        # archives only; keep CMake's supported static conversion for that
        # upstream target, without editing its source or enabling a loader.
        write(self.control / "static-only.cmake", STATIC_CMAKE_PROFILE)
        cross = f'''[binaries]
c = {str(self.cc)!r}
cpp = {str(self.cxx)!r}
ar = {self.tools['ar']!r}
strip = {self.tools['strip']!r}
pkg-config = {self.tools['pkg-config']!r}

[host_machine]
system = 'scarlet'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = {[str(prefix / part) for prefix in (self.prefix, self.sdk) for part in ('lib/pkgconfig', 'share/pkgconfig')]!r}

[built-in options]
default_library = 'static'
buildtype = 'release'
c_args = ['-ffunction-sections', '-fdata-sections']
cpp_args = ['-ffunction-sections', '-fdata-sections']
c_link_args = {[f'-L{self.prefix / "lib"}', f'-L{self.sdk / "lib"}']!r}
cpp_link_args = {[f'-L{self.prefix / "lib"}', f'-L{self.sdk / "lib"}']!r}
pkg_config_path = []
'''
        write(self.control / "native.ini", cross)

    @staticmethod
    def cmake_quote(path):
        value = str(path)
        equals = "="
        while "]" + equals + "]" in value:
            equals += "="
        return "[" + equals + "[" + value + "]" + equals + "]"

    def download(self, source):
        path = self.root / "downloads" / source["filename"]
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            temporary = path.with_name(path.name + ".part")
            request = urllib.request.Request(source["url"], headers={"User-Agent": "Scarlet-native-dependencies/1"})
            with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
                if not response.url.startswith("https://"):
                    raise RecipeError("Archive download redirected away from HTTPS")
                shutil.copyfileobj(response, output)
            if digest(temporary) != source["sha256"]:
                temporary.unlink()
                raise RecipeError(f"Official archive SHA-256 mismatch: {source['filename']}")
            temporary.replace(path)
        if digest(path) != source["sha256"]:
            raise RecipeError(f"Cached archive SHA-256 mismatch: {path}")
        return path

    def fetch_git(self, path, source, label):
        if not path.exists():
            path.mkdir(parents=True)
            self.run(["git", "init", path], f"fetch-{label}")
            self.run(["git", "-C", path, "remote", "add", "origin", source["git_url"]], f"fetch-{label}")
            self.run(["git", "-C", path, "fetch", "--depth=1", "origin", source["commit"]], f"fetch-{label}")
            self.run(["git", "-C", path, "checkout", "--detach", source["commit"]], f"fetch-{label}")
        head = self.run(["git", "-C", path, "rev-parse", "HEAD"], f"check-{label}", capture=True).strip()
        origin = self.run(["git", "-C", path, "remote", "get-url", "origin"], f"check-{label}", capture=True).strip()
        if head != source["commit"] or origin != source["git_url"]:
            raise RecipeError(f"Cached Git source has a different immutable pin or remote: {path}")
        for module in source.get("submodules", []):
            gitlink = self.run(["git", "-C", path, "ls-tree", "HEAD", "--", module["path"]], f"check-{label}", capture=True)
            if gitlink.split()[:3] != ["160000", "commit", module["commit"]]:
                raise RecipeError(f"Gitlink differs from the pinned submodule: {label}/{module['path']}")
            module_path = path / module["path"]
            if module_path.is_dir() and not any(module_path.iterdir()):
                module_path.rmdir()
            self.fetch_git(module_path, module, f"{label}-{module['path'].replace('/', '-')}")
        dirty = self.run(["git", "-C", path, "status", "--porcelain", "--untracked-files=all"], f"check-{label}", capture=True)
        if dirty:
            raise RecipeError(f"Git source cache was modified; use a fresh build directory: {path}\n{dirty}")
        for relative, expected in source.get("changed_source_sha256", {}).items():
            if digest(path / relative) != expected:
                raise RecipeError(f"Published source byte hash differs: {label}/{relative}")
        if source.get("public_suffix_list_sha256") and digest(path / "public_suffix_list.dat") != source["public_suffix_list_sha256"]:
            raise RecipeError("Pinned public suffix data differs from the verified release payload")

    def fetch(self, item):
        path, source = self.sources / item["name"], item["source"]
        if source["kind"] == "git":
            self.fetch_git(path, source, item["name"])
        else:
            archive = self.download(source)
            state = self.root / "source-state" / f"{item['name']}.json"
            if not path.exists():
                temporary = path.with_name(path.name + ".extracting")
                if temporary.exists():
                    raise RecipeError(f"Partial source extraction found; inspect/remove before retry: {temporary}")
                temporary.mkdir(parents=True)
                with tarfile.open(archive) as stream:
                    roots = {Path(member.name).parts[0] for member in stream if Path(member.name).parts}
                    if len(roots) != 1:
                        raise RecipeError(f"Release archive must have one source root: {archive}")
                    stream.extractall(temporary, filter="data")
                (temporary / next(iter(roots))).replace(path)
                temporary.rmdir()
                write(state, json.dumps({"archive_sha256": source["sha256"], "tree_sha256": tree_hash(path)}, indent=2) + "\n")
            if not state.is_file() or json.loads(state.read_text()) != {"archive_sha256": source["sha256"], "tree_sha256": tree_hash(path)}:
                raise RecipeError(f"Extracted source cache was modified or lacks provenance: {path}")
        return path

    def work_source(self, name, source):
        # Some genuine upstream builds (notably zlib CMake) rename source
        # files even in an out-of-source build. Keep fetched source immutable;
        # compile from a separate complete copy, never from a source overlay.
        work = self.root / "build-sources" / name
        state = self.root / "build-source-state" / f"{name}.json"
        original = tree_hash(source)
        if not work.exists():
            shutil.copytree(source, work, symlinks=True)
            write(state, json.dumps({"original_tree_sha256": original,
                                    "work_tree_sha256": tree_hash(work)}, indent=2) + "\n")
        if not state.is_file():
            raise RecipeError(f"Build source copy lacks a provenance checkpoint: {work}")
        checkpoint = json.loads(state.read_text())
        if checkpoint != {"original_tree_sha256": original, "work_tree_sha256": tree_hash(work)}:
            raise RecipeError(f"Build source copy was modified outside the recorded upstream build: {work}")
        return work, state, original

    def cmake(self, name, source):
        build = self.root / "build" / name
        self.run(["cmake", "-S", source, "-B", build, "-G", "Ninja",
                  f"-DCMAKE_TOOLCHAIN_FILE={self.control / 'native.cmake'}", "-DCMAKE_BUILD_TYPE=Release",
                  f"-DCMAKE_PROJECT_INCLUDE={self.control / 'static-only.cmake'}", "-DCMAKE_POLICY_DEFAULT_CMP0164=OLD",
                  f"-DCMAKE_INSTALL_PREFIX={self.prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIBS=OFF",
                  *[f"-D{option}" for option in CM_OPTIONS.get(name, [])]], f"configure-{name}")
        self.run(["cmake", "--build", build, "--parallel", self.args.jobs], f"build-{name}")
        self.run(["cmake", "--install", build], f"install-{name}")
        aliases = {"zlib": ("libz.a", ("libz.a", "libzlibstatic.a")),
                   "libpng": ("libpng16.a", ("libpng16.a", "liblibpng16_static.a"))}
        if name in aliases:
            canonical, alternatives = aliases[name]
            target = next((self.prefix / "lib" / value for value in alternatives if (self.prefix / "lib" / value).is_file()), None)
            if target is None:
                raise RecipeError(f"Installed real {name} archive not found")
            alias = self.prefix / "lib" / canonical
            if alias != target:
                if alias.exists() or alias.is_symlink():
                    raise RecipeError(f"Refusing to replace archive alias: {alias}")
                alias.symlink_to(target.name)

    def meson(self, name, source):
        build = self.root / "build" / name
        setup = ["meson", "setup", build, source, "--cross-file", self.control / "native.ini",
                 "--prefix", self.prefix, "--libdir", "lib", "--buildtype", "release",
                 "--default-library", "static", "--wrap-mode", "nofallback",
                 *[f"-D{option}" for option in MESON_OPTIONS[name]]]
        if (build / "meson-private/coredata.dat").is_file():
            setup.append("--reconfigure")
        self.run(setup, f"configure-{name}")
        self.run(["meson", "compile", "-C", build, "-j", self.args.jobs], f"build-{name}")
        stage = self.root / "install-stage" / name
        install_env = dict(self.env, DESTDIR=str(stage))
        self.run(["meson", "install", "-C", build, "--no-rebuild"], f"install-{name}", env=install_env)
        installed_prefix = stage / str(self.prefix).lstrip("/")
        if installed_prefix.is_dir():
            shutil.copytree(installed_prefix, self.prefix, dirs_exist_ok=True)
        # Fontconfig guest absolute /etc and /share paths never write to the host.
        for directory in ("etc", "share"):
            absolute = stage / directory
            if absolute.is_dir():
                shutil.copytree(absolute, self.prefix / directory, dirs_exist_ok=True)

    def icu(self, source):
        source = source / "icu4c/source"
        host, build = self.root / "build/icu-host", self.root / "build/icu"
        host.mkdir(parents=True, exist_ok=True)
        build.mkdir(parents=True, exist_ok=True)
        (build / "lib").mkdir(parents=True, exist_ok=True)
        host_platform = {"Linux": "Linux", "Darwin": "MacOSX"}.get(platform.system())
        if host_platform is None:
            raise RecipeError("ICU cross metadata recipe supports Linux and macOS hosts")
        host_env = dict(self.env, CC=self.args.host_cc, CXX=self.args.host_cxx)
        self.run([source / "runConfigureICU", host_platform, "--disable-shared", "--enable-static",
                  "--disable-tests", "--disable-samples"], "configure-icu-host", cwd=host, env=host_env)
        self.run(["make", "config/icucross.mk", "config/icucross.inc"], "metadata-icu-host", cwd=host, env=host_env)
        env = dict(self.env, CC=str(self.cc), CXX=str(self.cxx), AR=self.tools["ar"], RANLIB=self.tools["ranlib"],
                   CPPFLAGS=ICU_CPPFLAGS, CFLAGS="-O2 -ffunction-sections -fdata-sections",
                   CXXFLAGS="-O2 -fno-exceptions -ffunction-sections -fdata-sections")
        self.run([source / "configure", "--host=aarch64-unknown-scarlet", f"--with-cross-build={host}",
                  f"--prefix={self.prefix}", "--enable-static", "--disable-shared", "--with-data-packaging=static",
                  "--disable-tests", "--disable-samples", "--disable-extras", "--disable-tools",
                  "--disable-icuio", "--disable-dyload"], "configure-icu", cwd=build, env=env)
        for part in ("common", "i18n"):
            self.run(["make", "-C", build / part, f"-j{self.args.jobs}"], f"build-icu-{part}", env=env)
        spec = self.lock["icu_data"]
        archive = self.download(spec)
        payload = self.root / "inputs" / "icudt78l.dat"
        payload.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as stream:
            member = stream.getmember(spec["member"])
            if not member.isfile():
                raise RecipeError("Pinned ICU payload member is not a regular file")
            payload.write_bytes(stream.extractfile(member).read())
        if digest(payload) != spec["payload_sha256"]:
            raise RecipeError("Exact official ICU data payload SHA-256 mismatch")
        assembly = self.control / "icu-data.S"
        write(assembly, f'.section .rodata,"a",%progbits\n.balign 16\n.globl {spec["symbol"]}\n'
              f'.type {spec["symbol"]},%object\n{spec["symbol"]}:\n.incbin {json.dumps(str(payload))}\n'
              f'.size {spec["symbol"]}, .-{spec["symbol"]}\n.section .note.GNU-stack,"",%progbits\n')
        data_object = build / "icu-data.o"
        self.run([self.cc, "-c", assembly, "-o", data_object], "build-icu-data")
        (self.prefix / "lib").mkdir(parents=True, exist_ok=True)
        self.run([self.tools["ar"], "rcs", self.prefix / "lib/libicudata.a", data_object], "archive-icu-data")
        for library in ("libicuuc.a", "libicui18n.a"):
            shutil.copy2(build / "lib" / library, self.prefix / "lib" / library)
        for part in ("common", "i18n"):
            shutil.copytree(source / part / "unicode", self.prefix / "include/unicode", dirs_exist_ok=True)
        version = next(item["version"] for item in self.lock["dependencies"] if item["name"] == "icu")
        common = f"prefix={self.prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
        for name, libraries, requires in (("icu-uc", "-licuuc -licudata", ""),
                                          ("icu-i18n", "-licui18n", f"Requires: icu-uc = {version}\n")):
            write(self.prefix / "lib/pkgconfig" / f"{name}.pc", common + f"Name: {name}\n"
                  f"Description: Scarlet native ICU\nVersion: {version}\n{requires}"
                  f"Libs: -L${{libdir}} {libraries}\nCflags: -I${{includedir}} -DU_STATIC_IMPLEMENTATION\n")

    def sqlite(self, source):
        adapter = self.control / "sqlite"
        write(adapter / "CMakeLists.txt", f'''cmake_minimum_required(VERSION 3.24)
project(ScarletSQLite VERSION 3.50.4 LANGUAGES C)
add_library(sqlite3 STATIC {self.cmake_quote(source / 'sqlite3.c')})
target_include_directories(sqlite3 PUBLIC {self.cmake_quote(source)})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OS_UNIX=1 SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_MAX_MMAP_SIZE=0 SQLITE_TEMP_STORE=3
    HAVE_LOCALTIME_R=1 HAVE_GMTIME_R=1 HAVE_PREAD=1 HAVE_PWRITE=1)
install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES {self.cmake_quote(source / 'sqlite3.h')} {self.cmake_quote(source / 'sqlite3ext.h')} DESTINATION include)
''')
        self.cmake("sqlite", adapter)
        write(self.prefix / "lib/pkgconfig/sqlite3.pc", f"prefix={self.prefix}\nlibdir=${{prefix}}/lib\n"
              "includedir=${prefix}/include\nName: SQLite\nDescription: Native Scarlet SQLite\nVersion: 3.50.4\n"
              "Libs: -L${libdir} -lsqlite3\nCflags: -I${includedir}\n")

    def openssl(self, source):
        build = self.root / "build/openssl"
        build.mkdir(parents=True, exist_ok=True)
        config = self.control / "openssl-scarlet.conf"
        write(config, '''my %targets = (
    "scarlet-aarch64" => {
        inherit_from => [ "BASE_unix" ],
        CC => "clang",
        CFLAGS => "-O2 -ffunction-sections -fdata-sections",
        lib_cppflags => "-DL_ENDIAN -DNO_SYSLOG",
        bn_ops => "SIXTY_FOUR_BIT_LONG",
        sys_id => "SCARLET",
        thread_scheme => "pthreads",
    },
);
''')
        env = dict(self.env, CC=str(self.cc), AR=self.tools["ar"], RANLIB=self.tools["ranlib"])
        self.run(["perl", source / "Configure", f"--config={config}", "scarlet-aarch64",
                  f"--prefix={self.prefix}", "--libdir=lib", "no-shared", "no-module", "no-dso", "no-asm",
                  "no-tests", "no-apps", "no-async", "no-thread-pool", "no-quic", "no-ui-console",
                  "no-sock", "no-dgram", "no-posix-io", "no-stdio", "no-pie", "--with-rand-seed=getrandom"],
                 "configure-openssl", cwd=build, env=env)
        self.run(["make", f"-j{self.args.jobs}", "build_libs"], "build-openssl", cwd=build, env=env)
        self.run(["make", f"-j{self.args.jobs}", "install_dev"], "install-openssl", cwd=build, env=env)

    def build(self):
        self.prepare()
        if self.args.preflight_only:
            print("Native SDK source/header/archive preflight passed; no dependency source fetched or compiled.")
            return
        sources = {item["name"]: self.fetch(item) for item in self.lock["dependencies"]}
        if self.args.fetch_only:
            self.download(self.lock["icu_data"])
            print("All immutable dependency sources fetched and verified; no dependency compiled.")
            return
        for name in BUILD_ORDER:
            work, state, original = self.work_source(name, sources[name])
            try:
                if name in CM_OPTIONS:
                    self.cmake(name, work)
                elif name in MESON_OPTIONS:
                    self.meson(name, work)
                else:
                    getattr(self, name)(work)
            finally:
                # Checkpoint only changes made while executing the pinned
                # upstream build. Later manual edits are rejected on reuse.
                write(state, json.dumps({"original_tree_sha256": original,
                                        "work_tree_sha256": tree_hash(work)}, indent=2) + "\n")
        audit = {name: native_members(self.prefix / "lib" / name) for name in ARCHIVES}
        write(self.root / "dependency-build.json", json.dumps({"schema_version": 1,
              "target": self.lock["target"], "profile": self.lock["profile"],
              "lock_sha256": digest(self.args.lock), "sdk": str(self.sdk), "prefix": str(self.prefix),
              "archives": audit, "guest_execution_verified": False}, indent=2) + "\n")
        print(f"Built and audited native static dependencies in {self.prefix}; guest behavior requires separate validation.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=HERE / "dependencies.lock.json")
    parser.add_argument("--sdk", type=Path, help="Installed Scarlet native SDK prefix")
    parser.add_argument("--build-dir", type=Path, help="Caller-owned source cache, build and logs directory outside WebKit")
    parser.add_argument("--prefix", type=Path, help="Native dependency install prefix (default: BUILD_DIR/prefix)")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--ar", help="Override SDK manifest llvm-ar")
    parser.add_argument("--ranlib", help="Override SDK manifest llvm-ranlib")
    parser.add_argument("--strip", help="Override llvm-strip adjacent to SDK manifest llvm-ar")
    parser.add_argument("--host-cc", default="cc", help="Host compiler used only for ICU cross metadata configuration")
    parser.add_argument("--host-cxx", default="c++", help="Host compiler used only for ICU cross metadata configuration")
    parser.add_argument("--check", action="store_true", help="Validate the immutable source lock without fetching or writing")
    parser.add_argument("--plan", action="store_true", help="Print pins and native build options without fetching or writing")
    parser.add_argument("--fetch-only", action="store_true", help="Fetch pinned sources into BUILD_DIR; do not compile dependencies")
    parser.add_argument("--preflight-only", action="store_true", help="Check the actual native SDK header/archive contract before fetching sources")
    args = parser.parse_args()
    try:
        if args.jobs < 1 or args.jobs > 64:
            raise RecipeError("--jobs must be between 1 and 64")
        lock = load_lock(args.lock)
        if args.check:
            print(f"Validated {len(DEPENDENCIES)} immutable dependency pins and SDK-owned Scarlet/LLVM provenance.")
            return 0
        if args.plan:
            for item in lock["dependencies"]:
                source = item["source"]
                print(f"{item['name']} {item['version']}: {source.get('commit', source.get('sha256'))}")
                print("  " + " ".join(CM_OPTIONS.get(item["name"], MESON_OPTIONS.get(item["name"], [f"{item['name']} recipe in build.py"]))))
            print("ICU binary data:", lock["icu_data"]["payload_sha256"])
            print("SDK-owned LLVM:", lock["sdk"]["llvm_source"]["commit"])
            print("SDK-owned Scarlet:", lock["sdk"]["scarlet_source"]["commit"])
            print("Native build order:", " -> ".join(BUILD_ORDER))
            return 0
        if args.sdk is None or args.build_dir is None:
            parser.error("--sdk and --build-dir are required when building/fetching")
        Builder(args, lock).build()
        return 0
    except (RecipeError, OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(f"Scarlet dependency recipe failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

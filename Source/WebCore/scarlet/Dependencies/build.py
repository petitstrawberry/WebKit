#!/usr/bin/env python3
"""Compile supplied dependency sources; CMake owns retrieval and build outputs."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess
import tarfile


def write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)

DEPENDENCIES = (
    "icu", "zlib", "libpng", "freetype", "expat", "pixman", "fontconfig",
    "cairo", "harfbuzz", "libxml2", "sqlite", "libjpeg-turbo", "libwebp",
    "curl", "libpsl", "openssl",
)

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
    return {"bytes": len(data), "native_object_count": count}

class Builder:
    def __init__(self, args, lock):
        self.args, self.lock = args, lock
        self.root = args.build_dir.resolve()
        self.sdk = args.sdk.resolve()
        self.prefix = args.prefix.resolve()
        self.control = self.root / "control"
        self.cc = self.control / "cc"
        self.cxx = self.control / "cxx"
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith(("NIX_CFLAGS", "NIX_LDFLAGS", "NIX_PKG_CONFIG", "PKG_CONFIG_")) or key in (
                "CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "DESTDIR"):
                self.env.pop(key)
        self.env.update(PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=os.pathsep.join(
            str(p / d) for p in (self.prefix, self.sdk) for d in ("lib/pkgconfig", "share/pkgconfig")))
        self.tools = {name: self.tool(name) for name in ("cmake", "ninja", "meson", "make", "perl", "gperf", "pkg-config")}
        self.tools.update(ar=args.ar, ranlib=args.ranlib, strip=args.strip)

    def prepare(self):
        self.root.mkdir(parents=True, exist_ok=True)
        self.prefix.mkdir(parents=True, exist_ok=True)
        resource = subprocess.check_output([self.args.cc, "-print-resource-dir"], text=True).strip()
        flags = ["--target=aarch64-unknown-none-elf", "-nostdinc", "-isystem", str(self.sdk / "include"),
                 "-idirafter", resource + "/include", "-D__scarlet__=1", "-fno-stack-protector",
                 "-mno-outline-atomics", "-ffunction-sections", "-fdata-sections"]
        for path, compiler, extra in (
            (self.cc, self.args.cc, []),
            (self.cxx, self.args.cxx, ["-nostdinc++", "-isystem", str(self.sdk / "include/c++/v1"), "-fno-exceptions", "-fno-rtti"]),
        ):
            write(path, "#!/bin/sh\nexec " + shlex.join([compiler, *extra, *flags]) + ' "$@"\n')
            path.chmod(0o755)
        toolchain = f'''include("{self.sdk}/share/scarlet/cmake/aarch64-scarlet.cmake")
set(CMAKE_C_COMPILER "{self.cc}")
set(CMAKE_CXX_COMPILER "{self.cxx}")
set(CMAKE_AR "{self.tools['ar']}")
set(CMAKE_RANLIB "{self.tools['ranlib']}")
# Some upstream projects require real executable links for configuration.
# Use the target LLD and installed SDK archives; never the host ld/libc.
set(CMAKE_EXE_LINKER_FLAGS "-nostdlib -fuse-ld=lld -Wl,--start-group {self.sdk}/lib/libscarlet_cxx_support.a {self.sdk}/lib/libscarlet_c.a -Wl,--end-group" CACHE STRING "Native cross-link flags" FORCE)
set(CMAKE_FIND_ROOT_PATH "{self.prefix};{self.sdk}")
set(CMAKE_PREFIX_PATH "{self.prefix};{self.sdk}")
'''
        write(self.control / "native.cmake", toolchain)
        write(self.control / "static-only.cmake", STATIC_CMAKE_PROFILE)
        archives = [str(self.sdk / "lib" / n) for n in ("libscarlet_cxx_support.a", "libscarlet_c_startup.a", "libscarlet_c.a")]
        links = ["--target=aarch64-unknown-none-elf", "-nostdlib", "-fuse-ld=lld", *archives]
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
[built-in options]
c_link_args = {links!r}
cpp_link_args = {links!r}
pkg_config_path = {[str(p / 'lib/pkgconfig') for p in (self.prefix, self.sdk)]!r}
'''
        write(self.control / "native.ini", cross)

    def build(self):
        self.prepare()
        source_map = json.loads(self.args.source_map.read_text())
        for name in BUILD_ORDER:
            source = Path(source_map[name])
            work = self.root / "sources" / name
            if source.is_dir():
                shutil.copytree(source, work)
            else:
                work.mkdir(parents=True)
                with tarfile.open(source) as stream:
                    stream.extractall(work, filter="data")
                entries = list(work.iterdir())
                if len(entries) == 1 and entries[0].is_dir():
                    work = entries[0]
            if name in CM_OPTIONS:
                self.cmake(name, work)
            elif name in MESON_OPTIONS:
                self.meson(name, work)
            else:
                getattr(self, name)(work)
        for name in ARCHIVES:
            report = native_members(self.prefix / "lib" / name)
            print(f"{name}: {report['native_object_count']} AArch64 objects", flush=True)

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
            print("\n".join(log.read_text().splitlines()[-80:]), flush=True)
            if cwd and (Path(cwd) / "config.log").is_file():
                print("\n".join((Path(cwd) / "config.log").read_text().splitlines()[-160:]), flush=True)
            raise RecipeError(f"{label} failed with status {result.returncode}; see {log}")
        return result.stdout if capture else None

    def tool(self, name):
        value = shutil.which(name)
        if not value:
            raise RecipeError(f"Required host build tool missing: {name}")
        return value

    @staticmethod
    def cmake_quote(path):
        value = str(path)
        equals = "="
        while "]" + equals + "]" in value:
            equals += "="
        return "[" + equals + "[" + value + "]" + equals + "]"

    def cmake(self, name, source):
        build = self.root / "build" / name
        self.run(["cmake", "--fresh", "-S", source, "-B", build, "-G", "Ninja",
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
                   CXXFLAGS="-O2 -frtti -fno-exceptions -ffunction-sections -fdata-sections")
        env["LDFLAGS"] = "-nostdlib -fuse-ld=lld -Wl,--start-group " + " ".join(
            str(self.sdk / "lib" / n) for n in ("libc++.a", "libc++abi.a", "libscarlet_cxx_support.a", "libscarlet_c.a")
        ) + " -Wl,--end-group"
        self.run([source / "configure", "--host=aarch64-unknown-scarlet", f"--with-cross-build={host}",
                  f"--prefix={self.prefix}", "--enable-static", "--disable-shared", "--with-data-packaging=static",
                  "--disable-tests", "--disable-samples", "--disable-extras", "--disable-tools",
                  "--disable-icuio", "--disable-dyload"], "configure-icu", cwd=build, env=env)
        for part in ("common", "i18n"):
            self.run(["make", "-C", build / part, f"-j{self.args.jobs}"], f"build-icu-{part}", env=env)
        spec = self.lock["icu_data"]
        archive = self.args.icu_data
        payload = self.root / "inputs" / "icudt78l.dat"
        payload.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as stream:
            member = stream.getmember(spec["member"])
            if not member.isfile():
                raise RecipeError("Pinned ICU payload member is not a regular file")
            payload.write_bytes(stream.extractfile(member).read())
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
        if (source / "src/os_unix.c").exists():
            host = self.root / "build/sqlite-host"
            host.mkdir(parents=True, exist_ok=True)
            host_env = dict(self.env, CC=self.args.host_cc, CXX=self.args.host_cxx)
            self.run([source / "configure", "--disable-shared", "--disable-readline"],
                     "configure-sqlite-host", cwd=host, env=host_env)
            self.run(["make", "sqlite3.c"], "generate-sqlite-amalgamation", cwd=host, env=host_env)
            source = host
        adapter = self.control / "sqlite"
        write(adapter / "CMakeLists.txt", f'''cmake_minimum_required(VERSION 3.24)
    project(ScarletSQLite VERSION 3.50.4 LANGUAGES C)
    add_library(sqlite3 STATIC {self.cmake_quote(source / 'sqlite3.c')})
    target_include_directories(sqlite3 PUBLIC {self.cmake_quote(source)})
    target_compile_definitions(sqlite3 PRIVATE SQLITE_OS_UNIX=1 SQLITE_THREADSAFE=1
        SQLITE_ENABLE_LOCKING_STYLE=1
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

def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "build-dir", "prefix", "source-map", "icu-data"):
        p.add_argument("--" + name, required=True, type=Path)
    for name in ("cc", "cxx", "host-cc", "host-cxx", "ar", "ranlib", "strip"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--jobs", type=int, default=4)
    args = p.parse_args()
    lock = json.loads(Path(__file__).with_name("dependencies.lock.json").read_text())
    Builder(args, lock).build()


if __name__ == "__main__":
    main()

# Scarlet native dependencies

`build.py` builds the static CPU dependency profile used by the Scarlet WebCore
port. `dependencies.lock.json` is the source authority: changed dependencies
come directly from published Git commits, unchanged dependencies come from
official release archives with fixed SHA-256 hashes. Source patches and source
overlay replay are not part of this build.

The published Scarlet Git sources are:

| Source | Fixed commit | Use |
| --- | --- | --- |
| [Scarlet](https://github.com/petitstrawberry/Scarlet/tree/feature/webkit-native) | `2cb9d3142ffd5d276990f39dc6cce4a939d479b2` | SDK-owned libc, native support and shared-memory ABI |
| [LLVM](https://github.com/petitstrawberry/llvm-project/tree/scarlet) | `eeab068866d3156b27c937a6979e506daa736e93` | SDK-owned libc++ and libc++abi, LLVM libc utilities |
| [ICU](https://github.com/petitstrawberry/icu/tree/scarlet) | `49059ce7a1a1c72009c2ebae8f236b0de82100f7` | ICU 78.3 native common/i18n |
| [Fontconfig](https://github.com/petitstrawberry/fontconfig/tree/scarlet) | `55ba876e38312707b4557ae9326d71941c9c8443` | Fontconfig 2.16.0 native entropy |
| [Curl](https://github.com/petitstrawberry/curl/tree/scarlet) | `bbc8df7c5117ce9fca1674b7de7cdf1ba40b6ff9` | Curl 8.16.0 file-only profile |
| [libpsl](https://github.com/petitstrawberry/libpsl/tree/feature/optional-tools) | `bc0bf715e47e8786c646f0f4d6e6e15266b72805` | libpsl 0.21.5 without CLI tools |

The libpsl `list` submodule is also fixed to
`5db9b65997e3c9230ac4353b01994c2ae9667cb9`; its Gitlink is checked before use.
The remaining release versions and archive hashes are recorded in the lock.
Some release hashes have independent upstream digest checks recorded there;
others fix the bytes retrieved from the official HTTPS release location. The
recipe does not claim independent signature verification for those hashes.

## Build inputs

Use Python 3.12 or later on Linux or macOS, plus Git, CMake, Ninja, Meson, GNU
Make, Perl, gperf and pkg-config. A host C/C++ compiler is required solely for
ICU cross-build metadata configuration. No host ICU library, executable or
rendering result supplies a target dependency or a native validation result.

Provide an installed native Scarlet SDK. Its contract includes:

- `share/scarlet/cmake/aarch64-scarlet.cmake` and
  `share/scarlet/native-sdk.json`;
- `bin/aarch64-scarlet-clang` and `bin/aarch64-scarlet-clang++`;
- Scarlet libc, libc++ with filesystem, libc++abi, CXXSupport and CStartup;
- `lib/libscarlet_webcore_native_memory.a`, installed from Scarlet's native
  shared-memory component by the SDK's `--with-webcore-memory` build option.

The SDK owns the C/C++ standard libraries, CRT, native support and shared-memory
ABI. This recipe does not rebuild or copy them into the dependency prefix. The
SDK manifest must record a completed native link audit, the fixed Scarlet and LLVM commits,
the static filesystem-capable runtime profile and matching archive hashes.
Missing pins, missing SDK inputs and changed SDK archive bytes fail explicitly.
The SDK source record must match the clean published Scarlet revision and must
not carry development source changes. SDK builder/toolchain provenance remains
recorded in its manifest; the dependency lock does not replace the SDK's build
authority.

The native drivers must select AArch64 and `__scarlet__`, exclude host/Linux
target macros and provide native header/link isolation. LLVM archive tools are
taken from the SDK manifest; `--ar`, `--ranlib` and `--strip` allow explicit tool
locations when required by the host environment. Native pkg-config searches
only the new dependency prefix and the SDK. CMake library/include/package
searches and Meson cross configuration use those same native roots.

## Commands

From a fresh WebKit checkout, use external caller-owned build paths:

```sh
python3 Source/WebCore/scarlet/Dependencies/build.py --check
python3 Source/WebCore/scarlet/Dependencies/build.py --plan
python3 Source/WebCore/scarlet/Dependencies/build.py \
  --sdk "$SCARLET_SDK" \
  --build-dir "$SCARLET_DEPENDENCY_BUILD" \
  --prefix "$SCARLET_DEPENDENCY_PREFIX" \
  --jobs 2
```

`--check` and `--plan` read the lock without creating files or fetching sources.
Omit `--prefix` to use `BUILD_DIR/prefix`. Add `--fetch-only` to verify the SDK and
fetch the exact source inputs without compiling dependency libraries. See
`--help` for host compiler and archive-tool overrides.

`--preflight-only` runs the actual native SDK macros, archive and C++ header
checks before source fetching. The generated `-fsyntax-only` probe includes
`<filesystem>`, `<charconv>` and `<complex>` and instantiates the real float and
double complex types. It catches SDK declaration gaps such as ICU's required
parsing of libc++ `<complex>` before a full dependency build starts. This checks
header availability and type contracts, not runtime long-double math support.
The C11 probe also requires the real SDK `<uchar.h>` and verifies the native
16-bit and 32-bit character type widths needed by libpsl's ICU C headers. It
does not claim multibyte conversion function support.

Run the recipe boundary tests with
`python3 Source/WebCore/scarlet/Dependencies/test_recipe.py`. These cover missing
or moving pins, patch replay, source/cache isolation, unsupported paths, SDK
provenance and malformed/foreign archives. Synthetic archive fixtures test the
parser only; they do not provide native compilation or execution evidence.

Source caches, release downloads, generated build configuration, ICU payload,
install staging, logs and audit records stay below `--build-dir`. Both the
build directory and install prefix must be outside WebKit and separate from
the SDK. Paths use ASCII letters, digits and `/_.+@-`; whitespace and shell
metacharacters are rejected explicitly. No personal host path is stored in the
source recipe. A build directory
cannot be reused with a different lock, recipe, SDK or prefix. Existing dirty
Git sources and changed extracted source caches are rejected; use a fresh build
directory instead of replaying local modifications. Upstream builds compile
from separate complete source copies under `build-sources`, so normal upstream
generation or source renames (such as zlib's `zconf.h`) leave the verified fetch
cache unchanged. Those work copies are checkpointed after each upstream build;
later manual changes are rejected when the directory is reused.

The resulting prefix contains native archives, headers and pkg-config files.
Supply it through `CMAKE_PREFIX_PATH` when configuring `PORT=Scarlet` with the
SDK toolchain. WebCore's runtime and shared-memory inputs continue to come from
the SDK. Fontconfig configuration is installed through external DESTDIR
staging; its guest `/etc` and `/share` paths never install directly onto the
host. The guest must supply its licensed native fonts under `/share/fonts`.

The CMake dependency profile supports static archives only. zlib's release
unconditionally declares both shared and static targets, so the profile uses
CMake's supported static conversion and gives the second static archive a
distinct output name. Both are genuine compiled archives; no dynamic loader
or replacement source implementation is introduced.

## Native profile and evidence

ICU uses its published Scarlet configure/static make fragment. The host build
generates only `config/icucross.mk` and `config/icucross.inc`; target `common`
and `i18n` are compiled with the native drivers. ICU's prebuilt little-endian
data is not tracked in the Git source. The recipe downloads the official ICU
78.3 source release, verifies the whole archive, extracts only its exact
`icudt78l.dat` member into `BUILD_DIR/inputs`, verifies that payload separately
and embeds it as the real AArch64 `icudt78_dat` object. It does not copy data or
code over the fork checkout, and does not invoke host pkgdata.

Cairo, Pixman, FreeType and HarfBuzz use CPU painting/shaping without host
graphics systems. Expat and Fontconfig use real native entropy. The zlib and
PNG package names may use aliases to their actual built static archives.
SQLite retains native POSIX-facing VFS and pthreads; mmap, dynamic extensions
and disk-backed temporary stores are disabled. Persistent database behavior
still requires verified native file locking. Curl permits file transfers with
network protocols, proxies, TLS and threaded resolver disabled. OpenSSL builds
genuine offline certificate/crypto value support without sockets, shared
objects, modules, assembly, async, POSIX I/O, stdio or apps. These profiles do
not establish live networking support.

After a complete build, `dependency-build.json` records each required archive's
SHA-256, length and native object count. Every non-index archive member must
be a real ELF64 little-endian AArch64 relocatable object; empty, thin, host and
bitcode archives are rejected. This is source/build/architecture evidence.
Actual ABI behavior, WebCore rendering and native process teardown still
require independent Scarlet guest tests. The report explicitly leaves
`guest_execution_verified` false.

On 2026-10-13 this recipe's 16 pinned dependencies produced all 21 required
static archives through actual native configuration, compilation and install.
Every archive member passed the AArch64 ELF audit, and all 18 recipe boundary
tests passed. No compiled library was imported from the earlier lab prefixes.

Twenty archives were compiled with the formal SDK at Scarlet
`70ecf67c979ba2ab19caa029963c37a9e49fcdf4`. libpsl was configured and compiled
with the final SDK pin above after Scarlet added its C11 `uchar.h`. The SDK's
seven native archives, all 1,723 existing public headers and the native C/C++
driver files were byte-identical across these SDKs; the new header supplies the
character types needed by ICU's C headers. The earlier twenty build outputs
were retained with their actual SDK provenance. The final aggregate audit
records each archive's compilation SDK and the SDK compatibility comparison.

The native dependency build is complete. The dependency audit itself does not
establish guest execution or WebCore rendering; those require the independent
native integration tests described above.

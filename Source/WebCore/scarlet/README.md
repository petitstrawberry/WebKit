# Scarlet native CPU port

This branch carries the Scarlet port in Git. It is based on upstream WebKit
`7024b36074b0584a72f9d91406ed3b13548fb4e0`; building it does not require applying a
local patch. The initial profile is AArch64, the JavaScriptCore C-loop
interpreter, Cairo/FreeType/HarfBuzz CPU painting and native Scarlet thread GC.
JIT, Wasm, accelerated compositing, media, accessibility services and a WPE
browser process are disabled. Unsupported service entry points fail explicitly.

The SDK and native dependency libraries are build inputs. A host compiler, SDK
or host library must not replace a Scarlet target dependency. Use a Scarlet C/C++
driver providing the real CRT, libc, libc++, ABI support, kernel thread/syscall
interfaces and native link closure. Supply native Cairo, Pixman, Fontconfig,
FreeType, HarfBuzz, ICU, XML, SQLite, JPEG, PNG, zlib, WebP, OpenSSL, Curl, Expat
and PSL libraries through the native package prefixes.

Configure from the repository root, with SDK/prefix variables set by your build
environment:

```sh
cmake -S . -B build-scarlet -G Ninja \
  -DPORT=Scarlet \
  -DCMAKE_TOOLCHAIN_FILE="$SCARLET_SDK/share/scarlet/cmake/aarch64-scarlet.cmake" \
  -DSCARLET_DEPENDENCY_PREFIXES="$SCARLET_DEPENDENCY_PREFIX" \
  -DSCARLET_NATIVE_MEMORY_LIBRARY="$SCARLET_SDK/lib/libscarlet_webcore_native_memory.a" \
  -DSCARLET_BUILD_RENDERER=ON \
  -DSCARLET_NATIVE_LIBRARY_ROOTS="$SCARLET_SDK;$SCARLET_DEPENDENCY_PREFIX"
cmake --build build-scarlet --target ScarletWebCoreRender --parallel 3
```

`SCARLET_NATIVE_MEMORY_LIBRARY` is the native Scarlet shared-memory ABI used by
WebCore. The installed toolchain supplies the formal `libscarlet_cxx_support.a`
runtime and `libscarlet_c_startup.a` finalization adapter. Runtime inputs are
explicit archive paths, and
its linker validates every imported/static dependency against the trusted
native prefixes. Static engine object subtargets are preserved, and real
archives are linked in an LLD rescan group. No interpreter/dynamic loader or
host library is used by the native executable.

The optional `DirectRenderer` target constructs an actual Page, parses original
HTML, performs style/layout and paints through WebCore into Cairo BGRA/PNG. It
also exports DOM/render-tree/geometry evidence and uses an exclusive output
transaction with JSON committed last. Normal rendering keeps scripts sandboxed
and disabled. The separate `ScarletWebCoreScriptCounterCheck` positive control
allows an actual inline script to mutate the DOM and checks the real C-loop
entry counter; it is not built by default.

An explicit same-origin HTTPS snapshot profile delivers exact indexed native
resource bytes through real SubresourceLoader/CachedResource completion. It
preserves original HTML/CSS/font/image bytes and the original document origin,
enables real image loading, and waits for native RunLoop/style/font/resource
settlement. Transport success, image decoding, selected remote font glyphs and
pixel painting are independently verified. This profile is an offline byte
transport, not a live HTTP implementation.

Run `DirectRenderer/verify.py --help` for the strict artifact protocol. Run the
synthetic verifier tests as documented in `DirectRenderer/tests/README.md`.
Those host tests are protocol checks; native execution, native child status,
source provenance and window presentation require independent guest evidence.

The persistent `ScarletWebView` C ABI is selected with
`-DSCARLET_BUILD_EMBEDDING=ON`. It owns actual WebCore Page/frame/navigation,
RunLoop, event delivery and Cairo painting on one engine thread. Its HTTP
request/completion API delegates byte transport to the embedding host while
WebCore retains loader, redirect and security checks. The generated
`build-scarlet/ScarletWebView.link` contains one raw LLD argument per line for
the final Rust executable, without a CRT or `main` object. The closure selects
a fixed-address ELF with `--no-pie`, matching its native dependency archives.
The engine library does not supply a process entry point or interpreter; the
embedding frontend owns startup and any native Scarlet loader integration.
Installation places
the public header in `include`, the static engine archives in `lib`, and a
manifest with installed engine paths in `share/scarlet/ScarletWebView.link`.
The native SDK and dependency producer prefixes remain explicit inputs.
The embedding is experimental. Infinite-script shutdown failed in earlier
native runtime checks and still requires revalidation with the current build;
cross-thread termination is not yet a proven cancellation path. A successful
engine or frontend link does not establish live-site GUI compatibility.
Scarlet defaults to full static archives, and the embedding requires
`USE_THIN_ARCHIVES=OFF` so installed engine libraries retain their actual objects.
The installed manifest retains their absolute archive paths; it is developer
link metadata rather than a standalone SDK bundle. After relocating those
producer prefixes, reconfigure and reinstall to regenerate the manifest.

No logs, generated headers, build caches, dependency binaries or personal host
paths are stored in this port source tree.

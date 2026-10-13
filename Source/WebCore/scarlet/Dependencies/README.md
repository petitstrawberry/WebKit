# Scarlet native dependencies

`CMakeLists.txt` prepares the pinned sources with CMake FetchContent and builds
the real static AArch64 libraries using `build.py` and the installed SDK runtime.
The dependency commits and release checksums live in `dependencies.lock.json`.
Myrica calls this producer from its Cargo build directory; neither the Scarlet
flake nor bundle scripts manage these sources or compile the engine.

`build.py` configures, compiles and installs supplied sources. It does not fetch
repositories, maintain hashes/cache state, replay local patches, or build libc.
Host ICU configuration supplies cross-build metadata; the target common/i18n
libraries and official ICU data are native AArch64. Fontconfig and SQLite each
have one producer here. Curl remains file-only; live HTTP/TLS belongs to the
embedding client. A Rust embedding supplies the native shared-memory C ABI; a
standalone renderer supplies its real bridge archive.

The completed build audits every archive member as an AArch64 ELF relocatable.
`python3 test_recipe.py` checks malformed and foreign archive rejection. Building
and archive validation do not establish guest behavior or GPU rendering.

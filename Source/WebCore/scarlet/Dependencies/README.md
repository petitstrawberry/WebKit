# Scarlet native dependencies

`default.nix` owns the static AArch64 dependency package for the Scarlet WebKit
port. It reads the immutable Git commits and official release checksums from
`dependencies.lock.json`. Nix retrieves the sources, manages build outputs and
caching, and supplies the installed scarlet-sdk C/C++ runtime.

`build.py` only configures, compiles and installs the supplied upstream sources.
It does not fetch repositories, calculate source hashes, maintain source-cache
state, replay patches, or build libc/libc++ itself. Upstream generated files stay
in ordinary writable build copies inside the Nix build directory. Host ICU
configuration produces cross-build metadata only; target libraries use native
AArch64 compilers and Scarlet headers. ICU data comes from its pinned official
release archive.

The recipe uses real static Cairo, shaping/font, image, database and value-level
crypto libraries. Curl's profile is file-only; live HTTP/TLS requests remain the
embedding client's responsibility. SQLite retains the Scarlet flock VFS.
Fontconfig configuration is staged into the prefix, never installed into host
`/etc` or `/share`. The runtime and WebCore shared-memory bridge are separate
owned dependencies.

The completed build audits every archive member as an AArch64 ELF relocatable
object. `python3 test_recipe.py` checks malformed and foreign archive rejection.
Neither archive parsing nor a successful cross build establishes guest behavior
or GPU rendering; those are verified through the normal Scarlet applications.

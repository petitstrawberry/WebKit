include_guard(DIRECTORY)

# Included at the end of Source/WebCore/CMakeLists.txt. This is never a
# standalone project: all generated headers/features must be the engine's.
if (NOT PORT STREQUAL "Scarlet" OR NOT TARGET WebCore)
    message(FATAL_ERROR "ScarletStaticLink.cmake requires the configured Scarlet WebCore target")
endif ()
if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Scarlet")
    message(FATAL_ERROR "The Scarlet port requires the native Scarlet Clang/LLD toolchain")
endif ()
if (NOT SCARLET_NATIVE_LIBRARY_ROOTS)
    message(FATAL_ERROR "Set SCARLET_NATIVE_LIBRARY_ROOTS to the trusted Scarlet runtime/dependency prefixes")
endif ()

# Imported targets from WebKit's FindCairo/FindHarfBuzz omit private static
# dependencies. Keep every real native archive in one rescan group, including
# the memory/runtime bridges. The compiler driver supplies the Scarlet CRT and
# C++/C runtimes afterwards; it never searches host headers or host libc.
set(_scarlet_engine_targets WebCore PAL JavaScriptCore WTF)
if (TARGET bmalloc)
    list(APPEND _scarlet_engine_targets bmalloc)
endif ()
set(_scarlet_dependency_targets
    Cairo::Cairo Fontconfig::Fontconfig Freetype::Freetype
    HarfBuzz::HarfBuzz HarfBuzz::ICU ICU::i18n ICU::uc ICU::data
    JPEG::JPEG PNG::PNG ZLIB::ZLIB WebP::libwebp WebP::demux
    LibXml2::LibXml2 SQLite3::SQLite3 OpenSSL::SSL OpenSSL::Crypto
    CURL::libcurl LibPSL::LibPSL
)
if (USE_WOFF2)
    list(APPEND _scarlet_dependency_targets WOFF2::common WOFF2::dec Brotli::common Brotli::dec)
endif ()

function(_scarlet_check_archive _archive)
    if (NOT IS_ABSOLUTE "${_archive}" OR NOT EXISTS "${_archive}" OR NOT _archive MATCHES "\\.a$")
        message(FATAL_ERROR "Missing real native static port dependency: ${_archive}")
    endif ()
    file(REAL_PATH "${_archive}" _real_archive)
    set(_inside_native_root FALSE)
    foreach (_root IN LISTS SCARLET_NATIVE_LIBRARY_ROOTS)
        if (NOT IS_ABSOLUTE "${_root}" OR NOT IS_DIRECTORY "${_root}")
            message(FATAL_ERROR "Invalid trusted native library prefix: ${_root}")
        endif ()
        file(REAL_PATH "${_root}" _native_root)
        cmake_path(IS_PREFIX _native_root "${_real_archive}" NORMALIZE _inside)
        if (_inside)
            set(_inside_native_root TRUE)
            break()
        endif ()
    endforeach ()
    if (NOT _inside_native_root)
        message(FATAL_ERROR "Port dependency escaped the trusted native prefixes: ${_real_archive}")
    endif ()
endfunction()

foreach (_target IN LISTS _scarlet_engine_targets)
    if (NOT TARGET "${_target}")
        message(FATAL_ERROR "Missing configured native engine target: ${_target}")
    endif ()
    get_target_property(_type "${_target}" TYPE)
    if (NOT _type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Scarlet requires a native static engine target: ${_target} (${_type})")
    endif ()
endforeach ()
foreach (_target IN LISTS _scarlet_dependency_targets)
    if (NOT TARGET "${_target}")
        message(FATAL_ERROR "Missing native imported dependency: ${_target}")
    endif ()
    # Resolve aliases (SQLite uses one in this exact upstream revision).
    get_target_property(_alias "${_target}" ALIASED_TARGET)
    if (_alias)
        set(_imported_target "${_alias}")
    else ()
        set(_imported_target "${_target}")
    endif ()
    get_target_property(_archive "${_imported_target}" IMPORTED_LOCATION)
    if (NOT _archive)
        get_target_property(_archive "${_imported_target}" IMPORTED_LOCATION_RELEASE)
    endif ()
    if (NOT _archive)
        get_target_property(_archive "${_imported_target}" IMPORTED_LOCATION_NOCONFIG)
    endif ()
    _scarlet_check_archive("${_archive}")
endforeach ()

# These dependencies have no upstream imported target in this configuration.
# NO_DEFAULT_PATH is deliberate: a host pixman/expat archive cannot enter.
find_library(_scarlet_pixman NAMES pixman-1
    PATHS ${SCARLET_NATIVE_LIBRARY_ROOTS} PATH_SUFFIXES lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(_scarlet_expat NAMES expat
    PATHS ${SCARLET_NATIVE_LIBRARY_ROOTS} PATH_SUFFIXES lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(_scarlet_private_archives
    "${_scarlet_pixman}"
    "${_scarlet_expat}"
    "${SCARLET_CXX_RUNTIME_SUPPORT_LIBRARY}"
)
# Rust embeddings supply this C ABI from their own native Rust dependency.
# A standalone renderer still supplies the actual native bridge archive.
if (SCARLET_NATIVE_MEMORY_LIBRARY)
    list(APPEND _scarlet_private_archives "${SCARLET_NATIVE_MEMORY_LIBRARY}")
elseif (SCARLET_BUILD_RENDERER)
    message(FATAL_ERROR "A standalone Scarlet renderer requires its native memory bridge")
endif ()
list(REMOVE_DUPLICATES _scarlet_private_archives)
foreach (_archive IN LISTS _scarlet_private_archives)
    _scarlet_check_archive("${_archive}")
endforeach ()

# Scarlet is an ELF target with the real LLD driver. CMake has no built-in
# Scarlet platform file declaring RESCAN; declare its actual LLD syntax here.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")


# Use physical engine archives rather than their cyclic PostBuild link aliases.
set(_scarlet_engine_archives)
foreach (_engine IN LISTS _scarlet_engine_targets)
    list(APPEND _scarlet_engine_archives "$<TARGET_FILE:${_engine}>")
endforeach ()
set(_scarlet_static_group ${_scarlet_engine_archives} ${_scarlet_dependency_targets} ${_scarlet_private_archives})
list(JOIN _scarlet_static_group "," _scarlet_static_group_argument)
if (NOT TARGET Threads::Threads)
    message(FATAL_ERROR "Missing configured native Threads usage target")
endif ()

function(scarlet_attach_engine _consumer)
    foreach (_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        set_property(TARGET "${_consumer}" APPEND PROPERTY ${_property}
            "$<TARGET_PROPERTY:WebCore,${_property}>")
    endforeach ()
    add_dependencies("${_consumer}" WebCoreGeneratedSources)
    foreach (_engine IN LISTS _scarlet_engine_targets)
        add_dependencies("${_consumer}" "${_engine}")
        if (TARGET "${_engine}_PostBuild")
            add_dependencies("${_consumer}" "${_engine}_PostBuild")
            target_include_directories("${_consumer}" PRIVATE
                "$<TARGET_PROPERTY:${_engine}_PostBuild,INTERFACE_INCLUDE_DIRECTORIES>")
            target_compile_definitions("${_consumer}" PRIVATE
                "$<TARGET_PROPERTY:${_engine}_PostBuild,INTERFACE_COMPILE_DEFINITIONS>")
        endif ()
        # WebKit intentionally leaves these PCH subtarget objects out of static
        # archives. Embed the real objects without following link aliases.
        get_target_property(_interface "${_engine}" INTERFACE_LINK_LIBRARIES)
        foreach (_item IN LISTS _interface)
            if (_item MATCHES "\\$<TARGET_OBJECTS:")
                target_sources("${_consumer}" PRIVATE "${_item}")
            endif ()
        endforeach ()
    endforeach ()
endfunction()

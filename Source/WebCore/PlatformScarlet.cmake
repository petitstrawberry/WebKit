if (USE_SKIA)
    include(platform/Skia.cmake)
    list(REMOVE_ITEM WebCore_UNIFIED_SOURCE_LIST_FILES "platform/SourcesSkia.txt")
    list(APPEND WebCore_UNIFIED_SOURCE_LIST_FILES "platform/scarlet/SourcesSkiaScarlet.txt")
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        ${WEBCORE_DIR}/platform/graphics/scarlet/vulkan
        ${WEBCORE_DIR}/platform/graphics/freetype)
    list(APPEND WebCore_SOURCES
        platform/graphics/scarlet/vulkan/PlatformDisplayScarletSkia.cpp
        platform/graphics/scarlet/vulkan/SkiaUtilitiesScarlet.cpp
        platform/graphics/freetype/RefPtrFontconfig.cpp)
    list(APPEND WebCore_LIBRARIES Fontconfig::Fontconfig Freetype::Freetype ScarletSkiaVulkan)
else ()
    include(platform/Cairo.cmake)
    include(platform/FreeType.cmake)
endif ()
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)
include(platform/Curl.cmake)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    ${WEBCORE_DIR}/platform/generic
    ${WEBCORE_DIR}/platform/scarlet
    ${WEBCORE_DIR}/platform/network/scarlet
    ${WEBCORE_DIR}/platform/graphics/scarlet
    ${WEBCORE_DIR}/rendering/scarlet
)
list(APPEND WebCore_LIBRARIES OpenSSL::Crypto)
if (USE_WOFF2)
    list(APPEND WebCore_LIBRARIES WOFF2::common WOFF2::dec Brotli::common Brotli::dec)
endif ()
list(APPEND WebCore_SOURCES
    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp
    platform/text/Hyphenation.cpp
    platform/text/LocaleICU.cpp
    platform/unix/LoggingUnix.cpp

    # Native CPU port interfaces. Unavailable platform services fail explicitly.
    accessibility/scarlet/AXObjectCacheScarlet.cpp
    accessibility/scarlet/AccessibilityObjectScarlet.cpp
    editing/scarlet/EditorScarlet.cpp
    platform/scarlet/CursorScarlet.cpp
    platform/scarlet/MIMETypeRegistryScarlet.cpp
    platform/scarlet/PlatformScreenScarlet.cpp
    platform/scarlet/ScrollbarThemeScarlet.cpp
    platform/scarlet/ThemeScarlet.cpp
    platform/scarlet/UserAgentScarlet.cpp
    platform/scarlet/PasteboardScarlet.cpp
    platform/scarlet/PlatformKeyboardEventScarlet.cpp
    platform/scarlet/SharedMemoryScarlet.cpp
    platform/graphics/scarlet/SystemFontDatabaseScarlet.cpp
    platform/network/scarlet/ResourceHandleScarletOffline.cpp
    platform/network/scarlet/CurlSSLHandleScarlet.cpp
    platform/network/scarlet/NetworkStateNotifierScarlet.cpp
    rendering/scarlet/RenderThemeScarlet.cpp
)

# DNSResolveQueueCurl::resolve currently logs notImplemented without invoking
# its completion. Replace that source with an offline adapter which completes
# immediately with DNSError::CannotResolve; also prevent speculative prefetch.
list(REMOVE_ITEM WebCore_SOURCES platform/network/curl/DNSResolveQueueCurl.cpp)
list(APPEND WebCore_SOURCES platform/network/scarlet/DNSResolveQueueScarletOffline.cpp)

# The retained scene has its own compositor and the standard WebCore scrolling thread.
if (SCARLET_USE_COORDINATED_COMPOSITOR)
    include(platform/CoordinatedGraphics.cmake)
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        ${WEBCORE_DIR}/platform/graphics/texmap)
    list(APPEND WebCore_SOURCES
        platform/graphics/texmap/TextureMapperAnimation.cpp
        platform/graphics/scarlet/vulkan/ScarletLayerTree.cpp
        platform/graphics/scarlet/vulkan/ScarletScrollingCoordinator.cpp
        platform/graphics/skia/SkiaBackingStore.cpp
        platform/graphics/skia/SkiaCompositingLayer.cpp
        platform/graphics/skia/SkiaCompositingLayer3DRenderingContext.cpp
        platform/graphics/skia/SkiaCompositingLayerFilters.cpp
        platform/graphics/skia/SkiaCompositingLayerImageSetBatch.cpp
        platform/graphics/skia/SkiaCompositingLayerOverlapRegions.cpp
        platform/graphics/skia/SkiaGPUAtlas.cpp
        platform/graphics/skia/SkiaPaintingEngine.cpp
        platform/graphics/skia/SkiaReplayAtlas.cpp
        platform/graphics/skia/SkiaReplayCanvas.cpp)
    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/graphics/scarlet/vulkan/ScarletLayerTree.h
        platform/graphics/scarlet/vulkan/ScarletScrollingCoordinator.h)
else ()
    list(APPEND WebCore_SOURCES platform/graphics/scarlet/GraphicsLayerScarlet.cpp)
endif ()
# No EGL, libwpe, or TextureMapper renderer is used.


list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/graphics/scarlet/ScarletPlatformLayerContainer.h
    platform/scarlet/ScarletPlatform.h
    platform/scarlet/ScarletSharedMemoryHandle.h
    platform/scarlet/ThemeScarlet.h
    rendering/scarlet/RenderThemeScarlet.h
)
# Rust embeddings link their native memory C ABI through Cargo. Standalone
# C/C++ consumers provide the actual bridge archive explicitly.
if (SCARLET_NATIVE_MEMORY_LIBRARY OR NOT SCARLET_BUILD_EMBEDDING)
    if (NOT SCARLET_NATIVE_MEMORY_LIBRARY)
        find_library(_scarlet_native_memory NAMES scarlet_webcore_native_memory REQUIRED)
        set(SCARLET_NATIVE_MEMORY_LIBRARY "${_scarlet_native_memory}")
    endif ()
    if (NOT IS_ABSOLUTE "${SCARLET_NATIVE_MEMORY_LIBRARY}" OR NOT EXISTS "${SCARLET_NATIVE_MEMORY_LIBRARY}")
        message(FATAL_ERROR "SCARLET_NATIVE_MEMORY_LIBRARY must name the native Scarlet memory bridge archive")
    endif ()
    if (NOT TARGET Scarlet::NativeMemory)
        add_library(Scarlet::NativeMemory STATIC IMPORTED)
        set_target_properties(Scarlet::NativeMemory PROPERTIES IMPORTED_LOCATION "${SCARLET_NATIVE_MEMORY_LIBRARY}")
    endif ()
    list(APPEND WebCore_LIBRARIES Scarlet::NativeMemory)
endif ()

if (SCARLET_USE_NATIVE_MEDIA)
    list(APPEND WebCore_SOURCES platform/graphics/scarlet/MediaPlayerPrivateScarlet.cpp)
    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/graphics/scarlet/ScarletMedia.h
        platform/graphics/scarlet/MediaSampleScarlet.h
        platform/graphics/scarlet/MediaPlayerPrivateScarlet.h)
    if (ENABLE_MEDIA_SOURCE)
        list(APPEND WebCore_SOURCES
            platform/graphics/scarlet/MediaSourcePrivateScarlet.cpp
            platform/graphics/scarlet/SourceBufferPrivateScarlet.cpp)
        list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
            platform/graphics/scarlet/MediaSourcePrivateScarlet.h
            platform/graphics/scarlet/SourceBufferPrivateScarlet.h)
    endif ()
endif ()

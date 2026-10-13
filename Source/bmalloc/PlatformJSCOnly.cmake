if (APPLE)
    list(APPEND bmalloc_SOURCES
        bmalloc/ProcessCheck.mm
    )

    list(APPEND bmalloc_LIBRARIES
        "-framework Foundation"
        objc
    )
endif ()

# Scarlet uses the explicitly selected system allocator. Keep the framework's
# headers and diagnostics, but do not build unused libpas virtual-memory code.
if (CMAKE_SYSTEM_NAME STREQUAL "Scarlet" AND USE_SYSTEM_MALLOC)
    set(bmalloc_SOURCES
        bmalloc/CryptoRandom.cpp bmalloc/SystemHeap.cpp bmalloc/Environment.cpp
        bmalloc/Gigacage.cpp bmalloc/HeapKind.cpp bmalloc/Logging.cpp
        bmalloc/Mutex.cpp bmalloc/TZoneHeap.cpp bmalloc/TZoneHeapManager.cpp
        bmalloc/VMAllocate.cpp bmalloc/bmalloc.cpp)
    set(bmalloc_C_SOURCES)
endif ()

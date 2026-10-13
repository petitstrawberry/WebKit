include(PlatformJSCOnly.cmake)

if (NOT WTF_CPU_ARM64)
    message(FATAL_ERROR "The native Scarlet thread suspension port requires AArch64")
endif ()

list(APPEND WTF_PUBLIC_HEADERS
    scarlet/NativeThreadGC.h
)

list(APPEND WTF_SOURCES
    scarlet/NativeThreadGC.cpp
    scarlet/NativeThreadCall.S
)

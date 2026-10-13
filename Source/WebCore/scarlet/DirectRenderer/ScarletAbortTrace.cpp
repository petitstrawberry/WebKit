#include "config.h"

#include <cstdio>

#if !PLATFORM(SCARLET)
#error "The native abort trace is specific to Scarlet"
#endif

extern "C" [[noreturn]] void __real_abort();

// Keep the real fatal path intact. The native libc abort has no diagnostic,
// so record its immediate caller before it performs the original exit.
extern "C" [[noreturn]] void __wrap_abort()
{
    void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
    std::printf("SCARLET_NATIVE_ABORT caller=%p\n", caller);
    __real_abort();
}

// This exact upstream C++ signature is weak/hidden in the static engine.
// LLD wraps its named calls, letting us preserve all assertion arguments and
// the original fatal path without guessing at an older stack frame.
extern "C" [[noreturn]] void scarletRealWtfCrashWithInfo(int, const char*, const char*)
    asm("__real__Z16WTFCrashWithInfoiPKcS0_");
extern "C" [[noreturn]] void scarletWrapWtfCrashWithInfo(int, const char*, const char*)
    asm("__wrap__Z16WTFCrashWithInfoiPKcS0_");

extern "C" [[noreturn]] void scarletWrapWtfCrashWithInfo(int line, const char* file, const char* function)
{
    std::printf("SCARLET_WTF_CRASH line=%d file=%s function=%s\n", line, file, function);
    scarletRealWtfCrashWithInfo(line, file, function);
}

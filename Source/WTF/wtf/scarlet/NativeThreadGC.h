#pragma once

#include <wtf/Platform.h>

#if !OS(SCARLET) || !CPU(ARM64)
#error "NativeThreadGC requires Scarlet on AArch64"
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The general-purpose register order matches the native event context. SIMD
// registers and floating-point control/status are saved by NativeThreadCall.S.
struct ScarletGCRegisters {
    uint64_t gpr[31];
    uint64_t pc;
    uint64_t sp;
    uint64_t nzcv;
    uint64_t simd[64];
    uint64_t fpcr;
    uint64_t fpsr;
};

unsigned long scarlet_native_call(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
int scarlet_gc_register_current(void);
// The exiting thread calls this only after leaving WTF ThreadGroups and while
// holding ThreadSuspendLocker. This excludes scanners and protects raw records.
int scarlet_gc_unregister_current(void);
int scarlet_gc_suspend(pthread_t, struct ScarletGCRegisters**);
int scarlet_gc_resume(pthread_t);
int scarlet_gc_stack_bounds(void**, void**);
void scarlet_gc_print_statistics(void);

#ifdef __cplusplus
}
#endif
